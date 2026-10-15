#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>
#include <cstdio>
import rstd;
import rstd.test;
import vrento;
import vrento.tests.shaders;

namespace vrento_test::pipeline_cache
{
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace vrento::vulkan;
using namespace vrento::resource_registry;

template<rstd::size_t N>
auto stage(const unsigned (&words)[N], vrento::ShaderType type) -> Uni_ShaderSpv {
    auto value   = Box<ShaderSpv>::make();
    value->stage = type;
    for (auto word : words) value->spirv.push(unsigned(word));
    return value;
}
auto request(vrento::resource::PipelineLayoutHandle layout) -> PipelineResourceRequest {
    GraphicsPipeline defaults;
    defaults.toDefault();
    PipelineResourceRequest value;
    value.pipeline_layout            = layout;
    value.topology                   = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    value.depth                      = defaults.depth;
    value.raster                     = defaults.raster;
    value.multisample                = defaults.multisample;
    value.color_blend.colorWriteMask = 15;
    value.shader_stages.push(stage(shaders::vertex.words, vrento::ShaderType::VERTEX));
    value.shader_stages.push(stage(shaders::fragment.words, vrento::ShaderType::FRAGMENT));
    return value;
}
TEST(PipelineCache, CompatibilityKey) {
    auto original                 = request({ u64(1), u64(1) });
    auto key                      = MakePipelineCacheKey(original);
    original.color_load_op        = VK_ATTACHMENT_LOAD_OP_CLEAR;
    original.color_initial_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    original.color_final_layout   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    rstd_assert(SamePipelineCacheKey(key, MakePipelineCacheKey(original)));
    auto render_key        = MakeRenderPassCacheKey(original);
    original.color_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
    rstd_assert(! SameRenderPassCacheKey(render_key, MakeRenderPassCacheKey(original)));
    rstd_assert(SamePipelineCacheKey(key, MakePipelineCacheKey(original)));
    original.color_format = VK_FORMAT_R8G8B8A8_SRGB;
    rstd_assert(! SamePipelineCacheKey(key, MakePipelineCacheKey(original)));
    original.color_format                     = VK_FORMAT_R8G8B8A8_UNORM;
    original.multisample.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
    rstd_assert(! SamePipelineCacheKey(key, MakePipelineCacheKey(original)));
    original.multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    original.raster.cullMode                  = VK_CULL_MODE_FRONT_BIT;
    rstd_assert(! SamePipelineCacheKey(key, MakePipelineCacheKey(original)));
}
TEST(PipelineCache, RetentionAndRenderPass) {
    auto                     session = DeviceSession::create({ .validation = true }).unwrap();
    DescriptorLayoutRegistry descriptors;
    PipelineLayoutRegistry   layouts;
    RenderPassRegistry       passes;
    PipelineRegistry         pipelines;
    auto                     layout = layouts.Ensure(session.device(), {}, descriptors).unwrap();
    auto                     first =
        Some(pipelines.Ensure(session.device(), request(layout.handle), passes, layouts).unwrap());
    auto first_handle          = first->handle;
    auto changed               = request(layout.handle);
    changed.color_load_op      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    changed.color_final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    {
        auto second =
            pipelines.Ensure(session.device(), rstd::move(changed), passes, layouts).unwrap();
        rstd_assert(second.cache_hit && second.handle == first->handle);
        rstd_assert(second.render_pass != first->render_pass);
        rstd_assert(! SameRenderPassCacheKey(first->render_pass_key, second.render_pass_key));
    }
    rstd_assert(pipelines.stats().creations == u64(1));
    pipelines.SetLimits({ .max_entries = usize(), .unused_collections = u64() });
    rstd_assert(pipelines.Resolve(first_handle).is_some());
    first = None();
    pipelines.PruneExpired();
    rstd_assert(pipelines.entryCount() == usize());
    rstd_assert(pipelines.Resolve(first_handle).is_none());
    pipelines.SetLimits({ .max_entries = usize(8), .unused_collections = u64(10) });
    auto cached_handle = first_handle;
    {
        auto cached =
            pipelines.Ensure(session.device(), request(layout.handle), passes, layouts).unwrap();
        cached_handle = cached.handle;
    }
    pipelines.PruneExpired();
    rstd_assert(pipelines.Resolve(cached_handle).is_some());
    auto reused =
        pipelines.Ensure(session.device(), request(layout.handle), passes, layouts).unwrap();
    rstd_assert(reused.cache_hit && reused.handle == cached_handle);
    for (unsigned i {}; i < 8; ++i)
        rstd_assert(
            pipelines.Ensure(session.device(), request(layout.handle), passes, layouts)->cache_hit);
    rstd_assert(pipelines.stats().creations == u64(2));
    auto recipe     = pipelines.RegisterRecipe(request(layout.handle), layouts).unwrap();
    auto equivalent = pipelines.RegisterRecipe(request(layout.handle), layouts).unwrap();
    auto scope      = MakeRenderPassResourceDesc(request(layout.handle));
    auto hot = pipelines.EnsureRecipe(session.device(), recipe, scope, passes, layouts).unwrap();
    rstd_assert(hot.cache_hit && hot.handle == cached_handle);
    scope.color_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
    rstd_assert(
        pipelines.EnsureRecipe(session.device(), equivalent, scope, passes, layouts)->cache_hit);
    rstd_assert(pipelines.stats().creations == u64(2));
    rstd_assert(pipelines.ReleaseRecipe(recipe));
    rstd_assert(pipelines.EnsureRecipe(session.device(), recipe, scope, passes, layouts).is_none());
    rstd_assert(session.validation_errors() == 0);
}
TEST(PipelineCache, DriverData) {
    auto  session  = DeviceSession::create({ .validation = true }).unwrap();
    auto& graphics = session.graphics_resources();
    auto  layout   = graphics.layouts.Ensure(session.device(), {}, graphics.descriptors).unwrap();
    auto  pipeline =
        graphics.pipelines
            .Ensure(session.device(), request(layout.handle), graphics.passes, graphics.layouts)
            .unwrap();
    auto encoded = graphics.pipelines.driver_cache().encode(session.device()).unwrap();
    rstd_assert(encoded.len() >= usize(48));
    DriverPipelineCache warm;
    warm.initialize(session.device(), encoded.as_slice()).unwrap();
    rstd_assert(warm.stats().loaded && ! warm.stats().rejected);
    auto warm_data = warm.encode(session.device()).unwrap();
    rstd_assert(warm_data.len() >= usize(48));
    auto directory = rstd::fs::TempDir::make("vrento-pipeline-cache"_str).unwrap();
    auto root      = rstd::path::PathBuf::from(directory.path().as_os_str().to_os_string());
    auto path      = root.join("driver.cache"_str);
    warm.save(session.device(), path.as_path()).unwrap();
    DriverPipelineCache loaded;
    loaded.load(session.device(), path.as_path()).unwrap();
    rstd_assert(loaded.stats().loaded);
    encoded[encoded.len() - usize(1)] =
        u8(encoded.as_slice()[encoded.len() - usize(1)].to_primitive() ^ 1);
    warm.initialize(session.device(), encoded.as_slice()).unwrap();
    rstd_assert(! warm.stats().loaded && warm.stats().rejected);
    auto truncated = slice<u8>::from_raw_parts(encoded.data(), usize(24));
    warm.initialize(session.device(), truncated).unwrap();
    rstd_assert(warm.stats().rejected);
    auto other = DeviceSession::create().unwrap();
    rstd_assert(warm.encode(other.device()).unwrap_err().stage == DriverCacheStage::Device);
    auto restored = DeviceSession::create({ .pipeline_cache_data = warm_data.as_slice() }).unwrap();
    rstd_assert(restored.graphics_resources().pipelines.driver_cache().stats().loaded);
    rstd_assert(session.validation_errors() == 0);
}
TEST(PipelineCache, RegistryOwnership) {
    auto               session = DeviceSession::create({ .validation = true }).unwrap();
    ResourceRegistries first, second;
    rstd_assert(first.Initialize(session) && second.Initialize(session));
    rstd_assert(&first.PipelineCache() == &session.graphics_resources().pipelines);
    rstd_assert(&first.PipelineLayouts() == &second.PipelineLayouts());
    auto layout =
        first.PipelineLayouts().Ensure(session.device(), {}, first.DescriptorLayouts()).unwrap();
    auto created = first.Pipelines()
                       .Ensure(session.device(),
                               request(layout.handle),
                               first.RenderPasses(),
                               first.PipelineLayouts())
                       .unwrap();
    first.Reset();
    auto reused = second.Pipelines()
                      .Ensure(session.device(),
                              request(layout.handle),
                              second.RenderPasses(),
                              second.PipelineLayouts())
                      .unwrap();
    rstd_assert(reused.cache_hit && reused.handle == created.handle);
    second.Reset();
    rstd_assert(session.graphics_resources().pipelines.stats().creations == u64(1));
    rstd_assert(session.validation_errors() == 0);
}
TEST(PipelineCache, AsyncPreparation) {
    auto  session  = DeviceSession::create({ .validation = true }).unwrap();
    auto& graphics = session.graphics_resources();
    auto  layout   = graphics.layouts.Ensure(session.device(), {}, graphics.descriptors).unwrap();
    auto  scope    = MakeRenderPassResourceDesc(request(layout.handle));
    auto  hot_recipe =
        graphics.pipelines.RegisterRecipe(request(layout.handle), graphics.layouts).unwrap();
    auto hot =
        graphics.pipelines
            .EnsureRecipe(session.device(), hot_recipe, scope, graphics.passes, graphics.layouts)
            .unwrap();
    auto low_request            = request(layout.handle);
    low_request.raster.cullMode = VK_CULL_MODE_BACK_BIT;
    auto low_recipe =
        graphics.pipelines.RegisterRecipe(rstd::move(low_request), graphics.layouts).unwrap();
    auto high_request            = request(layout.handle);
    high_request.raster.cullMode = VK_CULL_MODE_FRONT_BIT;
    auto high_recipe =
        graphics.pipelines.RegisterRecipe(rstd::move(high_request), graphics.layouts).unwrap();
    auto preparer  = PipelinePreparer::create(session, usize(1)).unwrap();
    auto low       = preparer.request(low_recipe, scope).unwrap();
    auto high      = preparer.request(high_recipe, scope, 10).unwrap();
    auto duplicate = preparer.request(high_recipe, scope, 20).unwrap();
    rstd_assert(rstd::sync::Arc<PipelineBuild>::ptr_eq(high.build, duplicate.build));
    rstd_assert(high.build->status() == PipelineReadiness::Pending);
    rstd_assert(
        graphics.pipelines
            .EnsureRecipe(session.device(), high_recipe, scope, graphics.passes, graphics.layouts)
            .is_none());
    preparer.advance();
    rstd_assert(low.build->status() == PipelineReadiness::Pending);
    for (unsigned i {}; i < 16; ++i)
        rstd_assert(graphics.pipelines
                        .EnsureRecipe(
                            session.device(), hot_recipe, scope, graphics.passes, graphics.layouts)
                        ->handle == hot.handle);
    auto started = rstd::time::Instant::now();
    while (high.build->status() == PipelineReadiness::Pending &&
           started.elapsed().as_secs() < u64(5))
        rstd::thread::sleep(rstd::time::Duration::from_millis(u64(1)));
    rstd_assert(high.build->status() == PipelineReadiness::Ready);
    preparer.close();
    rstd_assert(low.build->status() == PipelineReadiness::Failed);
    auto ready = preparer.poll(high).unwrap();
    rstd_assert(ready.status == PipelineReadiness::Ready && ready.pipeline.is_some());
    auto again = preparer.poll(duplicate).unwrap();
    rstd_assert(again.pipeline->handle == ready.pipeline->handle);
    rstd_assert(graphics.pipelines.stats().creations == u64(2));
    rstd_assert(preparer.poll(low)->status == PipelineReadiness::Failed);
    rstd_assert(preparer.request(high_recipe, scope).unwrap_err().kind ==
                vrento::resource::ResourceErrorKind::Closed);
    auto retry   = PipelinePreparer::create(session).unwrap();
    auto retried = retry.request(low_recipe, scope, 0, true).unwrap();
    retry.advance();
    retry.close();
    rstd_assert(retry.poll(retried)->status == PipelineReadiness::Ready);
    rstd_assert(graphics.pipelines.stats().creations == u64(3));
    rstd_assert(session.validation_errors() == 0);
}
TEST(PipelineCache, ResourcePreparationPending) {
    auto                 session = DeviceSession::create({ .validation = true }).unwrap();
    RenderResourceSystem resources;
    rstd_assert(resources.Initialize(session));
    resources.SetPipelinePreparationMode(PipelinePreparationMode::Deferred).unwrap();
    auto& graphics = session.graphics_resources();
    auto  layout   = graphics.layouts.Ensure(session.device(), {}, graphics.descriptors).unwrap();
    vrento::resource::PipelineUseHandle   use { u64(1), u64(1) };
    vrento::resource::RenderPassUseHandle pass { u64(1), u64(1) };
    auto pending = resources.PreparePipeline(use, pass, session.device(), request(layout.handle));
    rstd_assert(pending.is_err() &&
                pending.unwrap_err().kind == vrento::resource::ResourceErrorKind::Pending);
    rstd_assert(resources.Prepared().Resolve(use).is_none());
    auto start = rstd::time::Instant::now();
    for (;;) {
        auto result =
            resources.PreparePipeline(use, pass, session.device(), request(layout.handle));
        if (result.is_ok()) break;
        rstd_assert(result.unwrap_err().kind == vrento::resource::ResourceErrorKind::Pending);
        rstd_assert(start.elapsed().as_secs() < u64(5));
        rstd::thread::sleep(rstd::time::Duration::from_millis(u64(1)));
    }
    rstd_assert(resources.Prepared().Resolve(use).is_some());
    rstd_assert(graphics.pipelines.stats().creations == u64(1));
    auto repeated = resources.PreparePipeline(use, pass, session.device(), request(layout.handle));
    rstd_assert(repeated.is_ok());
    rstd_assert(repeated->physical->pipeline.handle);
    rstd_assert(graphics.pipelines.stats().creations == u64(1));
    auto cold            = request(layout.handle);
    cold.raster.cullMode = VK_CULL_MODE_FRONT_BIT;
    vrento::resource::PipelineUseHandle   second { u64(2), u64(1) };
    vrento::resource::RenderPassUseHandle second_pass { u64(2), u64(1) };
    auto                                  cancelled =
        resources.PreparePipeline(second, second_pass, session.device(), rstd::move(cold));
    rstd_assert(cancelled.unwrap_err().kind == vrento::resource::ResourceErrorKind::Pending);
    resources.AbortPreparePlan();
    rstd_assert(resources.Prepared().Resolve(second).is_none());
    resources.Reset();
    rstd_assert(session.validation_errors() == 0);
}
TEST(PipelineCache, PreparationDeviceLoss) {
    auto  session  = DeviceSession::create({ .validation = true }).unwrap();
    auto& graphics = session.graphics_resources();
    auto  layout   = graphics.layouts.Ensure(session.device(), {}, graphics.descriptors).unwrap();
    auto  recipe =
        graphics.pipelines.RegisterRecipe(request(layout.handle), graphics.layouts).unwrap();
    auto signature = MakeRenderPassResourceDesc(request(layout.handle));
    auto worker    = PipelinePreparer::create(session).unwrap();
    auto queued    = worker.request(recipe, signature).unwrap();
    session.observe_result(VK_ERROR_DEVICE_LOST);
    rstd_assert(worker.poll(queued)->status == PipelineReadiness::Failed);
    rstd_assert(queued.build->status() == PipelineReadiness::Failed);
    rstd_assert(worker.request(recipe, signature).unwrap_err().kind ==
                vrento::resource::ResourceErrorKind::Device);
    worker.close();
    rstd_assert(graphics.pipelines.stats().creations == u64());
}
TEST(PipelineCache, CreationTimings) {
    auto  session  = DeviceSession::create({ .validation = true }).unwrap();
    auto& graphics = session.graphics_resources();
    auto  layout   = graphics.layouts.Ensure(session.device(), {}, graphics.descriptors).unwrap();
    constexpr unsigned samples = 3, hits = 128;
    double             cold_ms {}, warm_ms {}, object_ms {};
    for (unsigned sample {}; sample < samples; ++sample) {
        graphics.pipelines.Reset();
        graphics.pipelines.driver_cache().initialize(session.device()).unwrap();
        auto started = rstd::time::Instant::now();
        {
            auto cold =
                graphics.pipelines
                    .Ensure(
                        session.device(), request(layout.handle), graphics.passes, graphics.layouts)
                    .unwrap();
            rstd_assert(! cold.cache_hit);
        }
        cold_ms += started.elapsed().as_secs_f64() * 1000;
        auto data = graphics.pipelines.driver_cache().encode(session.device()).unwrap();
        graphics.pipelines.Reset();
        graphics.pipelines.driver_cache().initialize(session.device(), data.as_slice()).unwrap();
        rstd_assert(graphics.pipelines.driver_cache().stats().loaded);
        started = rstd::time::Instant::now();
        auto warm =
            graphics.pipelines
                .Ensure(session.device(), request(layout.handle), graphics.passes, graphics.layouts)
                .unwrap();
        warm_ms += started.elapsed().as_secs_f64() * 1000;
        auto recipe =
            graphics.pipelines.RegisterRecipe(request(layout.handle), graphics.layouts).unwrap();
        auto signature                 = MakeRenderPassResourceDesc(request(layout.handle));
        signature.color_initial_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        auto created                   = graphics.pipelines.stats().creations;
        started                        = rstd::time::Instant::now();
        for (unsigned query {}; query < hits; ++query) {
            signature.color_load_op =
                query % 2 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            auto cached =
                graphics.pipelines
                    .EnsureRecipe(
                        session.device(), recipe, signature, graphics.passes, graphics.layouts)
                    .unwrap();
            rstd_assert(cached.cache_hit && cached.handle == warm.handle);
        }
        object_ms += started.elapsed().as_secs_f64() * 1000;
        rstd_assert(graphics.pipelines.stats().creations == created);
    }
    std::printf("pipeline: %u samples; cold driver %.3f ms; loaded driver %.3f ms; %u object hits "
                "%.3f ms; hot creations 0\n",
                samples,
                cold_ms / samples,
                warm_ms / samples,
                hits,
                object_ms / samples);
    rstd_assert(session.validation_errors() == 0);
}
} // namespace vrento_test::pipeline_cache

