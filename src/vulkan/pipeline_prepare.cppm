export module vrento:vulkan.pipeline_prepare;
export import :vulkan.device_session;
import rstd;

export namespace vrento::vulkan
{
using namespace rstd::prelude;
using resource_registry::PipelineBuild;
using resource_registry::PipelineReadiness;
struct PipelinePrepareRequest {
    DeviceSession                  session;
    resource::PipelineRecipeHandle recipe;
    RenderPassResourceDesc         signature;
    rstd::sync::Arc<PipelineBuild> build;
};
struct PipelinePrepareResult {
    PipelineReadiness                                 status;
    Option<resource_registry::PipelineResourceResult> pipeline;
    Option<resource::ResourceError>                   error;
};
enum class PipelinePreparationMode
{
    Synchronous,
    Deferred
};

// Requests and publication stay on the host thread; workers only create immutable objects.
class PipelinePreparer {
    struct Queued {
        rstd::sync::Arc<PipelineBuild> build;
        rstd::uint32_t                 priority {};
    };
    DeviceSession                       session_;
    Vec<Queued>                         queued_;
    Vec<rstd::sync::Arc<PipelineBuild>> active_;
    usize                               parallelism_;
    Option<rstd::thread::ThreadPool>    pool_;
    PipelinePreparer(DeviceSession session, usize parallelism, rstd::thread::ThreadPool pool)
        : session_(rstd::move(session)), parallelism_(parallelism), pool_(Some(rstd::move(pool))) {}

public:
    PipelinePreparer(const PipelinePreparer&) = delete;
    PipelinePreparer(PipelinePreparer&& other)
        : session_(rstd::move(other.session_)),
          queued_(rstd::move(other.queued_)),
          active_(rstd::move(other.active_)),
          parallelism_(other.parallelism_),
          pool_(other.pool_.take()) {}
    ~PipelinePreparer() { close(); }
    static auto create(DeviceSession session, usize parallelism = usize(2))
        -> Result<PipelinePreparer, resource::ResourceError> {
        if (! session.valid() || session.lost())
            return Err(resource::ResourceError { .kind    = resource::ResourceErrorKind::Device,
                                                 .message = "pipeline preparation device"_Str });
        if (parallelism == usize() || parallelism > usize(8))
            return Err(resource::ResourceError { .kind    = resource::ResourceErrorKind::Invalid,
                                                 .message = "pipeline preparation invalid"_Str });
        auto builder = rstd::thread::ThreadPoolBuilder::make();
        builder.worker_count(parallelism);
        auto pool = builder.build();
        if (pool.is_err())
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::Worker,
                .message = "create pipeline worker pool failed"_Str,
                .source  = Some(resource::ResourceErrorSource::Io(pool.unwrap_err())) });
        return Ok(PipelinePreparer(rstd::move(session), parallelism, rstd::move(pool).unwrap()));
    }
    auto request(resource::PipelineRecipeHandle recipe, const RenderPassResourceDesc& signature,
                 rstd::uint32_t priority = 0, bool retry = false)
        -> Result<PipelinePrepareRequest, resource::ResourceError> {
        if (pool_.is_none())
            return Err(resource::ResourceError { .kind    = resource::ResourceErrorKind::Closed,
                                                 .message = "pipeline preparation closed"_Str });
        if (session_.lost())
            return Err(resource::ResourceError { .kind    = resource::ResourceErrorKind::Device,
                                                 .message = "pipeline preparation device"_Str });
        auto& owner     = session_.graphics_resources();
        auto  requested = owner.pipelines.RequestRecipe(
            session_.device(), recipe, signature, owner.passes, retry);
        if (requested.is_err()) return Err(rstd::move(requested).unwrap_err());
        auto work = rstd::move(requested).unwrap();
        if (work.dispatch)
            queued_.push({ work.build.clone(), priority });
        else
            for (auto& queued : queued_)
                if (rstd::sync::Arc<PipelineBuild>::ptr_eq(queued.build, work.build))
                    queued.priority = rstd::cmp::max(queued.priority, priority);
        return Ok(PipelinePrepareRequest { session_, recipe, signature, rstd::move(work.build) });
    }
    void advance() {
        active_.retain([](const auto& build) {
            return build->status() == PipelineReadiness::Pending;
        });
        if (pool_.is_none()) return;
        if (session_.lost()) {
            for (auto& work : queued_) work.build->cancel();
            queued_.clear();
            return;
        }
        while (! queued_.is_empty() && active_.len() < parallelism_) {
            usize selected {};
            for (usize i { 1 }; i < queued_.len(); ++i)
                if (queued_[i].priority > queued_[selected].priority) selected = i;
            auto work   = queued_.remove(selected).build;
            auto posted = pool_->handle().post([session = session_, work = work.clone()]() mutable {
                work->execute(session.device());
            });
            if (posted.is_err())
                work->fail(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::Closed,
                    .message = "pipeline worker queue closed during submission"_Str });
            else
                active_.push(rstd::move(work));
        }
    }
    auto poll(const PipelinePrepareRequest& request)
        -> Result<PipelinePrepareResult, resource::ResourceError> {
        if (request.session.epoch() != session_.epoch())
            return Err(resource::ResourceError { .kind    = resource::ResourceErrorKind::Device,
                                                 .message = "pipeline preparation device"_Str });
        advance();
        if (session_.lost())
            return Ok(PipelinePrepareResult {
                PipelineReadiness::Failed,
                None(),
                Some(resource::ResourceError {
                    .message = "pipeline device lost"_Str,
                    .source  = Some(resource::ResourceErrorSource::Native(
                        static_cast<rstd::int32_t>(VK_ERROR_DEVICE_LOST))) }) });
        auto                                              status = request.build->status();
        Option<resource_registry::PipelineResourceResult> pipeline;
        auto                                              failure = request.build->error();
        if (status == PipelineReadiness::Ready) {
            auto& owner    = session_.graphics_resources();
            auto  prepared = owner.pipelines.TryEnsureRecipe(
                session_.device(), request.recipe, request.signature, owner.passes, owner.layouts);
            if (prepared.is_err()) {
                status  = PipelineReadiness::Failed;
                failure = Some(rstd::move(prepared).unwrap_err());
            } else
                pipeline = Some(rstd::move(prepared).unwrap());
        }
        return Ok(PipelinePrepareResult { status, rstd::move(pipeline), rstd::move(failure) });
    }
    void close() {
        for (auto& work : queued_) work.build->cancel();
        queued_.clear();
        auto pool = pool_.take();
        if (pool) rstd::move(*pool).join();
        active_.clear();
    }
};

