#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

import rstd;
import rstd.test;
import vrento;

#include "../src/vulkan/impl/ExtensionNames.hpp"

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::BTreeSet;
using rstd::ffi::CString;
using vrento::vulkan::Extension;
using vrento::vulkan::detail::SelectExtensionNames;

namespace vrento_test::extension_names
{
TEST(ExtensionNames, Selection) {
    auto supported = BTreeSet<String>::make();
    supported.insert("VK_test_b"_Str);
    supported.insert("VK_test_a"_Str);
    Vec<CString> owned;
    {
        auto                dynamic_name = "VK_test_b"_Str;
        array<Extension, 4> requested {
            Extension { true, dynamic_name.as_str() },
            Extension { false, "VK_test_missing"_str },
            Extension { false, "VK_test_a"_str },
            Extension { true, "VK_test_b"_str },
        };
        auto selected = SelectExtensionNames(supported, requested.as_slice()).unwrap();
        rstd_assert(selected.len() == usize(2));
        owned = selected.clone();
    }
    rstd_assert(owned[usize()].as_ref().to_str().unwrap() == "VK_test_a"_str);
    rstd_assert(owned[usize(1)].as_ref().to_str().unwrap() == "VK_test_b"_str);
    rstd_assert(owned[usize()].as_ptr()[9] == '\0');

    array<Extension, 2> missing {
        Extension { true, "VK_test_first_missing"_str },
        Extension { true, "VK_test_second_missing"_str },
    };
    auto rejected = SelectExtensionNames(supported, missing.as_slice());
    rstd_assert(rejected.is_err());
    rstd_assert(rejected.unwrap_err() == "VK_test_first_missing"_str);
    rstd_assert(SelectExtensionNames(supported, {}).unwrap().is_empty());

    supported.insert("invalid\0name"_Str);
    array<Extension, 1> embedded_nul { Extension { true, "invalid\0name"_str } };
    rstd_assert(SelectExtensionNames(supported, embedded_nul.as_slice()).is_err());

    vrento::vulkan::Instance instance;
    vrento::vulkan::Device   device;
    rstd_assert(instance.enabled_extensions().is_empty());
    rstd_assert(device.enabled_instance_extensions().is_empty());
    rstd_assert(device.enabled_device_extensions().is_empty());
    rstd_assert(! instance.supportExt("VK_test_a"_str));
    rstd_assert(! instance.supportLayer("VK_test_a"_str));
    rstd_assert(! device.supportExt("VK_test_a"_str));

    int  calls = 0;
    auto check = [count = 0, &calls](const vvk::PhysicalDevice&) mutable {
        calls = ++count;
        return true;
    };
    auto callback = vrento::vulkan::CheckGpuOp::from_ref(check);
    rstd_assert(callback.as_mut_ref()->operator()(vvk::PhysicalDevice {}));
    rstd_assert(calls == 1);
    rstd_assert(callback.as_mut_ref()->operator()(vvk::PhysicalDevice {}));
    rstd_assert(calls == 2);
}
} // namespace vrento_test::extension_names

namespace vrento_test::device_session
{
using namespace vrento::vulkan;

struct NativeWindow {
    int* dropped;

    explicit NativeWindow(int& count): dropped(&count) {}
    NativeWindow(const NativeWindow&) = delete;
    NativeWindow(NativeWindow&& other): dropped(other.dropped) { other.dropped = nullptr; }
    ~NativeWindow() {
        if (dropped) ++*dropped;
    }
};

struct SurfaceFactory {
    VkInstance instance {};
    int        dropped {};
    bool       fail { true };

    auto create(VkInstance value) -> Result<SessionSurface, DeviceSessionError> {
        instance = value;
        if (fail)
            return Err(
                DeviceSessionError { DeviceSessionStage::Surface, VK_ERROR_OUT_OF_HOST_MEMORY });
        return Ok(SessionSurface { VK_NULL_HANDLE,
                                   Box<dyn<SurfaceLifetime>>::make(NativeWindow(dropped)) });
    }
};
} // namespace vrento_test::device_session

namespace rstd
{
template<>
struct Impl<vrento::vulkan::SessionSurfaceFactory, vrento_test::device_session::SurfaceFactory>
    : ImplBase<vrento_test::device_session::SurfaceFactory> {
    auto create(VkInstance instance)
        -> Result<vrento::vulkan::SessionSurface, vrento::vulkan::DeviceSessionError> {
        return this->self().create(instance);
    }
};
} // namespace rstd

