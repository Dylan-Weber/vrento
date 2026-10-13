export module vrento:resource_registry.lifetime;
import rstd;
import :vulkan.api;
import :vulkan.device_session;
import :texture.core;

import :resource_registry.prepared;

export namespace vrento::resource_registry
{

using namespace rstd::prelude;

struct RegistryBufferOwner {
    vulkan::DeviceSession session;
    vulkan::BufferManager manager;

    explicit RegistryBufferOwner(vulkan::DeviceSession device_session)
        : session(rstd::move(device_session)), manager(session.device()) {}
};

struct FrameSubmissionLease {
    rstd::sync::Arc<RegistryBufferOwner>   buffer_owner;
    PreparedResourceLeases                 resources;
    texture::SubmissionReceipt             receipt;
    vulkan::RecordedBufferUploads          recorded_buffers;
    Option<vulkan::BufferUploadBatchLease> buffer_uploads;
    bool                                   uploads_prepared {}, uploads_recorded {};

    FrameSubmissionLease(rstd::sync::Arc<RegistryBufferOwner> owner,
                         PreparedResourceLeases prepared, texture::SubmissionReceipt submitted)
        : buffer_owner(rstd::move(owner)),
          resources(rstd::move(prepared)),
          receipt(rstd::move(submitted)) {}
};

} // namespace vrento::resource_registry