namespace vrento_test::pipeline_cache
{
TEST(PipelineCache, PendingUseDoesNotPublishPreviousRecipe) {
    auto created = DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto                 session = created.unwrap();
    RenderResourceSystem resources;
    ASSERT_TRUE(resources.Initialize(session));
    resources.SetPipelinePreparationMode(PipelinePreparationMode::Deferred).unwrap();
    auto& graphics = session.graphics_resources();
    auto  layout   = graphics.layouts.Ensure(session.device(), {}, graphics.descriptors).unwrap();
    auto  replacement                      = request(layout.handle);
    replacement.color_blend.colorWriteMask = 7;
    auto expected =
        graphics.pipelines
            .Ensure(session.device(), replacement.clone(), graphics.passes, graphics.layouts)
            .unwrap();
    vrento::resource::PipelineUseHandle   use { u64(1), u64(1) };
    vrento::resource::RenderPassUseHandle pass { u64(1), u64(1) };
    auto pending = resources.PreparePipeline(use, pass, session.device(), request(layout.handle));
    ASSERT_TRUE(pending.is_err());
    EXPECT_EQ(pending.unwrap_err().kind, vrento::resource::ResourceErrorKind::Pending);
    auto changed = resources.PreparePipeline(use, pass, session.device(), rstd::move(replacement));
    ASSERT_TRUE(changed.is_ok());
    EXPECT_EQ(changed->resource, expected.handle);
    EXPECT_EQ(resources.Prepared().Resolve(use).unwrap()->resource, expected.handle);
    resources.Reset();
    EXPECT_EQ(session.validation_errors(), 0u);
}
} // namespace vrento_test::pipeline_cache
