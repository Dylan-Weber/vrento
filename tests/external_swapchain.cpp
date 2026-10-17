#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>
#include <fcntl.h>
#include <unistd.h>

import rstd;
import rstd.test;
import vrento;

using namespace rstd::prelude;
using namespace vrento;
using namespace rstd::literals;
using rstd::sync::Arc;
using rstd::sync::atomic::Atomic;

namespace vrento_test::external_swapchain
{
struct Counts {
    int  destroyed {};
    int  submitted {};
    int  aborted {};
    bool fail {};
};

class TestSwapchain final : public ExSwapchain {
public:
    explicit TestSwapchain(Counts& counts): counts_(counts) {}
    ~TestSwapchain() override { ++counts_.destroyed; }
    unsigned                  width() const override { return 1; }
    unsigned                  height() const override { return 1; }
    VkFormat                  format() const override { return VK_FORMAT_R8G8B8A8_UNORM; }
    bool                      ready() const override { return true; }
    void                      setOnReadyChanged(Option<ExSwapchainReadyCallback>) override {}
    FrameSurfaceAcquireResult acquireRenderTarget() override {
        FrameSurfaceIdentity identity { u64(1), u32(), u64(++serial_) };
        return { .completion = MakeCompletionCapability(identity) };
    }

private:
    FrameSurfaceCompletionResult CompleteRendered(FrameSurfaceIdentity identity, int fd) override {
        if (fd >= 0) ::close(fd);
        ++counts_.submitted;
        if (counts_.fail)
            return { .status   = FrameSurfaceCompletionStatus::SessionLost,
                     .identity = identity,
                     .error    = Some(FrameSurfaceError {
                         FrameSurfaceErrorKind::SessionLost,
                         Some(Arc<dyn<rstd::error::Error>>::make(vulkan::BackendError {
                             "submit surface"_Str,
                             vulkan::BackendCause::Native(VK_ERROR_DEVICE_LOST) })) }) };
        return { .status = FrameSurfaceCompletionStatus::Submitted, .identity = identity };
    }
    FrameSurfaceCompletionResult AbortRenderTarget(FrameSurfaceIdentity identity) override {
        ++counts_.aborted;
        if (counts_.fail)
            return { .status = FrameSurfaceCompletionStatus::StaleIdentity, .identity = identity };
        return { .status = FrameSurfaceCompletionStatus::Aborted, .identity = identity };
    }
    Counts&        counts_;
    rstd::uint64_t serial_ {};
};

class TestTriple final : public TripleSwapchain<int> {
public:
    TestTriple(): presented_(&values[0]), ready_(&values[1]), inprogress_(&values[2]) {}
    unsigned width() const override { return 1; }
    unsigned height() const override { return 1; }

private:
    Atomic<int*>& presented() override { return presented_; }
    Atomic<int*>& ready() override { return ready_; }
    Atomic<int*>& inprogress() override { return inprogress_; }
    int           values[3] {};
    Atomic<int*>  presented_;
    Atomic<int*>  ready_;
    Atomic<int*>  inprogress_;
};

TEST(ExternalSwapchain, Lifetime) {
    Counts submitted;
    {
        auto owner = ExSwapchain::Make<TestSwapchain>(submitted);
        auto weak  = owner.downgrade();
        auto frame = owner->AsSwapchain().acquireRenderTarget();
        owner.reset();
        rstd_assert(! weak.expired() && submitted.destroyed == 0);
        rstd_assert(frame.completion.Submit(-1).status == FrameSurfaceCompletionStatus::Submitted);
        rstd_assert(weak.expired() && submitted.destroyed == 1 && submitted.submitted == 1);
        rstd_assert(! frame.completion.valid());
        rstd_assert(frame.completion.Abort().status == FrameSurfaceCompletionStatus::NotPending);
        int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        rstd_assert(fd >= 0);
        (void)frame.completion.Submit(fd);
        rstd_assert(::fcntl(fd, F_GETFD) == -1);
    }
    Counts aborted;
    {
        auto owner       = ExSwapchain::Make<TestSwapchain>(aborted);
        auto first       = owner->AsSwapchain().acquireRenderTarget();
        auto second      = owner->AsSwapchain().acquireRenderTarget();
        first.completion = rstd::move(second.completion);
        rstd_assert(aborted.aborted == 1 && ! second.completion.valid());
        owner.reset();
        rstd_assert(aborted.destroyed == 0);
    }
    rstd_assert(aborted.aborted == 2 && aborted.destroyed == 1);
    Counts unused;
    {
        auto owner = ExSwapchain::Make<TestSwapchain>(unused);
    }
    rstd_assert(unused.destroyed == 1);
    TestTriple triple;
    auto       slots = triple.snapshot_all_slots();
    rstd_assert(triple.eatFrame() == nullptr);
    rstd_assert(triple.getInprogress() == slots[usize(2)]);
    triple.renderFrame();
    rstd_assert(triple.getInprogress() == slots[usize(1)]);
    rstd_assert(triple.eatFrame() == slots[usize(2)]);
    rstd_assert(triple.eatFrame() == nullptr);
}
TEST(ExternalSwapchain, SurfaceErrors) {
    FrameSurfaceAcquireResult waiting;
    rstd_assert(! waiting.acquired() && ! waiting.error);
    FrameSurfaceAcquireResult busy { .status = FrameSurfaceAcquireStatus::Busy };
    rstd_assert(! busy.acquired() && ! busy.error);
    FrameSurfaceAcquireResult lost { .status = FrameSurfaceAcquireStatus::SessionLost };
    rstd_assert(! lost.acquired() && lost.error->kind == FrameSurfaceErrorKind::SessionLost);
    FrameSurfaceAcquireResult protocol { .status = FrameSurfaceAcquireStatus::ProtocolError };
    rstd_assert(! protocol.acquired() &&
                protocol.error->kind == FrameSurfaceErrorKind::ProtocolFailure);
    auto local = ExSwapchain::Make<vulkan::LocalExSwapchain>(
        rstd::array<vulkan::LocalExHandle, 3> {}, VkExtent2D { 1, 1 }, 0u);
    auto invalid = local->AsSwapchain().acquireRenderTarget();
    rstd_assert(! invalid.acquired() && ! invalid.completion.valid());
    rstd_assert(invalid.error && invalid.error->kind == FrameSurfaceErrorKind::InvalidLease);
    rstd_assert(rstd::as<rstd::error::Error>(*invalid.error).source().is_none());

    FrameSurfaceCompletionCapability empty;
    auto                             absent = empty.Abort();
    rstd_assert(! absent.completed() && absent.error->kind == FrameSurfaceErrorKind::NotPending);
    FrameSurfaceCompletionResult success { .status = FrameSurfaceCompletionStatus::Submitted };
    rstd_assert(success.completed() && ! success.error);

    Counts counts { .fail = true };
    auto   owner   = ExSwapchain::Make<TestSwapchain>(counts);
    auto   frame   = owner->AsSwapchain().acquireRenderTarget();
    auto   aborted = frame.completion.Abort();
    rstd_assert(aborted.error->kind == FrameSurfaceErrorKind::StaleIdentity);
    frame = owner->AsSwapchain().acquireRenderTarget();
    owner.reset();
    int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    rstd_assert(fd >= 0);
    auto failed = frame.completion.Submit(fd);
    rstd_assert(::fcntl(fd, F_GETFD) == -1);
    rstd_assert(! frame.completion.valid() && counts.destroyed == 1);
    rstd_assert(! failed.completed() && failed.error->kind == FrameSurfaceErrorKind::SessionLost);
    auto saved   = failed.error->clone();
    failed.error = None();
    auto cause   = rstd::as<rstd::error::Error>(saved).source();
    rstd_assert(cause.is_some());
    rstd_assert(rstd::format("{}", saved).as_str().contains("submit surface"_str));
    rstd_assert(rstd::format("{}", *cause).as_str().contains("submit surface"_str));
}
} // namespace vrento_test::external_swapchain
