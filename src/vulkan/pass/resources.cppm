export module vrento:vulkan.pass.resources;
import rstd;
import :resource_registry.system;
import :vulkan.pipeline_layout;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

export namespace vrento::vulkan
{
class GraphicsCompileResources {
    DeviceSession                                session_;
    Arc<resource_registry::RenderResourceSystem> owner_;
    PipelineLayoutAssignments                    layouts_;
    struct Prepared {
        resource::PipelineUseHandle            use;
        resource_registry::PipelinePreparation value;
    };
    mutable Vec<Prepared> prepared_;

public:
    GraphicsCompileResources(DeviceSession                                session,
                             Arc<resource_registry::RenderResourceSystem> owner,
                             PipelineLayoutAssignments                    layouts)
        : session_(rstd::move(session)), owner_(rstd::move(owner)), layouts_(rstd::move(layouts)) {}
    auto session() const -> const DeviceSession& { return session_; }
    auto prepared(resource::PipelineUseHandle use) const
        -> Option<ref<resource_registry::PipelinePreparation>> {
        for (const auto& entry : prepared_)
            if (entry.use == use)
                return Some(
                    ref<resource_registry::PipelinePreparation>::from_raw_parts(&entry.value));
        return None();
    }
    auto Prepare(resource::PipelineUseHandle pipeline, resource::RenderPassUseHandle render_pass,
                 const PipelineResourceRequest& recipe, const DeviceSession& session) const
        -> Result<vrento::Preparation<resource_registry::PipelinePreparation>,
                  resource::ResourceError> {
        auto layout = layouts_.Resolve(pipeline);
        if (! layout)
            return Err(
                resource::ResourceError { .kind    = resource::ResourceErrorKind::MissingDefinition,
                                          .message = "pipeline layout unavailable"_Str });
        auto request            = recipe.clone();
        request.pipeline_layout = *layout;
        request.raster.depthClampEnable &= session.device().capabilities().depth_clamp;
        auto result =
            owner_->PreparePipeline(pipeline, render_pass, session.device(), rstd::move(request));
        if (result.is_err()) {
            auto error = result.unwrap_err();
            error.contexts.push(resource::ResourceErrorContext::Pipeline(pipeline, render_pass));
            return Err(rstd::move(error));
        }
        if (! result->ready()) return Ok(PendingPreparation {});
        auto value = result->take_ready();
        prepared_.retain([&](const Prepared& entry) {
            return entry.use != pipeline;
        });
        prepared_.push(
            { pipeline,
              resource_registry::PipelinePreparation { value.resource,
                                                       value.render_pass,
                                                       value.cache_key.clone(),
                                                       value.render_pass_key.clone(),
                                                       value.cache_hit,
                                                       value.cache_observed_count,
                                                       value.render_pass_cache_hit,
                                                       value.render_pass_cache_observed_count,
                                                       value.physical.clone() } });
        return Ok(rstd::move(value));
    }
};
} // namespace vrento::vulkan
