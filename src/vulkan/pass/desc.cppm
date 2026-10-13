export module vrento:vulkan.pass.desc;
export import :pass.desc;
import :vulkan.pass.execution;
import rstd;
export namespace vrento::vulkan
{
struct VulkanResourceUse {
    pass::ContentRef content;
    PassUsage        usage { PassUsage::Sampled };
    PassStages       stages { PassStages::Fragment };
    GraphRange       range;
    auto clone() const -> VulkanResourceUse { return { content.clone(), usage, stages, range }; }
};
struct VulkanPassIO {
    Vec<VulkanResourceUse>      inputs, outputs;
    Option<GraphicsDescription> graphics;
    LogicalQueue                queue;
    GraphSerial                 kind {};
    GraphIndex                  schema_version { 1 };
    Vec<rstd::uint8_t>          parameters;
    struct StorageReuse {
        pass::ContentRef output, input;
        auto             clone() const -> StorageReuse { return { output.clone(), input.clone() }; }
    };
    Vec<StorageReuse> storage;
    auto              logical() const -> pass::PassIO {
        pass::PassIO result;
        for (const auto& use : inputs)
            result.inputs.push({ use.content.clone(), use.range, use.usage != PassUsage::Release });
        for (const auto& use : outputs) {
            pass::Output output { use.content.clone(), use.range };
            auto         version = use.content.version();
            for (const auto& link : storage)
                if (link.output.version() == version) output.reuse = Some(link.input.clone());
            if (graphics) {
                const Option<GraphAttachment>* attachments[] { &graphics->color,
                                                               &graphics->depth,
                                                               &graphics->resolve };
                for (auto attachment : attachments)
                    if (*attachment && (**attachment).resource == version.resource &&
                        (**attachment).version == version.version)
                        output.available = (**attachment).store == AttachmentStore::Store;
            }
            result.outputs.push(rstd::move(output));
        }
        return result;
    }
    auto execution(PassId id) const -> PassExecutionInfo {
        PassExecutionInfo result { .id             = id,
                                   .kind           = kind,
                                   .schema_version = schema_version,
                                   .queue          = queue,
                                   .parameters     = parameters.clone(),
                                   .graphics       = graphics };
        auto              append = [&](const VulkanResourceUse& use, GraphAccess access) {
            auto version = use.content.version();
            result.uses.push(
                { version.resource, version.version, access, use.usage, use.stages, use.range });
        };
        for (const auto& use : inputs) append(use, GraphAccess::Read);
        for (const auto& use : outputs) append(use, GraphAccess::Write);
        return result;
    }
    auto validate() const -> Result<empty, fg::Error> {
        if (! kind || ! schema_version) return Err(fg::Error { fg::ErrorKind::Conflict });
        for (const auto& input : inputs)
            if (! input.content.valid()) return Err(fg::Error { fg::ErrorKind::MissingResource });
        for (const auto& output : outputs) {
            auto version = output.content.version();
            if (! output.content.valid() || ! version.version)
                return Err(fg::Error { fg::ErrorKind::Conflict, {}, version.resource });
            for (const auto& input : inputs)
                if (input.content.version() == version)
                    return Err(fg::Error { fg::ErrorKind::Conflict, {}, version.resource });
        }
        return Ok(empty {});
    }
    auto clone() const -> VulkanPassIO {
        return { inputs.clone(), outputs.clone(), graphics,           queue,
                 kind,           schema_version,  parameters.clone(), storage.clone() };
    }
};
template<class T>
auto describe_io(const T& value) -> VulkanPassIO {
    if constexpr (requires { value.vulkan_io(); })
        return value.vulkan_io();
    else
        return value.io.clone();
}
} // namespace vrento::vulkan
export namespace rstd
{
template<class T>
    requires requires(const T& value) {
        typename T::PassDescriptor;
        vrento::vulkan::describe_io(value);
    }
struct Impl<vrento::pass::PassDesc, T> : ImplBase<T> {
    auto IO() const -> vrento::pass::PassIO {
        return vrento::vulkan::describe_io(this->self()).logical();
    }
    auto Validate() const -> Result<empty, vrento::fg::Error> {
        if constexpr (requires { this->self().validate(); }) {
            auto valid = this->self().validate();
            if (valid.is_err()) return valid;
        }
        return vrento::vulkan::describe_io(this->self()).validate();
    }
};
} // namespace rstd