namespace vrento_test::device_session
{
TEST(DeviceSession, InvalidOptions) {
    rstd_assert(DeviceSession::create({ .extent = { 0, 1 } }).unwrap_err().stage ==
                DeviceSessionStage::Invalid);
    rstd_assert(DeviceSession::create({ .extent = { 1, 0 } }).unwrap_err().stage ==
                DeviceSessionStage::Invalid);
    array<rstd::uint8_t, VK_UUID_SIZE - 1> uuid {};
    rstd_assert(
        DeviceSession::create({ .physical_device_uuid = uuid.as_slice() }).unwrap_err().stage ==
        DeviceSessionStage::Invalid);
}

TEST(DeviceSession, DefaultsAndSharedOwner) {
    Option<DeviceSession> retained;
    VkDevice              handle {};
    rstd::uint64_t        epoch {};
    {
        auto session = DeviceSession::create().unwrap();
        rstd_assert(session.valid() && ! session.has_surface());
        rstd_assert(session.instance_owner().api_version() == WP_VULKAN_VERSION);
        rstd_assert(session.instance() == *session.instance_owner().inst());
        rstd_assert(session.device().instance_handle() == session.instance());
        rstd_assert(session.device().instance_api_version() == WP_VULKAN_VERSION);
        handle     = *session.device().handle();
        epoch      = session.epoch();
        auto moved = rstd::move(session);
        rstd_assert(! session.valid() && moved.valid());
        auto copied = moved;
        copied      = moved;
        rstd_assert(&copied.device() == &moved.device());
        rstd_assert(&copied.instance_owner() == &moved.instance_owner());
        rstd_assert(copied.epoch() == epoch);
        retained = Some(rstd::move(copied));
    }
    rstd_assert(*retained->device().handle() == handle && retained->epoch() == epoch);
    rstd_assert(retained->wait_idle() == VK_SUCCESS);
    retained->release_surface();
    rstd_assert(retained->valid() && ! retained->has_surface());
}

TEST(DeviceSession, UuidAndApiVersion) {
    Instance probe;
    Instance::TryCreate(probe, {}, {}).unwrap();
    array<Extension, 1> extensions { Extension { true, "VK_KHR_timeline_semaphore"_str } };
    array<rstd::uint8_t, VK_UUID_SIZE> uuid {};
    auto                               api_version = WP_VULKAN_VERSION;
    bool                               found {};
    for (const auto& gpu : probe.inst().EnumeratePhysicalDevices()) {
        if (! Device::CheckGPU(gpu, extensions.as_slice(), VK_NULL_HANDLE)) continue;
        VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2  properties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                  &id };
        gpu.GetProperties2KHR(properties);
        for (unsigned i = 0; i < VK_UUID_SIZE; ++i) uuid[usize(i)] = id.deviceUUID[i];
        api_version = properties.properties.apiVersion >= VK_API_VERSION_1_2 ? VK_API_VERSION_1_2
                                                                             : WP_VULKAN_VERSION;
        found       = true;
    }
    rstd_assert(found);
    auto session = DeviceSession::create({ .physical_device_uuid = uuid.as_slice(),
                                           .api_version          = api_version,
                                           .extent               = { 4, 8 },
                                           .validation           = true })
                       .unwrap();
    VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2  properties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
    session.device().gpu().GetProperties2KHR(properties);
    rstd_assert(uuid.as_slice() ==
                slice<rstd::uint8_t>::from_raw_parts(id.deviceUUID, usize(VK_UUID_SIZE)));
    rstd_assert(session.instance_owner().api_version() == api_version);
    rstd_assert(session.device().instance_api_version() == api_version);
    rstd_assert(session.device().out_extent().width == 4 &&
                session.device().out_extent().height == 8);
    rstd_assert(session.validation_errors() == 0);

    array<Extension, 1> unavailable { Extension { true, "VK_test_missing"_str } };
    rstd_assert(DeviceSession::create({ .device_extensions    = unavailable.as_slice(),
                                        .physical_device_uuid = uuid.as_slice() })
                    .unwrap_err()
                    .stage == DeviceSessionStage::Device);
}

TEST(DeviceSession, SurfaceFactoryFailures) {
    SurfaceFactory factory;
    auto           surface = dyn<SessionSurfaceFactory>::from_ref(factory);
    auto           failed  = DeviceSession::create({}, Some(surface.as_mut_ref())).unwrap_err();
    rstd_assert(factory.instance != VK_NULL_HANDLE);
    rstd_assert(failed.stage == DeviceSessionStage::Surface &&
                failed.result == VK_ERROR_OUT_OF_HOST_MEMORY);
    factory.fail = false;
    auto invalid = DeviceSession::create({}, Some(surface.as_mut_ref())).unwrap_err();
    rstd_assert(invalid.stage == DeviceSessionStage::Surface);
    rstd_assert(factory.dropped == 1);
}
} // namespace vrento_test::device_session
