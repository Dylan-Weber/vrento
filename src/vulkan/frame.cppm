export module vrento:vulkan.frame;
export import :vulkan.renderer;
import :resource_registry.system;
import rstd;

export namespace vrento::vulkan
{
struct GraphFrameRequest {
    struct Buffer {
        fg::ResourceVersion version;
        BufferAllocation    allocation;
    };
    GraphBindings                  bindings;
    Vec<PassFrameBinding>          parameters;
    Vec<Buffer>                    buffers;
    Box<dyn<GraphicsScopeBackend>> scopes;
};

enum class FrameState
{
    Pending,
    Ready,
    Recorded,
    Submitted,
    Completed,
    Failed,
    Cancelled,
    Uncertain
};
class GraphFrame {
    Arc<resource_registry::RenderResourceSystem> resources_;
    Option<GraphPreparation>                     preparation_;
    Option<texture::Submission>                  work_;
    texture::SubmissionReceipt                   receipt_;
    FrameState                                   state_ { FrameState::Pending };
    Option<GraphExecutionError>                  error_;
    Option<texture::SubmissionToken>             completion_;

    GraphFrame(Arc<resource_registry::RenderResourceSystem> resources, GraphPreparation preparation,
               texture::Submission work)
        : resources_(rstd::move(resources)),
          preparation_(Some(rstd::move(preparation))),
          work_(Some(rstd::move(work))),
          receipt_(work_->receipt()) {}
    auto fail(GraphExecutionError error) -> Result<empty, GraphExecutionError> {
        error_ = Some(error);
        state_ = receipt_.uncertain() ? FrameState::Uncertain : FrameState::Failed;
        if (work_) work_->discard();
        work_        = None();
        preparation_ = None();
        return Err(error);
    }

public:
    GraphFrame(const GraphFrame&) = delete;
    GraphFrame(GraphFrame&&)      = default;
    static auto begin(Arc<resource_registry::RenderResourceSystem> resources, CompiledGraph graph,
                      GraphFrameRequest request, texture::Submission work,
                      Vec<resource_registry::PreparedTextureUpdate> regions = {})
        -> Result<GraphFrame, GraphExecutionError> {
        auto uploaded = resources->PreparePendingUploads(work);
        if (uploaded.is_err())
            return Err(GraphExecutionError {
                GraphExecutionStage::Prepare, {}, {}, VK_SUCCESS, Some(uploaded.unwrap_err()) });
        auto batch = Arc<resource_registry::PreparedResourceUploads>::make(uploaded.unwrap());
        for (auto& region : regions) {
            auto added = batch->Add(rstd::move(region));
            if (added.is_err())
                return Err(GraphExecutionError {
                    GraphExecutionStage::Prepare, {}, {}, VK_SUCCESS, Some(added.unwrap_err()) });
        }
        work.retain(batch.clone());
        auto recorded = batch->Record(work);
        if (recorded.is_err())
            return Err(GraphExecutionError {
                GraphExecutionStage::Prepare, {}, {}, VK_SUCCESS, Some(recorded.unwrap_err()) });
        for (auto& buffer : request.buffers) {
            auto binding = batch->BufferBinding(buffer.allocation);
            auto existing =
                request.bindings.resolve(buffer.version.resource, buffer.version.version);
            if (existing) {
                if ((*existing)->Info().identity != binding->Info().identity)
                    return Err(GraphExecutionError {
                        GraphExecutionStage::Binding, {}, buffer.version.resource });
                continue;
            }
            auto bound = request.bindings.bind_version(buffer.version, rstd::move(binding));
            if (bound.is_err()) return Err(bound.unwrap_err());
        }
        auto started = graph.prepare_frame(request.bindings,
                                           request.parameters.as_slice(),
                                           rstd::move(request.scopes),
                                           true,
                                           &work);
        if (started.is_err()) return Err(started.unwrap_err());
        return Ok(GraphFrame(rstd::move(resources), started.unwrap(), rstd::move(work)));
    }
    auto state() const -> FrameState { return state_; }
    auto error() const -> Option<GraphExecutionError> { return error_; }
    auto receipt() const -> texture::SubmissionReceipt { return receipt_; }
    auto completion() const -> Option<texture::SubmissionToken> {
        return completion_ ? Some(texture::SubmissionToken(*completion_))
                           : None<texture::SubmissionToken>();
    }
    auto advance() -> Result<empty, GraphExecutionError> {
        if (state_ == FrameState::Ready) return Ok(empty {});
        if (state_ != FrameState::Pending)
            return Err(error_.unwrap_or(GraphExecutionError { GraphExecutionStage::Prepare }));
        auto result = preparation_->advance();
        if (result.is_err()) {
            if (result.unwrap_err().stage == GraphExecutionStage::Pending) return result;
            return fail(result.unwrap_err());
        }
        state_ = FrameState::Ready;
        return Ok(empty {});
    }
    auto record() -> Result<empty, GraphExecutionError> {
        if (state_ != FrameState::Ready)
            return Err(GraphExecutionError { state_ == FrameState::Pending
                                                 ? GraphExecutionStage::Pending
                                                 : GraphExecutionStage::Record });
        auto result = preparation_->program()->record(*work_);
        if (result.is_err()) return fail(result.unwrap_err());
        state_ = FrameState::Recorded;
        return Ok(empty {});
    }
    auto submit() -> Result<texture::SubmissionToken, GraphExecutionError> {
        if (state_ != FrameState::Recorded)
            return Err(GraphExecutionError { GraphExecutionStage::Record });
        auto result = resources_->SubmitFrame(work_.take().unwrap());
        if (result.is_err()) {
            auto error = GraphExecutionError {
                GraphExecutionStage::Record, {}, {}, VK_SUCCESS, Some(result.unwrap_err())
            };
            (void)fail(error);
            return Err(error);
        }
        completion_  = Some(result.unwrap());
        state_       = FrameState::Submitted;
        preparation_ = None();
        return Ok(*completion_);
    }
    auto wait(texture::Serial timeout = 5'000'000'000) -> Result<empty, texture::Error> {
        if (state_ == FrameState::Completed) return Ok(empty {});
        if (state_ != FrameState::Submitted || ! completion_ || ! resources_->Ready())
            return Err(texture::Error { texture::ErrorKind::Invalid });
        auto result = resources_->TextureContext().wait(*completion_, timeout);
        if (result.is_err()) return result;
        state_ = FrameState::Completed;
        return Ok(empty {});
    }
    void cancel() {
        if (state_ != FrameState::Pending && state_ != FrameState::Ready &&
            state_ != FrameState::Recorded)
            return;
        if (work_) work_->discard();
        work_        = None();
        state_       = FrameState::Cancelled;
        preparation_ = None();
    }
};
} // namespace vrento::vulkan
