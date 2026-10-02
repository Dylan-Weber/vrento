export module vrento.graph_executor;
export import vrento.rgraph;
import rstd;
using namespace rstd::prelude;

export namespace vrento::rg {
enum class ExecutionError {
    InvalidPass,
    WrongType,
    Duplicate,
    MissingExecutor,
    Cycle,
    Prepare,
    Record,
    NotPrepared
};

class DeclaredTextures {
    const RenderGraph &graph_;
    NodeHandle pass_;

  public:
    DeclaredTextures(const RenderGraph &graph, NodeHandle pass) : graph_(graph), pass_(pass) {}
    auto read(TextureNodeRef texture) const -> Option<TextureNodeState> {
        return graph_.readsTexture(pass_, texture) ? graph_.textureState(texture)
                                                   : None<TextureNodeState>();
    }
    auto write(TextureNodeRef texture) const -> Option<TextureNodeState> {
        return graph_.writesTexture(pass_, texture) ? graph_.textureState(texture)
                                                    : None<TextureNodeState>();
    }
};

template <class PrepareContext, class RecordContext> class GraphExecutor {
    struct Entry {
        NodeHandle node;
        explicit Entry(NodeHandle id) : node(id) {}
        virtual ~Entry() = default;
        virtual auto prepare(const RenderGraph &, PrepareContext &)
            -> Result<empty, ExecutionError> = 0;
        virtual auto record(RecordContext &) -> Result<empty, ExecutionError> = 0;
    };
    template <class PassType, class Adapter> struct Bound : Entry {
        Adapter adapter;
        Bound(NodeHandle node, Adapter value) : Entry(node), adapter(rstd::move(value)) {}
        auto prepare(const RenderGraph &graph, PrepareContext &context)
            -> Result<empty, ExecutionError> override {
            auto state = graph.passState(this->node);
            if (state.is_none())
                return Err(ExecutionError::InvalidPass);
            auto pass = graph.template getPass<PassType>(state->pass);
            if (pass.is_none())
                return Err(ExecutionError::WrongType);
            DeclaredTextures resources(graph, this->node);
            return adapter.prepare(*pass, resources, context);
        }
        auto record(RecordContext &context) -> Result<empty, ExecutionError> override {
            return adapter.record(context);
        }
    };
    Vec<Box<Entry>> entries_;
    Vec<usize> order_;
    bool prepared_{};
    const RenderGraph *graph_{};

  public:
    template <class PassType, class Adapter>
    auto bind(const RenderGraph &graph, NodeHandle node, Adapter adapter)
        -> Result<empty, ExecutionError> {
        if (graph_ && graph_ != &graph)
            return Err(ExecutionError::InvalidPass);
        auto state = graph.passState(node);
        if (state.is_none())
            return Err(ExecutionError::InvalidPass);
        if (graph.template getPass<PassType>(state->pass).is_none())
            return Err(ExecutionError::WrongType);
        for (const auto &entry : entries_)
            if (entry->node == node)
                return Err(ExecutionError::Duplicate);
        auto bound = Box<Bound<PassType, Adapter>>::make(node, rstd::move(adapter));
        entries_.push(Box<Entry>::from_raw(rstd::move(bound).into_raw().template cast<Entry>()));
        graph_ = &graph;
        prepared_ = false;
        return Ok(empty{});
    }
    auto prepare(const RenderGraph &graph, PrepareContext &context)
        -> Result<empty, ExecutionError> {
        prepared_ = false;
        order_.clear();
        if (graph_ && graph_ != &graph)
            return Err(ExecutionError::InvalidPass);
        auto sorted = graph.topologicalOrder();
        if (sorted.is_err())
            return Err(ExecutionError::Cycle);
        for (auto node : sorted.unwrap()) {
            auto state = graph.passState(node);
            if (state->type == PassNode::Type::Virtual)
                continue;
            usize index{};
            while (index < entries_.len() && entries_[index]->node != node)
                ++index;
            if (index == entries_.len())
                return Err(ExecutionError::MissingExecutor);
            order_.push(usize(index));
        }
        for (auto index : order_) {
            auto result = entries_[index]->prepare(graph, context);
            if (result.is_err())
                return result;
        }
        prepared_ = true;
        return Ok(empty{});
    }
    auto record(RecordContext &context) -> Result<empty, ExecutionError> {
        if (!prepared_)
            return Err(ExecutionError::NotPrepared);
        for (auto index : order_) {
            auto result = entries_[index]->record(context);
            if (result.is_err()) {
                prepared_ = false;
                return result;
            }
        }
        return Ok(empty{});
    }
};
} // namespace vrento::rg
