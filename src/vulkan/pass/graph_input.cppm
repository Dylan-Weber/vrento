export module vrento:vulkan.pass.graph_input;
export import :vulkan.pass.input;
export import :pass.resources;
import rstd;
export namespace vrento::vulkan
{
class GraphInput {
    struct Data {
        fg::FrameGraph              graph;
        Vec<PassInput>              descriptions;
        Arc<pass::GraphResourceMap> resources;
    };
    Arc<Data> data_;
    explicit GraphInput(Arc<Data> data): data_(rstd::move(data)) {}
    static auto publish(fg::FrameGraph graph, Vec<PassInput> descriptions,
                        Arc<pass::GraphResourceMap> resources) -> Result<GraphInput, GraphError> {
        if (resources->scope != graph.scope() || descriptions.len() != graph.passes().len())
            return Err(GraphError { GraphErrorKind::Conflict });
        for (usize i {}; i < descriptions.len(); ++i) {
            const auto& value = descriptions[i];
            if (value.scope() != graph.scope() || ! graph.find_pass(value.execution().id))
                return Err(GraphError { GraphErrorKind::MissingPass, value.execution().id });
            const auto& io = value.io();
            auto validate  = [&](const pass::ContentRef& content) -> Result<empty, GraphError> {
                auto resolved = content.resolve(graph);
                if (resolved.is_err()) return Err(resolved.unwrap_err());
                return resources->validate(content);
            };
            for (const auto& read : io.inputs) {
                auto valid = validate(read.source);
                if (valid.is_err()) return Err(valid.unwrap_err());
            }
            for (const auto& write : io.outputs) {
                auto valid = validate(write.target);
                if (valid.is_err()) return Err(valid.unwrap_err());
            }
            for (usize j {}; j < i; ++j)
                if (descriptions[j].execution().id == value.execution().id)
                    return Err(GraphError { GraphErrorKind::Duplicate, value.execution().id });
        }
        return Ok(GraphInput(Arc<Data>::make(
            Data { rstd::move(graph), rstd::move(descriptions), rstd::move(resources) })));
    }

public:
    GraphInput(const GraphInput& other): data_(other.data_.clone()) {}
    GraphInput(GraphInput&&)                           = default;
    auto        operator=(GraphInput&&) -> GraphInput& = default;
    static auto create(fg::FrameGraph graph, Vec<PassInput> descriptions,
                       pass::GraphResourceMap resources) -> Result<GraphInput, GraphError> {
        return publish(rstd::move(graph),
                       rstd::move(descriptions),
                       Arc<pass::GraphResourceMap>::make(rstd::move(resources)));
    }
    auto graph() const -> const fg::FrameGraph& { return data_->graph; }
    auto descriptions() const -> slice<PassInput> { return data_->descriptions.as_slice(); }
    auto resources() const -> const pass::GraphResourceMap& { return *data_->resources; }
    auto with_descriptions(Vec<PassInput> descriptions) const -> Result<GraphInput, GraphError> {
        return publish(data_->graph, rstd::move(descriptions), data_->resources.clone());
    }
};
} // namespace vrento::vulkan
