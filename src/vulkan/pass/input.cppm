export module vrento:vulkan.pass.input;
export import :vulkan.graph;
export import :vulkan.pass.desc;
export import :pass.graph_builder;
import rstd;
using rstd::sync::Arc;

export namespace vrento::vulkan
{
class PassInput {
    struct Data {
        GraphSerial              scope, revision;
        PassExecutionInfo        execution;
        pass::PassIO             io;
        Arc<dyn<pass::PassDesc>> description;
    };
    Arc<Data> data_;
    explicit PassInput(Arc<Data> data): data_(rstd::move(data)) {}

public:
    template<typename T>
    static auto publish(GraphSerial scope, GraphSerial revision, PassId id, T value)
        -> Result<PassInput, GraphError> {
        if (! scope || ! revision || ! id.value)
            return Err(GraphError { GraphErrorKind::Conflict, id });
        auto concrete = dyn<pass::PassDesc>::from_ref(value);
        auto valid    = concrete.as_ref()->Validate();
        if (valid.is_err()) return Err(valid.unwrap_err());
        auto io = concrete.as_ref()->IO();
        for (const auto& input : io.inputs)
            if (auto source = input.source.resolve(scope); source.is_err())
                return Err(source.unwrap_err());
        for (const auto& output : io.outputs) {
            if (auto target = output.target.resolve(scope); target.is_err())
                return Err(target.unwrap_err());
            if (auto reference = output.target.output(); reference && reference->producer != id)
                return Err(GraphError { GraphErrorKind::Conflict, id });
        }
        auto execution = describe_io(value).execution(id);
        return Ok(
            PassInput(Arc<Data>::make(Data { scope,
                                             revision,
                                             rstd::move(execution),
                                             rstd::move(io),
                                             Arc<dyn<pass::PassDesc>>::make(rstd::move(value)) })));
    }
    auto scope() const -> GraphSerial { return data_->scope; }
    auto revision() const -> GraphSerial { return data_->revision; }
    auto execution() const -> const PassExecutionInfo& { return data_->execution; }
    auto type() const -> rstd::any::TypeId { return description().concrete_type_id(); }
    template<typename T>
    auto get() const -> Option<ref<T>> {
        return pass::try_desc<T>(description());
    }
    auto description() const -> ref<dyn<pass::PassDesc>> {
        return data_->description.as_ptr().as_ptr().as_ref();
    }
    auto io() const -> const pass::PassIO& { return data_->io; }
    auto same_input(const PassInput& other) const -> bool {
        return Arc<Data>::ptr_eq(data_, other.data_);
    }
    auto clone() const -> PassInput { return PassInput(data_.clone()); }
};

struct DeclaredContentPass {
    PassInput        description;
    pass::ContentRef output;
};
inline auto declare_input(fg::FrameGraphBuilder& graph, const PassInput& input, String name = {})
    -> Result<empty, GraphError> {
    if (graph.data().scope != input.scope() || ! input.revision())
        return Err(GraphError { GraphErrorKind::Conflict, input.execution().id });
    return pass::declare_io(
        graph, input.execution().id, { input.execution().queue.id }, input.io(), rstd::move(name));
}
template<typename T>
inline auto declare_input(fg::FrameGraphBuilder& graph, PassId id, T description,
                          GraphSerial revision = 1, String name = {})
    -> Result<PassInput, GraphError> {
    auto published = PassInput::publish(graph.data().scope, revision, id, rstd::move(description));
    if (published.is_err()) return Err(published.unwrap_err());
    auto input    = published.unwrap();
    auto declared = declare_input(graph, input, rstd::move(name));
    if (declared.is_err()) return Err(declared.unwrap_err());
    return Ok(rstd::move(input));
}

} // namespace vrento::vulkan
