export module vrento:vulkan.renderer;
export import :vulkan.pass.compiled;
export import :vulkan.pass.factory;
import rstd;
using rstd::sync::Arc;

export namespace vrento::vulkan
{
struct RendererStatistics {
    GraphSerial graph_compiles {}, native_compiles {}, pending_polls {};
    GraphSerial compile_requests {}, graph_reuses {}, native_completed {}, compile_failures {},
        compile_cancellations {};
    ExecutionStatistics execution;
};
enum class CompileReason
{
    Initial,
    Reused,
    Scope,
    Graph,
    Description,
    Resources,
    Standalone
};
enum class CompileState
{
    Idle,
    Pending,
    Ready,
    Failed,
    Cancelled
};
enum class CompileKind
{
    Graph,
    Pass
};
struct CompileStatus {
    GraphSerial                 id {};
    CompileState                state { CompileState::Idle };
    Option<GraphExecutionError> error;
    CompileKind                 kind { CompileKind::Graph };
    GraphSerial                 scope {};
    CompileReason               reason { CompileReason::Initial };
};
class Renderer {
    GraphSerial            next_compile_id_ {};
    CompileStatus          status_;
    DeviceSession          session_;
    RendererStatistics     statistics_;
    Arc<ExecutionCounters> execution_ { Arc<ExecutionCounters>::make() };
    struct FactoryEntry {
        rstd::any::TypeId     type;
        Arc<dyn<PassFactory>> factory;
    };
    Vec<FactoryEntry> factories_;
    struct PendingCompile {
        Option<VulkanPassSet> graph;
        Option<GraphInput>    input;
        Vec<PassInput>        standalone;
        auto                  descriptions() const -> slice<PassInput> {
            return input ? input->descriptions() : standalone.as_slice();
        }
        Vec<CompiledGraph::Entry>             entries;
        Option<Arc<GraphicsCompileResources>> resources;
    };
    Option<PendingCompile> pending_;
    Option<CompiledGraph>  compiled_;
    Option<CompiledPass>   compiled_pass_;

public:
    explicit Renderer(DeviceSession session): session_(rstd::move(session)) {}
    auto session() const -> const DeviceSession& { return session_; }
    auto statistics() const -> RendererStatistics {
        auto value      = statistics_;
        value.execution = execution_->snapshot();
        return value;
    }
    auto bind(Arc<dyn<PassFactory>> factory) -> Result<empty, GraphExecutionError> {
        auto type = factory->Type();
        for (const auto& entry : factories_)
            if (entry.type == type) return Err(GraphExecutionError { GraphExecutionStage::Schema });
        factories_.push({ type, rstd::move(factory) });
        return Ok(empty {});
    }
    auto status() const -> CompileStatus { return status_; }
    auto ready() const -> bool {
        return session_.valid() && ! session_.lost() && status_.state == CompileState::Ready &&
               (status_.kind == CompileKind::Graph ? compiled_.is_some()
                                                   : compiled_pass_.is_some());
    }
    auto compile(GraphInput input, Option<Arc<GraphicsCompileResources>> resources = None())
        -> Result<empty, GraphExecutionError> {
        cancel_compile();
        ++statistics_.compile_requests;
        status_ = { ++next_compile_id_,
                    CompileState::Pending,
                    None(),
                    CompileKind::Graph,
                    input.graph().scope() };
        return finish_compile(begin_compile(rstd::move(input), rstd::move(resources)));
    }
    auto compile(fg::FrameGraph graph, Vec<PassInput> descriptions,
                 Option<Arc<GraphicsCompileResources>> resources = None())
        -> Result<empty, GraphExecutionError> {
        pass::GraphResourceMap mapping { .scope = graph.scope() };
        for (const auto& node : graph.passes())
            for (const auto& use : node.uses) mapping.externals.push(fg::ResourceUse(use));
        auto input =
            GraphInput::create(rstd::move(graph), rstd::move(descriptions), rstd::move(mapping));
        if (input.is_err()) {
            cancel_compile();
            ++statistics_.compile_requests;
            status_ = { ++next_compile_id_, CompileState::Pending, None(), CompileKind::Graph };
            return finish_compile(Err(GraphExecutionError { GraphExecutionStage::Schema,
                                                            input.unwrap_err().pass,
                                                            input.unwrap_err().resource,
                                                            VK_SUCCESS,
                                                            None(),
                                                            Some(input.unwrap_err()) }));
        }
        return compile(input.unwrap(), rstd::move(resources));
    }
    auto continue_compile(GraphSerial id) -> Result<empty, GraphExecutionError> {
        if (id != status_.id || status_.state != CompileState::Pending || ! pending_)
            return Err(GraphExecutionError { GraphExecutionStage::Prepare });
        return finish_compile(advance_compile());
    }

private:
    auto finish_compile(Result<empty, GraphExecutionError> result)
        -> Result<empty, GraphExecutionError> {
        if (result.is_ok()) {
            status_.state = CompileState::Ready;
            status_.error = None();
        } else {
            auto error    = result.unwrap_err();
            status_.state = error.stage == GraphExecutionStage::Pending ? CompileState::Pending
                                                                        : CompileState::Failed;
            status_.error = Some(error);
            if (status_.state == CompileState::Failed) {
                ++statistics_.compile_failures;
                pending_ = None();
            }
        }
        return result;
    }
    auto begin_compile(GraphInput input, Option<Arc<GraphicsCompileResources>> resources = None())
        -> Result<empty, GraphExecutionError> {
        auto graph        = input.graph();
        auto descriptions = input.descriptions();
        if (! session_.valid() || session_.lost())
            return Err(GraphExecutionError { GraphExecutionStage::Device });
        for (usize i {}; i < descriptions.len(); ++i) {
            const auto& input = descriptions[i];
            if (input.scope() != graph.scope() || ! input.revision() ||
                ! input.execution().id.value)
                return Err(
                    GraphExecutionError { GraphExecutionStage::Schema, input.execution().id });
            for (usize j {}; j < i; ++j)
                if (descriptions[j].execution().id == input.execution().id)
                    return Err(
                        GraphExecutionError { GraphExecutionStage::Schema, input.execution().id });
        }
        if (compiled_) {
            const auto& previous = *compiled_->data_;
            if (graph.scope() != previous.graph.scope())
                status_.reason = CompileReason::Scope;
            else {
                auto current = fg::encode_frame_graph(graph);
                auto cached  = fg::encode_frame_graph(previous.graph.frame_graph());
                if (current.is_err() || cached.is_err() || *current != *cached)
                    status_.reason = CompileReason::Graph;
                else {
                    bool same = descriptions.len() == previous.input.descriptions().len();
                    if (same)
                        for (const auto& description : descriptions) {
                            bool found {};
                            for (const auto& cached : previous.input.descriptions())
                                if (cached.execution().id == description.execution().id)
                                    found = description.same_input(cached);
                            same &= found;
                        }
                    if (! same)
                        status_.reason = CompileReason::Description;
                    else if (! input.resources().same_bindings(previous.input.resources()) ||
                             resources.is_some() != previous.resources.is_some() ||
                             (resources && ! Arc<GraphicsCompileResources>::ptr_eq(
                                               *resources, *previous.resources)))
                        status_.reason = CompileReason::Resources;
                    else {
                        status_.reason = CompileReason::Reused;
                        ++statistics_.graph_reuses;
                        return Ok(empty {});
                    }
                }
            }
        }
        ++statistics_.graph_compiles;
        Vec<PassExecutionInfo> declarations;
        for (const auto& desc : descriptions) {
            if (desc.scope() != graph.scope() || ! desc.revision())
                return Err(
                    GraphExecutionError { GraphExecutionStage::Schema, desc.execution().id });
            declarations.push(desc.execution().clone());
        }
        auto set = VulkanPassSet::associate(rstd::move(graph), rstd::move(declarations));
        if (set.is_err())
            return Err(GraphExecutionError { GraphExecutionStage::Graph,
                                             set.unwrap_err().pass,
                                             set.unwrap_err().resource,
                                             VK_SUCCESS,
                                             None(),
                                             Some(set.unwrap_err()) });
        for (const auto& desc : descriptions) {
            bool found {};
            for (const auto& entry : factories_) found |= entry.type == desc.type();
            if (! found)
                return Err(
                    GraphExecutionError { GraphExecutionStage::Schema, desc.execution().id });
        }
        pending_ = Some(PendingCompile {
            Some(set.unwrap()), Some(rstd::move(input)), {}, {}, rstd::move(resources) });
        return advance_compile();
    }

public:
    auto compile_pass(PassInput                             description,
                      Option<Arc<GraphicsCompileResources>> resources = None())
        -> Result<CompiledPass, GraphExecutionError> {
        cancel_compile();
        ++statistics_.compile_requests;
        status_ = { ++next_compile_id_, CompileState::Pending, None(),
                    CompileKind::Pass,  description.scope(),   CompileReason::Standalone };
        if (! session_.valid() || session_.lost()) {
            auto failed = finish_compile(Err(GraphExecutionError { GraphExecutionStage::Device }));
            return Err(failed.unwrap_err());
        }
        if (! description.scope() || ! description.revision()) {
            auto failed = finish_compile(Err(
                GraphExecutionError { GraphExecutionStage::Schema, description.execution().id }));
            return Err(failed.unwrap_err());
        }
        Vec<PassInput> descriptions;
        descriptions.push(rstd::move(description));
        pending_ = Some(
            PendingCompile { None(), None(), rstd::move(descriptions), {}, rstd::move(resources) });
        auto result = finish_compile(advance_compile());
        if (result.is_err()) return Err(result.unwrap_err());
        return Ok(CompiledPass(*compiled_pass_));
    }

private:
    auto advance_compile() -> Result<empty, GraphExecutionError> {
        if (! pending_) return Err(GraphExecutionError { GraphExecutionStage::Prepare });
        if (session_.lost()) {
            pending_ = None();
            return Err(
                GraphExecutionError { GraphExecutionStage::Device, {}, {}, VK_ERROR_DEVICE_LOST });
        }
        ++statistics_.pending_polls;
        auto&              pending = *pending_;
        PassCompileContext context { session_, pending.resources.clone() };
        for (usize i(pending.entries.len()); i < pending.descriptions().len(); ++i) {
            const auto&                   desc = pending.descriptions()[i];
            Option<Arc<dyn<VulkanPass>>>  native;
            Option<Arc<dyn<PassFactory>>> factory;
            for (auto& entry : factories_)
                if (entry.type == desc.type()) {
                    ++statistics_.native_compiles;
                    auto result = entry.factory->Compile(desc, context);
                    if (result.is_err()) {
                        auto error = result.unwrap_err();
                        error.pass = desc.execution().id;
                        if (error.stage != GraphExecutionStage::Pending) pending_ = None();
                        return Err(error);
                    }
                    ++statistics_.native_completed;
                    if (result->as_ptr().as_ptr().as_ref().concrete_type_id() !=
                        entry.factory->PassType()) {
                        auto id  = desc.execution().id;
                        pending_ = None();
                        return Err(GraphExecutionError { GraphExecutionStage::Schema, id });
                    }
                    factory = Some(entry.factory.clone());
                    native  = Some(rstd::move(result).unwrap());
                    break;
                }
            if (! native) {
                auto id  = desc.execution().id;
                pending_ = None();
                return Err(GraphExecutionError { GraphExecutionStage::Schema, id });
            }
            pending.entries.push({ desc.execution().id,
                                   desc.revision(),
                                   rstd::move(*native),
                                   rstd::move(*factory) });
        }
        if (pending.graph) {
            auto result = CompiledGraph::publish(rstd::move(*pending.graph),
                                                 session_,
                                                 rstd::move(*pending.input),
                                                 rstd::move(pending.entries),
                                                 pending.resources.clone(),
                                                 execution_.clone());
            if (result.is_err()) return Err(result.unwrap_err());
            compiled_ = Some(result.unwrap());
        } else {
            compiled_pass_.insert(
                CompiledPass(Arc<CompiledPass::Data>::make(session_,
                                                           rstd::move(pending.standalone[usize()]),
                                                           pending.entries[usize()].native.clone(),
                                                           pending.entries[usize()].factory.clone(),
                                                           pending.resources.clone(),
                                                           execution_.clone())));
        }
        pending_ = None();
        return Ok(empty {});
    }

public:
    auto pending() const -> bool { return status_.state == CompileState::Pending; }
    auto compiled() const -> Option<CompiledGraph> {
        return compiled_ ? Some(CompiledGraph(*compiled_)) : None<CompiledGraph>();
    }
    auto compiled_pass() const -> Option<CompiledPass> {
        return compiled_pass_ ? Some(CompiledPass(*compiled_pass_)) : None<CompiledPass>();
    }
    auto cancel_compile(GraphSerial id) -> bool {
        if (id != status_.id || ! pending()) return false;
        pending_ = None();
        ++statistics_.compile_cancellations;
        status_.state = CompileState::Cancelled;
        status_.error = None();
        return true;
    }
    void cancel_compile() { (void)cancel_compile(status_.id); }
    void clear() {
        cancel_compile();
        status_        = {};
        pending_       = None();
        compiled_      = None();
        compiled_pass_ = None();
    }
};

} // namespace vrento::vulkan