class PipelinePreparationQueue {
    DeviceSession            session_;
    Option<PipelinePreparer> worker_;
    bool                     closed_ {};
    PipelinePreparationQueue(DeviceSession session, Option<PipelinePreparer> worker)
        : session_(rstd::move(session)), worker_(rstd::move(worker)) {}

public:
    PipelinePreparationQueue(const PipelinePreparationQueue&) = delete;
    PipelinePreparationQueue(PipelinePreparationQueue&&)      = default;
    static auto create(DeviceSession session, PipelinePreparationMode mode)
        -> Result<PipelinePreparationQueue, resource::ResourceError> {
        if (! session.valid() || session.lost())
            return Err(resource::ResourceError { .kind    = resource::ResourceErrorKind::Device,
                                                 .message = "pipeline preparation device"_Str });
        Option<PipelinePreparer> worker;
        if (mode == PipelinePreparationMode::Deferred) {
            auto created = PipelinePreparer::create(session);
            if (created.is_err()) return Err(rstd::move(created).unwrap_err());
            worker = Some(created.unwrap());
        }
        return Ok(PipelinePreparationQueue(rstd::move(session), rstd::move(worker)));
    }
    auto prepare(resource::PipelineRecipeHandle recipe, const RenderPassResourceDesc& signature,
                 bool retry = false) -> Result<PipelinePrepareResult, resource::ResourceError> {
        if (closed_)
            return Err(resource::ResourceError { .kind    = resource::ResourceErrorKind::Closed,
                                                 .message = "pipeline preparation closed"_Str });
        if (session_.lost())
            return Err(resource::ResourceError { .kind    = resource::ResourceErrorKind::Device,
                                                 .message = "pipeline preparation device"_Str });
        if (worker_) {
            auto request = worker_->request(recipe, signature, 0, retry);
            if (request.is_err()) return Err(rstd::move(request).unwrap_err());
            auto work = request.unwrap();
            if (work.build->status() == PipelineReadiness::Pending) {
                worker_->advance();
                return Ok(PipelinePrepareResult { PipelineReadiness::Pending, None() });
            }
            return worker_->poll(work);
        }
        auto& graphics = session_.graphics_resources();
        auto  result   = graphics.pipelines.TryEnsureRecipe(
            session_.device(), recipe, signature, graphics.passes, graphics.layouts);
        if (result.is_err())
            return Ok(PipelinePrepareResult {
                PipelineReadiness::Failed, None(), Some(rstd::move(result).unwrap_err()) });
        return Ok(
            PipelinePrepareResult { PipelineReadiness::Ready, Some(rstd::move(result).unwrap()) });
    }
    void close() {
        closed_ = true;
        if (worker_) worker_->close();
    }
};
} // namespace vrento::vulkan
