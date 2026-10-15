export module vrento:vulkan.graph_queue;
export import :vulkan.graph;
import rstd;

namespace vrento::vulkan
{
struct QueueSubmitDelegate {
    rstd::sync::Arc<dyn<NativeQueueSubmit>> owner;
    auto Submit(const vvk::Queue& queue, const VkSubmitInfo& info, VkFence fence) -> VkResult {
        return owner->Submit(queue, info, fence);
    }
};
} // namespace vrento::vulkan
namespace rstd
{
template<>
struct Impl<vrento::vulkan::NativeQueueSubmit, vrento::vulkan::QueueSubmitDelegate>
    : ImplBase<vrento::vulkan::QueueSubmitDelegate> {
    auto Submit(const vvk::Queue& queue, const VkSubmitInfo& info, VkFence fence) -> VkResult {
        return self().Submit(queue, info, fence);
    }
};
} // namespace rstd
export namespace vrento::vulkan
{
struct GraphQueueAssignment {
    LogicalQueue    logical;
    QueueParameters native;
};
enum class GraphSubmissionStatus
{
    Submitted,
    Failed,
    Partial,
    Uncertain
};
struct GraphQueueState {
    PreparedGraphProgram     program;
    Vec<Box<TextureContext>> queues;
    Vec<usize>               mapping;
    struct Handoff {
        usize               source, target;
        PassResourceUse     use;
        fg::ResourceVersion source_version;
    };
    Vec<Handoff> handoffs;
    bool         used {};
    explicit GraphQueueState(PreparedGraphProgram value): program(rstd::move(value)) {}
};
class GraphSubmission {
    rstd::sync::Arc<GraphQueueState>    owner_;
    Vec<graph_texture::SubmissionToken> tokens_;
    GraphSubmissionStatus               status_ { GraphSubmissionStatus::Submitted };
    Option<GraphExecutionError>         error_;
    friend class GraphQueueProgram;
    explicit GraphSubmission(rstd::sync::Arc<GraphQueueState> owner): owner_(rstd::move(owner)) {}

public:
    GraphSubmission(GraphSubmission&&) = default;
    auto status() const -> GraphSubmissionStatus { return status_; }
    auto error() const -> const Option<GraphExecutionError>& { return error_; }
    auto accepted() const -> slice<graph_texture::SubmissionToken> { return tokens_.as_slice(); }
    auto poll() -> Result<bool, graph_texture::Error> {
        if (status_ == GraphSubmissionStatus::Uncertain)
            return Err(graph_texture::Error { graph_texture::ErrorKind::Closed });
        bool complete = true;
        for (const auto& token : tokens_) {
            auto ready = token.poll();
            if (ready.is_err()) return Err(ready.unwrap_err());
            complete &= ready.unwrap();
        }
        if (complete)
            for (auto& queue : owner_->queues) {
                auto retired = queue->Poll();
                if (retired.is_err()) return Err(retired.unwrap_err());
            }
        return Ok(complete);
    }
    auto ready() -> Result<bool, graph_texture::Error> {
        if (status_ != GraphSubmissionStatus::Submitted) return Ok(false);
        return poll();
    }
    auto wait(graph_texture::Serial timeout = 5'000'000'000)
        -> Result<empty, graph_texture::Error> {
        if (status_ == GraphSubmissionStatus::Uncertain)
            return Err(graph_texture::Error { graph_texture::ErrorKind::Closed });
        auto begin = rstd::time::Instant::now();
        for (const auto& token : tokens_) {
            auto elapsed   = begin.elapsed().as_nanos().to_primitive();
            auto remaining = elapsed >= timeout ? graph_texture::Serial()
                                                : timeout - graph_texture::Serial(elapsed);
            auto waited    = owner_->queues[usize()]->wait(token, remaining);
            if (waited.is_err()) return Err(waited.unwrap_err());
        }
        auto retired = poll();
        if (retired.is_err()) return Err(retired.unwrap_err());
        return Ok(empty {});
    }
};
class GraphQueueProgram {
    rstd::sync::Arc<GraphQueueState> state_;
    explicit GraphQueueProgram(rstd::sync::Arc<GraphQueueState> state): state_(rstd::move(state)) {}

public:
    GraphQueueProgram(GraphQueueProgram&&) = default;
    static auto prepare(DeviceSession session, VulkanPassSet graph, const GraphBindings& bindings,
                        VulkanPassBackends& backends, slice<GraphQueueAssignment> assignments,
                        Option<rstd::sync::Arc<dyn<NativeQueueSubmit>>> submitter = None())
        -> Result<GraphQueueProgram, GraphExecutionError> {
        auto prepared =
            PreparedGraphProgram::prepare(session, rstd::move(graph), bindings, backends, false);
        if (prepared.is_err()) return Err(prepared.unwrap_err());
        auto state = rstd::sync::Arc<GraphQueueState>::make(prepared.unwrap());
        for (const auto& batch : state->program.schedule().batches) {
            const auto logical =
                state->program.description().passes()[usize(batch.passes[usize()])].queue;
            Option<QueueParameters> selected;
            for (const auto& entry : assignments)
                if (entry.logical.id == batch.queue.value) {
                    if (selected || entry.logical != logical)
                        return Err(GraphExecutionError { GraphExecutionStage::Unsupported });
                    selected = Some(QueueParameters(entry.native));
                }
            if (! selected) return Err(GraphExecutionError { GraphExecutionStage::Unsupported });
            auto flags = selected->flags;
            if ((logical.capability == QueueClass::Graphics && ! (flags & VK_QUEUE_GRAPHICS_BIT)) ||
                (logical.capability == QueueClass::Compute && ! (flags & VK_QUEUE_COMPUTE_BIT)) ||
                (logical.capability == QueueClass::Transfer &&
                 ! (flags &
                    (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT))))
                return Err(GraphExecutionError { GraphExecutionStage::Unsupported });
            usize index {};
            while (index < state->queues.len() &&
                   *state->queues[index]->queue().handle != *selected->handle)
                ++index;
            if (index == state->queues.len()) {
                Option<Box<dyn<NativeQueueSubmit>>> delegate;
                if (submitter)
                    delegate = Some(Box<dyn<NativeQueueSubmit>>::make(
                        QueueSubmitDelegate { submitter->clone() }));
                auto context =
                    TextureContext::create_on_queue(session, *selected, rstd::move(delegate));
                if (context.is_err())
                    return Err(GraphExecutionError {
                        GraphExecutionStage::Device,
                        {},
                        {},
                        vrento::vulkan::GraphExecutionCause::Texture(context.unwrap_err()) });
                state->queues.push(Box<TextureContext>::make(context.unwrap()));
            }
            state->mapping.push(usize(index));
        }
        struct RangeOwner {
            ResourceId id;
            GraphRange range;
            usize      batch;
            GraphIndex version {};
        };
        Vec<RangeOwner> owners;
        const auto&     schedule = state->program.schedule();
        for (usize batch {}; batch < schedule.batches.len(); ++batch) {
            const auto& pass = state->program.description()
                                   .passes()[usize(schedule.batches[batch].passes[usize()])];
            for (const auto& use : pass.uses) {
                auto info = state->program
                                .binding_version(fg::ResourceVersion { use.resource, use.version })
                                .unwrap()
                                ->Info();
                Vec<GraphRange> ranges;
                if (info.kind == GraphResourceKind::Buffer)
                    ranges.push(GraphRange(use.range));
                else
                    for (GraphIndex layer = use.range.first_layer;
                         layer < use.range.first_layer + use.range.layer_count;
                         ++layer)
                        for (GraphIndex mip = use.range.first_mip;
                             mip < use.range.first_mip + use.range.mip_count;
                             ++mip)
                            ranges.push({ .aspect      = use.range.aspect,
                                          .first_mip   = mip,
                                          .first_layer = layer });
                for (const auto& range : ranges) {
                    Vec<RangeOwner> next;
                    for (const auto& old : owners) {
                        bool overlaps =
                            state->program
                                    .binding_version(fg::ResourceVersion { old.id, old.version })
                                    .unwrap()
                                    ->Info()
                                    .identity == info.identity &&
                            (info.kind == GraphResourceKind::Buffer
                                 ? old.range.offset < range.offset + range.size &&
                                       range.offset < old.range.offset + old.range.size
                                 : old.range == range);
                        if (! overlaps) {
                            next.push(RangeOwner(old));
                            continue;
                        }
                        auto intersection = range;
                        if (info.kind == GraphResourceKind::Buffer) {
                            intersection.offset = rstd::cmp::max(range.offset, old.range.offset);
                            intersection.size = rstd::cmp::min(range.offset + range.size,
                                                               old.range.offset + old.range.size) -
                                                intersection.offset;
                            if (old.range.offset < intersection.offset) {
                                auto left       = old;
                                left.range.size = intersection.offset - old.range.offset;
                                next.push(rstd::move(left));
                            }
                            if (intersection.offset + intersection.size <
                                old.range.offset + old.range.size) {
                                auto right         = old;
                                right.range.offset = intersection.offset + intersection.size;
                                right.range.size =
                                    old.range.offset + old.range.size - right.range.offset;
                                next.push(rstd::move(right));
                            }
                        }
                        auto source_family =
                            state->queues[state->mapping[old.batch]]->queue().family_index;
                        auto target_family =
                            state->queues[state->mapping[batch]]->queue().family_index;
                        if (old.batch != batch && source_family != target_family) {
                            auto transferred  = use;
                            transferred.range = intersection;
                            state->handoffs.push(
                                { old.batch, batch, transferred, { old.id, old.version } });
                        }
                    }
                    next.push({ use.resource, range, batch, use.version });
                    owners = rstd::move(next);
                }
            }
        }
        return Ok(GraphQueueProgram(rstd::move(state)));
    }
    auto execute() -> GraphSubmission {
        GraphSubmission result(state_.clone());
        auto            fail = [&](GraphExecutionError error, bool uncertain = false) {
            result.status_ = uncertain                   ? GraphSubmissionStatus::Uncertain
                             : result.tokens_.is_empty() ? GraphSubmissionStatus::Failed
                                                         : GraphSubmissionStatus::Partial;
            result.error_  = Some(rstd::move(error));
        };
        if (state_->used) {
            fail(GraphExecutionError { GraphExecutionStage::Record });
            return result;
        }
        state_->used         = true;
        const auto& schedule = state_->program.schedule();
        for (usize index {}; index < schedule.batches.len(); ++index) {
            auto& queue = state_->queues[state_->mapping[index]];
            auto  begun = queue->Begin();
            if (begun.is_err()) {
                fail(GraphExecutionError {
                    GraphExecutionStage::Record,
                    {},
                    {},
                    vrento::vulkan::GraphExecutionCause::Texture(begun.unwrap_err()) });
                break;
            }
            auto work     = begun.unwrap();
            auto add_wait = [&](usize before) -> Result<empty, graph_texture::Error> {
                return work.commands()->Wait(result.tokens_[before], graph_texture::Stages::All);
            };
            Option<graph_texture::Error> wait_error;
            for (auto before : schedule.batches[index].waits) {
                auto waited = add_wait(usize(before));
                if (waited.is_err()) {
                    wait_error = Some(waited.unwrap_err());
                    break;
                }
            }
            for (const auto& handoff : state_->handoffs)
                if (! wait_error && handoff.target == index) {
                    auto waited = add_wait(handoff.source);
                    if (waited.is_err()) wait_error = Some(waited.unwrap_err());
                }
            if (wait_error) {
                fail(GraphExecutionError { GraphExecutionStage::Access,
                                           {},
                                           {},
                                           vrento::vulkan::GraphExecutionCause::Texture(
                                               (rstd::move(wait_error)).unwrap()) });
                break;
            }
            auto recorded = state_->program.record_batch(index, work);
            if (recorded.is_err()) {
                fail(recorded.unwrap_err());
                break;
            }
            bool release_failed {};
            for (const auto& handoff : state_->handoffs)
                if (handoff.source == index) {
                    auto destination =
                        state_->queues[state_->mapping[handoff.target]]->queue().family_index;
                    auto released = state_->program.binding_version(handoff.source_version)
                                        .unwrap()
                                        ->Release(work, destination, handoff.use);
                    if (released.is_err()) {
                        fail(released.unwrap_err());
                        release_failed = true;
                        break;
                    }
                }
            if (release_failed) break;
            auto receipt   = work.receipt();
            auto submitted = queue->Submit(rstd::move(work));
            if (submitted.is_err()) {
                fail(GraphExecutionError { GraphExecutionStage::Submit,
                                           {},
                                           {},
                                           vrento::vulkan::GraphExecutionCause::Texture(
                                               submitted.unwrap_err()) },
                     receipt.uncertain());
                break;
            }
            result.tokens_.push(submitted.unwrap());
        }
        return result;
    }
};
} // namespace vrento::vulkan
