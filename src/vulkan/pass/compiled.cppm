export module vrento:vulkan.pass.compiled;
export import :vulkan.pass.graph_input;
export import :vulkan.pass.resources;
import :vulkan.pass.factory;
import rstd;
using namespace rstd::prelude;
using rstd::collections::HashMap;
using rstd::sync::Arc;

export namespace vrento::vulkan
{
template<typename T>
inline auto try_vulkan_pass(ref<dyn<VulkanPass>> pass) -> Option<ref<T>> {
    if (pass.concrete_type_id() != rstd::any::TypeId::of<T>()) return None();
    return Some(ref<T>::from_raw_parts(static_cast<const T*>(pass.as_raw_ptr())));
}
template<typename T>
struct BoundVulkanPass {
    Arc<dyn<VulkanPass>>       pass;
    Arc<typename T::FrameData> data;
    auto                       Prepare(const GraphPrepareContext& context) const
        -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError> {
        auto concrete = try_vulkan_pass<T>(pass.as_ptr().as_ptr().as_ref());
        if (! concrete) return Err(GraphExecutionError { GraphExecutionStage::Schema });
        return (*concrete)->Instantiate(context, data.clone());
    }
};
struct PassBindingOwner {};
class PassFrameBinding {
    Arc<PassBindingOwner> owner_;
    usize                 slot_;
    PassId                id_;
    GraphSerial           revision_;
    Arc<dyn<VulkanPass>>  pass_, binding_;
    friend class CompiledGraph;
    friend class CompiledPass;
    friend struct NativeFrameBackend;
    friend struct StandaloneFrameBackend;
    PassFrameBinding(Arc<PassBindingOwner> owner, usize slot, PassId id, GraphSerial revision,
                     Arc<dyn<VulkanPass>> pass, Arc<dyn<VulkanPass>> binding)
        : owner_(rstd::move(owner)),
          slot_(slot),
          id_(id),
          revision_(revision),
          pass_(rstd::move(pass)),
          binding_(rstd::move(binding)) {}

public:
    auto id() const -> PassId { return id_; }
    auto revision() const -> GraphSerial { return revision_; }
    auto clone() const -> PassFrameBinding {
        return { owner_.clone(), slot_, id_, revision_, pass_.clone(), binding_.clone() };
    }
};

struct ExecutionStatistics {
    GraphSerial frame_preparations {}, native_instantiations {}, native_pending {},
        native_failures {}, native_records {}, record_failures {};
};
class ExecutionCounters {
    using Counter = rstd::sync::atomic::Atomic<u64>;
    Counter     frames_, instances_, pending_, failures_, records_, record_failures_;
    static void increment(Counter& value) {
        value.fetch_add(u64(1), rstd::sync::atomic::Ordering::Relaxed);
    }

public:
    void frame() { increment(frames_); }
    void instantiate() { increment(instances_); }
    void pending() { increment(pending_); }
    void fail() { increment(failures_); }
    void record() { increment(records_); }
    void record_failed() { increment(record_failures_); }
    auto snapshot() const -> ExecutionStatistics {
        auto read = [](const Counter& value) {
            return value.load(rstd::sync::atomic::Ordering::Relaxed).to_primitive();
        };
        return { read(frames_),   read(instances_), read(pending_),
                 read(failures_), read(records_),   read(record_failures_) };
    }
};

class CompiledPass {
    struct Data {
        DeviceSession                         session;
        PassInput                             description;
        Arc<dyn<VulkanPass>>                  native;
        Arc<dyn<PassFactory>>                 factory;
        Option<Arc<GraphicsCompileResources>> resources;
        Arc<ExecutionCounters>                execution;
        Arc<PassBindingOwner>                 identity { Arc<PassBindingOwner>::make() };
        Data(DeviceSession device, PassInput desc, Arc<dyn<VulkanPass>> implementation,
             Arc<dyn<PassFactory>> factory_owner, Option<Arc<GraphicsCompileResources>> context,
             Arc<ExecutionCounters> counters)
            : session(rstd::move(device)),
              description(rstd::move(desc)),
              native(rstd::move(implementation)),
              factory(rstd::move(factory_owner)),
              resources(rstd::move(context)),
              execution(rstd::move(counters)) {}
    };
    Arc<Data> data_;
    explicit CompiledPass(Arc<Data> data): data_(rstd::move(data)) {}
    friend class Renderer;
    friend struct StandaloneFrameBackend;

public:
    CompiledPass(const CompiledPass& other): data_(other.data_.clone()) {}
    CompiledPass(CompiledPass&&) = default;
    template<typename T>
    auto frame(PassId id, GraphSerial revision, typename T::FrameData input) const
        -> Result<PassFrameBinding, GraphExecutionError> {
        if (data_->description.execution().id != id || data_->description.revision() != revision ||
            ! try_vulkan_pass<T>(data_->native.as_ptr().as_ptr().as_ref()))
            return Err(GraphExecutionError { GraphExecutionStage::Schema, id });
        auto binding = Arc<dyn<VulkanPass>>::make(BoundVulkanPass<T> {
            data_->native.clone(), Arc<typename T::FrameData>::make(rstd::move(input)) });
        return Ok(PassFrameBinding(data_->identity.clone(),
                                   usize(),
                                   id,
                                   revision,
                                   data_->native.clone(),
                                   rstd::move(binding)));
    }
    auto record(const GraphBindings&, Option<PassFrameBinding>, Box<dyn<GraphicsScopeBackend>>,
                texture::Submission&) const -> Result<empty, GraphExecutionError>;
};

class GraphPreparation;
class CompiledGraph {
    struct Entry {
        PassId                id;
        GraphSerial           revision;
        Arc<dyn<VulkanPass>>  native;
        Arc<dyn<PassFactory>> factory;
        usize                 description {};
    };
    struct Data {
        VulkanPassSet                         graph;
        DeviceSession                         session;
        GraphInput                            input;
        Vec<Entry>                            entries;
        Option<Arc<GraphicsCompileResources>> resources;
        Arc<ExecutionCounters>                execution;
        Arc<PassBindingOwner>                 identity { Arc<PassBindingOwner>::make() };
        HashMap<PassId, usize>                indices;
        Arc<Vec<usize>>                       routes;
        Data(VulkanPassSet set, DeviceSession device, GraphInput values, Vec<Entry> passes,
             Option<Arc<GraphicsCompileResources>> context, Arc<ExecutionCounters> counters,
             HashMap<PassId, usize> index, Vec<usize> dispatch)
            : graph(rstd::move(set)),
              session(rstd::move(device)),
              input(rstd::move(values)),
              entries(rstd::move(passes)),
              resources(rstd::move(context)),
              execution(rstd::move(counters)),
              indices(rstd::move(index)),
              routes(Arc<Vec<usize>>::make(rstd::move(dispatch))) {}
    };
    Arc<Data> data_;
    explicit CompiledGraph(Arc<Data> data): data_(rstd::move(data)) {}
    friend class Renderer;
    friend struct NativeFrameBackend;
    static auto publish(VulkanPassSet graph, DeviceSession session, GraphInput input,
                        Vec<Entry> entries, Option<Arc<GraphicsCompileResources>> resources,
                        Arc<ExecutionCounters> execution)
        -> Result<CompiledGraph, GraphExecutionError> {
        auto descriptions = input.descriptions();
        if (entries.len() != descriptions.len() || entries.len() != graph.passes().len())
            return Err(GraphExecutionError { GraphExecutionStage::Schema });
        HashMap<PassId, usize> descriptions_by_id;
        for (usize i {}; i < descriptions.len(); ++i)
            if (descriptions_by_id.insert(descriptions[i].execution().id, i))
                return Err(GraphExecutionError { GraphExecutionStage::Schema,
                                                 descriptions[i].execution().id });
        HashMap<PassId, usize> indices;
        for (usize i {}; i < entries.len(); ++i) {
            auto& entry = entries[i];
            auto  found = descriptions_by_id.get(entry.id);
            if (! found || indices.insert(entry.id, i))
                return Err(GraphExecutionError { GraphExecutionStage::Schema, entry.id });
            const auto& desc = descriptions[**found];
            if (desc.scope() != graph.scope() || desc.revision() != entry.revision ||
                desc.type() != entry.factory->Type() ||
                entry.native.as_ptr().as_ptr().as_ref().concrete_type_id() !=
                    entry.factory->PassType())
                return Err(GraphExecutionError { GraphExecutionStage::Schema, entry.id });
            entry.description = **found;
        }
        auto routes = Vec<usize>::with_capacity(graph.passes().len());
        for (const auto& pass : graph.passes()) {
            auto index = indices.get(pass.id);
            if (! index) return Err(GraphExecutionError { GraphExecutionStage::Schema, pass.id });
            routes.push(usize(**index));
        }
        return Ok(CompiledGraph(Arc<Data>::make(rstd::move(graph),
                                                rstd::move(session),
                                                rstd::move(input),
                                                rstd::move(entries),
                                                rstd::move(resources),
                                                rstd::move(execution),
                                                rstd::move(indices),
                                                rstd::move(routes))));
    }

public:
    CompiledGraph(const CompiledGraph& other): data_(other.data_.clone()) {}
    CompiledGraph(CompiledGraph&&)                    = default;
    auto operator=(CompiledGraph&&) -> CompiledGraph& = default;
    auto description() const -> const VulkanPassSet& { return data_->graph; }
    auto description(PassId id) const -> Option<ref<PassInput>> {
        auto index = data_->indices.get(id);
        if (! index) return None();
        return Some(ref<PassInput>::from_raw_parts(
            &data_->input.descriptions()[data_->entries[**index].description]));
    }
    auto input() const -> const GraphInput& { return data_->input; }
    auto descriptions() const -> slice<PassInput> { return data_->input.descriptions(); }
    auto session() const -> const DeviceSession& { return data_->session; }
    template<typename T>
    auto frame(PassId id, GraphSerial revision, typename T::FrameData input) const
        -> Result<PassFrameBinding, GraphExecutionError> {
        auto index = data_->indices.get(id);
        if (! index) return Err(GraphExecutionError { GraphExecutionStage::Schema, id });
        const auto& entry = data_->entries[**index];
        if (entry.revision != revision ||
            ! try_vulkan_pass<T>(entry.native.as_ptr().as_ptr().as_ref()))
            return Err(GraphExecutionError { GraphExecutionStage::Schema, id });
        auto binding = Arc<dyn<VulkanPass>>::make(BoundVulkanPass<T> {
            entry.native.clone(), Arc<typename T::FrameData>::make(rstd::move(input)) });
        return Ok(PassFrameBinding(data_->identity.clone(),
                                   **index,
                                   id,
                                   revision,
                                   entry.native.clone(),
                                   rstd::move(binding)));
    }
    auto prepare_frame(const GraphBindings&, slice<PassFrameBinding>,
                       Box<dyn<GraphicsScopeBackend>>, bool merge_scopes = true,
                       texture::Submission* work = nullptr) const
        -> Result<GraphPreparation, GraphExecutionError>;
    auto instantiate(const GraphBindings&, slice<PassFrameBinding>, Box<dyn<GraphicsScopeBackend>>,
                     bool merge_scopes = true, texture::Submission* work = nullptr) const
        -> Result<PreparedGraphProgram, GraphExecutionError>;
};

template<typename Owner>
struct RetainedNativePass {
    Arc<dyn<VulkanPass>> commands;
    Arc<Owner>           owner;
    Arc<dyn<VulkanPass>> selected;
    auto Record(GraphRecordContext& context) const -> Result<empty, GraphExecutionError> {
        owner->execution->record();
        auto result = commands->Record(context);
        if (result.is_err()) owner->execution->record_failed();
        return result;
    }
};
template<typename Owner>
inline auto prepare_native(const Arc<Owner>& owner, PassId id, Arc<dyn<VulkanPass>> selected,
                           const GraphPrepareContext& context)
    -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError> {
    owner->execution->instantiate();
    auto prepared = selected->Prepare(context);
    if (prepared.is_err()) {
        auto error = prepared.unwrap_err();
        if (error.stage == GraphExecutionStage::Pending)
            owner->execution->pending();
        else
            owner->execution->fail();
        error.pass = id;
        return Err(error);
    }
    return Ok(Arc<dyn<VulkanPass>>::make(
        RetainedNativePass<Owner> { prepared.unwrap(), owner.clone(), rstd::move(selected) }));
}
struct NativeFrameBackend {
    Arc<CompiledGraph::Data>       owner;
    Vec<Option<PassFrameBinding>>  inputs;
    Box<dyn<GraphicsScopeBackend>> scopes;
    auto                           resolve_passes(const VulkanPassSet& graph, slice<GraphIndex>)
        -> Result<Arc<Vec<usize>>, GraphExecutionError> {
        if (! graph.same_set(owner->graph))
            return Err(GraphExecutionError { GraphExecutionStage::Schema });
        return Ok(owner->routes.clone());
    }
    auto prepare_scope(const GraphicsDescription& description, const GraphPrepareContext& context)
        -> Result<Arc<dyn<PreparedGraphicsScope>>, GraphExecutionError> {
        return scopes->Prepare(description, context);
    }
    auto prepare_at(usize slot, const PassExecutionInfo& pass, const GraphPrepareContext& context)
        -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError> {
        if (slot >= owner->entries.len() || owner->entries[slot].id != pass.id)
            return Err(GraphExecutionError { GraphExecutionStage::Schema, pass.id });
        const auto& input = inputs[slot];
        auto selected     = input ? input->binding_.clone() : owner->entries[slot].native.clone();
        return prepare_native(owner, pass.id, rstd::move(selected), context);
    }
};
struct StandaloneFrameBackend {
    Arc<CompiledPass::Data>        owner;
    Option<PassFrameBinding>       input;
    Box<dyn<GraphicsScopeBackend>> scopes;
    auto prepare_scope(const GraphicsDescription& description, const GraphPrepareContext& context)
        -> Result<Arc<dyn<PreparedGraphicsScope>>, GraphExecutionError> {
        return scopes->Prepare(description, context);
    }
    auto prepare(const PassExecutionInfo& pass, const GraphPrepareContext& context)
        -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError> {
        if (pass.id != owner->description.execution().id)
            return Err(GraphExecutionError { GraphExecutionStage::Schema, pass.id });
        auto selected = input ? input->binding_.clone() : owner->native.clone();
        return prepare_native(owner, pass.id, rstd::move(selected), context);
    }
};
class GraphPreparation {
    PreparedGraphProgram        program_;
    NativeFrameBackend          backend_;
    Option<GraphExecutionError> failure_;
    friend class CompiledGraph;
    GraphPreparation(PreparedGraphProgram program, NativeFrameBackend backend)
        : program_(rstd::move(program)), backend_(rstd::move(backend)) {}

public:
    GraphPreparation(const GraphPreparation&) = delete;
    GraphPreparation(GraphPreparation&&)      = default;
    auto advance() -> Result<empty, GraphExecutionError> {
        if (failure_) return Err(*failure_);
        auto result = program_.continue_prepare(backend_);
        if (result.is_err() && result.as_ref().unwrap_err().stage != GraphExecutionStage::Pending)
            failure_ = Some(GraphExecutionError(result.as_ref().unwrap_err()));
        return result;
    }
    auto ready() const -> bool { return ! failure_ && program_.ready(); }
    auto program() const -> Option<PreparedGraphProgram> {
        return ready() ? Some(PreparedGraphProgram(program_)) : None<PreparedGraphProgram>();
    }
};

auto CompiledPass::record(const GraphBindings& bindings, Option<PassFrameBinding> input,
                          Box<dyn<GraphicsScopeBackend>> scopes, texture::Submission& work) const
    -> Result<empty, GraphExecutionError> {
    data_->execution->frame();
    const auto& desc = data_->description;
    const auto& pass = desc.execution();
    if (desc.scope() != bindings.scope() ||
        (input &&
         (! Arc<PassBindingOwner>::ptr_eq(input->owner_, data_->identity) ||
          input->slot_ != usize() || input->id_ != pass.id || input->revision_ != desc.revision() ||
          ! Arc<dyn<VulkanPass>>::ptr_eq(input->pass_, data_->native))))
        return Err(GraphExecutionError { GraphExecutionStage::Schema, pass.id });
    StandaloneFrameBackend backend { data_.clone(), rstd::move(input), rstd::move(scopes) };
    auto                   frozen = bindings.freeze_pass(pass, data_->session, work);
    if (frozen.is_err()) return Err(frozen.unwrap_err());
    return record_vulkan_pass(data_->session, pass.clone(), frozen.unwrap(), backend, work);
}
auto CompiledGraph::prepare_frame(const GraphBindings& bindings, slice<PassFrameBinding> inputs,
                                  Box<dyn<GraphicsScopeBackend>> scopes, bool merge_scopes,
                                  texture::Submission* work) const
    -> Result<GraphPreparation, GraphExecutionError> {
    data_->execution->frame();
    auto slots = Vec<Option<PassFrameBinding>>::with_capacity(data_->entries.len());
    for (usize i {}; i < data_->entries.len(); ++i) slots.push(None());
    for (const auto& input : inputs) {
        if (! Arc<PassBindingOwner>::ptr_eq(input.owner_, data_->identity) ||
            input.slot_ >= data_->entries.len())
            return Err(GraphExecutionError { GraphExecutionStage::Schema, input.id_ });
        const auto& entry = data_->entries[input.slot_];
        if (entry.id != input.id_ || entry.revision != input.revision_ ||
            ! Arc<dyn<VulkanPass>>::ptr_eq(input.pass_, entry.native) || slots[input.slot_])
            return Err(GraphExecutionError { GraphExecutionStage::Schema, input.id_ });
        slots[input.slot_] = Some(input.clone());
    }
    auto started = PreparedGraphProgram::begin_prepare(
        data_->session, data_->graph, bindings, merge_scopes, work);
    if (started.is_err()) return Err(started.unwrap_err());
    return Ok(GraphPreparation(
        started.unwrap(),
        NativeFrameBackend { data_.clone(), rstd::move(slots), rstd::move(scopes) }));
}
auto CompiledGraph::instantiate(const GraphBindings& bindings, slice<PassFrameBinding> inputs,
                                Box<dyn<GraphicsScopeBackend>> scopes, bool merge_scopes,
                                texture::Submission* work) const
    -> Result<PreparedGraphProgram, GraphExecutionError> {
    auto started = prepare_frame(bindings, inputs, rstd::move(scopes), merge_scopes, work);
    if (started.is_err()) return Err(started.unwrap_err());
    auto preparation = started.unwrap();
    auto result      = preparation.advance();
    if (result.is_err()) return Err(result.unwrap_err());
    return Ok(preparation.program().unwrap());
}
} // namespace vrento::vulkan
