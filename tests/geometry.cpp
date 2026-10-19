#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

import rstd;
import rstd.test;
import vrento;

using namespace rstd::literals;

using namespace rstd::prelude;
using namespace vrento;

namespace vrento_test::geometry
{
TEST(BufferContent, PreservesAllocationAndAllocatorUntilLastOwner) {
    rstd::alloc::AllocationTracker  tracker;
    Option<resource::BufferContent> retained;
    const void*                     pointer {};
    {
        auto bytes = Vec<u8, rstd::alloc::TrackingAllocator<>>::new_in(
            rstd::alloc::TrackingAllocator<>(rstd::alloc::Global {}, tracker));
        bytes.resize(usize(4096), u8(7));
        pointer      = bytes.as_slice().as_raw_ptr();
        auto content = resource::BufferContent::from(rstd::move(bytes));
        EXPECT_EQ(content.as_slice().as_raw_ptr(), pointer);
        retained = Some(content.clone());
        EXPECT_EQ(retained->as_slice().as_raw_ptr(), pointer);
        EXPECT_EQ(tracker.snapshot().allocations, u64(1));
    }
    EXPECT_EQ(retained->as_slice()[usize()], u8(7));
    EXPECT_EQ(tracker.snapshot().deallocations, u64());
    retained = None();
    EXPECT_EQ(tracker.snapshot().live_bytes, u64());
    EXPECT_EQ(tracker.snapshot().deallocations, u64(1));
}

TEST(BufferContent, DistinguishesBorrowedBytesEmptyAndZeroed) {
    Vec<u8> input;
    input.resize(usize(4), u8(7));
    auto content   = resource::BufferContent::from(input.as_slice());
    input[usize()] = u8(9);
    EXPECT_EQ(content.as_slice()[usize()], u8(7));
    EXPECT_FALSE(content.is_zeroed());
    auto moved = rstd::move(content);
    EXPECT_TRUE(content.as_slice().is_empty());
    EXPECT_EQ(content.len(), usize());
    EXPECT_EQ(moved.as_slice()[usize()], u8(7));
    auto zeroed = resource::BufferContent::zeroed(usize(1024 * 1024));
    EXPECT_TRUE(zeroed.clone().is_zeroed());
    EXPECT_EQ(zeroed.len(), usize(1024 * 1024));
    EXPECT_TRUE(zeroed.as_slice().is_empty());
    EXPECT_FALSE(resource::BufferContent {}.is_zeroed());
}

TEST(Geometry, SnapshotSharesOwnedBytesAndSeparatesLayout) {
    Vec<u8> bytes;
    bytes.resize(usize(16), u8(3));
    auto           pointer = bytes.as_slice().as_raw_ptr();
    GeometryLayout layout;
    layout.vertices.push({ .byte_length        = usize(16),
                           .capacity           = usize(4096),
                           .stride             = usize(16),
                           .count              = usize(1),
                           .data_generation    = u64(2),
                           .storage_generation = u64(3) });
    Vec<resource::BufferContent> content;
    content.push(resource::BufferContent::from(rstd::move(bytes)));
    auto snapshot = GeometrySnapshot::from(rstd::move(layout), rstd::move(content)).unwrap();
    auto copy     = snapshot.clone();
    snapshot      = {};
    EXPECT_EQ(copy.vertexContent(usize()).as_slice().as_raw_ptr(), pointer);
    EXPECT_EQ(copy.layout().vertices[usize()].capacity, usize(4096));
    EXPECT_EQ(copy.layout().vertices[usize()].byte_length, usize(16));
    EXPECT_EQ(copy.view().vertices[usize()].bytes[usize()], u8(3));
    EXPECT_TRUE(GeometrySnapshot::from(copy.layout(), {}).is_none());
}

TEST(Geometry, RecaptureReusesUnchangedStreamsAndCountOnlyChanges) {
    Vec<u8> bytes;
    bytes.resize(usize(8), u8(2));
    GeometryView source;
    source.vertices.push({ bytes.as_slice(), usize(64), usize(4), usize(2), u64(1), u64(2) });
    auto first                     = GeometrySnapshot::capture(source);
    source.vertices[usize()].count = usize(1);
    auto second =
        GeometrySnapshot::capture(source, Some(ref<GeometrySnapshot>::from_raw_parts(&first)));
    EXPECT_EQ(first.vertexContent(usize()).as_slice().as_raw_ptr(),
              second.vertexContent(usize()).as_slice().as_raw_ptr());
    EXPECT_EQ(second.layout().vertices[usize()].count, usize(1));
    bytes[usize()] = u8(9);
    ++source.vertices[usize()].data_generation;
    auto changed =
        GeometrySnapshot::capture(source, Some(ref<GeometrySnapshot>::from_raw_parts(&second)));
    EXPECT_EQ(first.vertexContent(usize()).as_slice()[usize()], u8(2));
    EXPECT_EQ(changed.vertexContent(usize()).as_slice()[usize()], u8(9));
}

TEST(Geometry, Mutations) {
    auto attributes = Vec<VertexArray::VertexAttribute>::with_capacity(usize(2));
    attributes.push({ "position"_Str, VertexType::FLOAT3, true });
    attributes.push({ "uv"_Str, VertexType::FLOAT2, false });
    VertexArray layout(rstd::move(attributes), usize(2));
    rstd_assert(layout.Attributes().len() == usize(2));
    rstd_assert(layout.Attributes()[usize(1)].name.as_str() == "uv"_str);
    rstd_assert(layout.OneSize() == usize(6));
    rstd_assert(layout.AttributeOffset("position"_str).unwrap() == usize());
    rstd_assert(layout.AttributeOffset("uv"_str).unwrap() == usize(4 * sizeof(float)));
    rstd_assert(layout.AttributeOffset("missing"_str).is_none());
    const array<float, 10> packed { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f };
    rstd_assert(layout.AddVertex(packed.begin()));
    rstd_assert(layout.AddVertex(packed.begin() + 5));
    rstd_assert(layout.Data()[3] == 0.0f && layout.Data()[9] == 0.0f);
    const array<float, 4> uv { 0.25f, 0.5f, 0.75f, 1.0f };
    auto                  layout_generation = layout.DataGeneration();
    rstd_assert(layout.SetVertex("uv"_str, uv.as_slice()));
    rstd_assert(layout.DataGeneration() == layout_generation + u64(1));
    rstd_assert(layout.Data()[4] == 0.25f && layout.Data()[10] == 0.75f);
    rstd_assert(layout.Data()[0] == 1.0f && layout.Data()[6] == 6.0f);
    VertexArray duplicate(
        { { "uv"_Str, VertexType::FLOAT2, false }, { "uv"_Str, VertexType::FLOAT4, true } },
        usize(1));
    rstd_assert(duplicate.AttributeOffset("uv"_str).unwrap() == usize(2 * sizeof(float)));

    VertexArray vertices({ { "position"_Str, VertexType::FLOAT3, false } }, usize(2));
    rstd_assert(vertices.ID() == u32::MAX);
    auto generation = vertices.DataGeneration();
    auto written    = vertices.RewriteVertices([](VertexWriter& writer) {
        auto first = writer.AppendZeroedVertex();
        rstd_assert(first.is_some());
        (*first)[usize()] = 1.0f;
        auto second       = writer.AppendZeroedVertex();
        rstd_assert(second.is_some());
        (*second)[usize()] = 2.0f;
        rstd_assert(writer.AppendZeroedVertex().is_none());
    });
    rstd_assert(written.vertex_count == usize(2) && written.overflowed);
    rstd_assert(vertices.DataGeneration() == generation + u64(1));
    rstd_assert(vertices.VertexCount() == usize(2));
    rstd_assert(vertices.Data()[3] == 2.0f);
    const float malformed[] { 1.0f, 2.0f };
    generation = vertices.DataGeneration();
    rstd_assert(
        ! vertices.SetVertex("position"_str, slice<float>::from_raw_parts(malformed, usize(2))));
    rstd_assert(vertices.DataGeneration() == generation);
    rstd_assert(! vertices.SetVertexs(usize::MAX, {}));
    auto*       data = vertices.Data();
    VertexArray moved(rstd::move(vertices));
    rstd_assert(vertices.BufferView().bytes.is_empty());
    rstd_assert(vertices.BufferView().count == usize());
    rstd_assert(moved.Data() == data);
    rstd_assert(moved.DataGeneration() == generation);
    moved.ResetSize();
    rstd_assert(moved.VertexCount() == usize());
    rstd_assert(moved.CapacitySize() == usize(6));

    VertexArray empty({}, usize(2));
    auto        result = empty.RewriteVertices([](VertexWriter& writer) {
        rstd_assert(writer.AppendZeroedVertex().is_none());
    });
    rstd_assert(result.vertex_count == usize() && result.overflowed);

    IndexArray     indices(usize(3));
    const uint32_t input[] { 0, 1, 0 };
    auto           view = slice<uint32_t>::from_raw_parts(input, usize(3));
    indices.Assign(usize(), view);
    rstd_assert(indices.DataCount() == usize(3));
    rstd_assert(indices.RenderDataCount() == usize(3));
    indices.SetRenderDataCount(usize(2));
    rstd_assert(indices.RenderDataCount() == usize(2));
    generation = indices.DataGeneration();
    indices.Assign(usize::MAX, view);
    rstd_assert(indices.DataGeneration() == generation);
    IndexArray moved_indices(rstd::move(indices));
    rstd_assert(indices.BufferView().bytes.is_empty());
    rstd_assert(indices.BufferView().count == usize());
    rstd_assert(moved_indices.Data()[1] == 1);
    rstd_assert(moved_indices.RenderDataCount() == usize(2));
}
} // namespace vrento_test::geometry
