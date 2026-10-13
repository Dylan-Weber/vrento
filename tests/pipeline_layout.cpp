#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

import rstd;
import rstd.test;
import vrento;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace vrento::vulkan;

namespace vrento_test::pipeline_layout
{
static auto Requirement(u64 index, u32 set_index, bool shared) -> PipelineLayoutRequirement {
    PipelineLayoutRequirement requirement {
        .pipeline = { .index = index, .generation = u64(1) },
    };
    PipelineLayoutSetRequirement set { .set = set_index };
    set.bindings.push(PipelineLayoutBindingRequirement {
        .binding          = u32(1),
        .descriptor_type  = u32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        .descriptor_count = u32(1),
        .stage_flags      = u32(VK_SHADER_STAGE_VERTEX_BIT),
        .shared_identity  = shared ? Some(u64(42)) : None<u64>(),
    });
    requirement.descriptor_sets.push(rstd::move(set));
    return requirement;
}

TEST(PipelineLayout, Bindings) {
    vrento::resource_registry::DescriptorSetPacketKey uniform, storage;
    uniform.buffers.push({ .binding = 0, .offset = 0, .size = 64 });
    storage.buffers.push(
        { .binding = 0, .offset = 0, .size = 64, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER });
    rstd_assert(! (uniform == storage));
    auto requirements = Vec<PipelineLayoutRequirement>::make();
    requirements.push(Requirement(u64(1), u32(), false));
    auto local = PlanPipelineLayouts(requirements.as_slice(), {}, false);
    rstd_assert(local.is_ok());
    rstd_assert(local->global_bindings.is_empty());
    rstd_assert(local->families.len() == usize(1));
    rstd_assert(local->families[usize()].request.descriptor_sets.len() == usize(1));

    requirements.clear();
    requirements.push(Requirement(u64(1), u32(2), true));
    requirements.push(Requirement(u64(2), u32(2), true));
    PipelineLayoutPolicy policy { .shared_set = Some(u32(2)) };
    auto                 shared = PlanPipelineLayouts(requirements.as_slice(), policy, false);
    rstd_assert(shared.is_ok());
    rstd_assert(shared->global_bindings.len() == usize(1));
    rstd_assert(shared->families.len() == usize(1));
    const auto& sets = shared->families[usize()].request.descriptor_sets;
    rstd_assert(sets.len() == usize(3));
    rstd_assert(sets[usize()].bindings.is_empty());
    rstd_assert(sets[usize(2)].bindings.len() == usize(1));
    rstd_assert(PlanPipelineLayouts(requirements.as_slice(), {}, false).is_err());
    requirements[usize(1)].descriptor_sets[usize()].bindings[usize()].shared_identity =
        Some(u64(43));
    rstd_assert(PlanPipelineLayouts(requirements.as_slice(), policy, false).is_err());

    requirements.clear();
    requirements.push(Requirement(u64(1), u32(), false));
    requirements[usize()].descriptor_sets[usize()].push_descriptor = true;
    auto push = PlanPipelineLayouts(requirements.as_slice(), {}, true, u32(32));
    rstd_assert(push.is_ok());
    rstd_assert(push->families[usize()].request.descriptor_sets[usize()].push_descriptor);
    auto fallback = PlanPipelineLayouts(requirements.as_slice(), {}, false);
    rstd_assert(fallback.is_ok());
    rstd_assert(! fallback->families[usize()].request.descriptor_sets[usize()].push_descriptor);
}
} // namespace vrento_test::pipeline_layout

namespace vrento_test::pipeline_layout
{
struct SharedBytes {
    array<u8, 16> bytes {};
    auto          LoadBuffer(const vrento::resource::BufferRequest&)
        -> Result<slice<u8>, vrento::resource::ResourceError> {
        return Ok(bytes.as_slice());
    }
};
TEST(PreparedGraphicsResources, SharedBindingsAndDescriptorLifetime) {
    namespace resource = vrento::resource;
    auto created       = DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto session   = created.unwrap();
    auto resources = rstd::sync::Arc<vrento::resource_registry::RenderResourceSystem>::make();
    ASSERT_TRUE(resources->Initialize(session));
    resource::ResourcePlan    plan { .generation = u64(1) };
    resource::BufferUseHandle buffer { .index = u64(), .generation = u64(1) };
    plan.buffers.push(
        { .handle     = buffer,
          .definition = vrento::resource::BufferRequest {
              .name       = "shared"_Str,
              .definition = { .size = usize(16), .usage = resource::BufferUsage::Uniform },
              .lifetime   = resource::BufferLifetimeClass::Dynamic } });
    SharedBytes bytes;
    auto        provider = dyn<resource::BufferContentProvider>::from_ref(bytes);
    auto        transaction =
        resources->BeginPreparePlan(plan, { .buffer = Some(provider.as_mut_ref()) }).unwrap();
    EXPECT_EQ(resources->ContinuePreparePlan(transaction).unwrap(),
              vrento::resource_registry::ResourcePrepareProgress::Complete);
    resources->CommitPreparePlan(transaction).unwrap();
    auto requirements = [] {
        Vec<PipelineLayoutRequirement> values;
        values.push(Requirement(u64(1), u32(), true));
        return values;
    };
    Vec<SharedDescriptorBufferUse> uses;
    uses.push({ u32(1), u64(42), buffer });
    auto prepared = Some(
        PreparedGraphicsResources::prepare(
            resources.clone(), {}, uses.as_slice(), requirements(), { .shared_set = Some(u32()) })
            .unwrap());
    auto descriptor = (*prepared)->shared_descriptor().unwrap();
    EXPECT_TRUE((*prepared)->layouts().Resolve({ .index = u64(1), .generation = u64(1) }));
    auto snapshot    = resources->Prepared().Resolve(descriptor).unwrap()->clone();
    auto compilation = (*prepared)->compilation();
    auto lifetime    = resources.downgrade();
    uses.push({ u32(1), u64(43), buffer });
    EXPECT_TRUE(
        PreparedGraphicsResources::prepare(
            resources.clone(), {}, uses.as_slice(), requirements(), { .shared_set = Some(u32()) })
            .is_err());
    EXPECT_TRUE(resources->Prepared().Resolve(descriptor));
    prepared = None();
    EXPECT_FALSE(resources->Prepared().Resolve(descriptor));
    EXPECT_TRUE(snapshot.set);
    {
        [[maybe_unused]] auto released = rstd::move(resources);
    }
    EXPECT_TRUE(lifetime.upgrade());
    EXPECT_EQ(session.validation_errors(), 0u);
}
} // namespace vrento_test::pipeline_layout
