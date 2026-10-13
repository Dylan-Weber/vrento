export module vrento:resource.handle;
import rstd;

using namespace rstd::prelude;

export namespace vrento::resource
{

template<typename Tag>
struct ResourceHandle {
    u64 index { u64::MAX };
    u64 generation {};

    bool Valid() const noexcept { return index != u64::MAX && generation != u64(); }

    friend bool operator==(const ResourceHandle&, const ResourceHandle&) = default;
};

template<typename T, typename Tag>
class ResourceReference {
    struct Definition {
        ResourceHandle<Tag> id;
        u64                 revision { 1 };
        T                   value;
    };
    Option<rstd::sync::Arc<Definition>> data_;
    explicit ResourceReference(rstd::sync::Arc<Definition> data): data_(Some(rstd::move(data))) {}

public:
    ResourceReference() = default;
    ResourceReference(T value) {
        static rstd::sync::atomic::Atomic<u64> next;
        auto index = next.fetch_add(u64(1), rstd::sync::atomic::Ordering::Relaxed);
        data_      = Some(rstd::sync::Arc<Definition>::make(
            Definition { { index, u64(1) }, u64(1), rstd::move(value) }));
    }
    ResourceReference(const ResourceReference& other): data_(other.data_.clone()) {}
    ResourceReference(ResourceReference&&) = default;
    auto operator=(const ResourceReference& other) -> ResourceReference& {
        data_ = other.data_.clone();
        return *this;
    }
    auto operator=(ResourceReference&&) -> ResourceReference& = default;
    auto valid() const -> bool { return data_.is_some(); }
    auto id() const -> ResourceHandle<Tag> { return data_ ? (*data_)->id : ResourceHandle<Tag> {}; }
    auto revision() const -> u64 { return data_ ? (*data_)->revision : u64(); }
    auto definition() const -> const T& { return (**data_).value; }
    auto clone() const -> ResourceReference { return ResourceReference(*this); }
    auto with_definition(T value) const -> ResourceReference {
        if (! data_) return ResourceReference(rstd::move(value));
        return ResourceReference(rstd::sync::Arc<Definition>::make(
            Definition { (*data_)->id, (*data_)->revision + u64(1), rstd::move(value) }));
    }
};
struct TextureResourceTag;
struct BufferResourceTag;
struct TextureHandleTag;
struct BufferHandleTag;
struct PipelineHandleTag;
struct PipelineRecipeHandleTag;
struct ShaderHandleTag;
struct RenderPassHandleTag;
struct FramebufferHandleTag;
struct ResourceUseHandleTag;
struct TextureUseHandleTag;
struct BufferUseHandleTag;
struct ShaderUseHandleTag;
struct PipelineUseHandleTag;
struct RenderPassUseHandleTag;
struct FramebufferUseHandleTag;
struct ExternalUseHandleTag;
struct DescriptorLayoutHandleTag;
struct PipelineLayoutHandleTag;
struct DescriptorBindingHandleTag;

using TextureHandle           = ResourceHandle<TextureHandleTag>;
using BufferHandle            = ResourceHandle<BufferHandleTag>;
using PipelineHandle          = ResourceHandle<PipelineHandleTag>;
using PipelineRecipeHandle    = ResourceHandle<PipelineRecipeHandleTag>;
using ShaderHandle            = ResourceHandle<ShaderHandleTag>;
using RenderPassHandle        = ResourceHandle<RenderPassHandleTag>;
using FramebufferHandle       = ResourceHandle<FramebufferHandleTag>;
using ResourceUseHandle       = ResourceHandle<ResourceUseHandleTag>;
using TextureUseHandle        = ResourceHandle<TextureUseHandleTag>;
using BufferUseHandle         = ResourceHandle<BufferUseHandleTag>;
using ShaderUseHandle         = ResourceHandle<ShaderUseHandleTag>;
using PipelineUseHandle       = ResourceHandle<PipelineUseHandleTag>;
using RenderPassUseHandle     = ResourceHandle<RenderPassUseHandleTag>;
using FramebufferUseHandle    = ResourceHandle<FramebufferUseHandleTag>;
using ExternalUseHandle       = ResourceHandle<ExternalUseHandleTag>;
using DescriptorLayoutHandle  = ResourceHandle<DescriptorLayoutHandleTag>;
using PipelineLayoutHandle    = ResourceHandle<PipelineLayoutHandleTag>;
using DescriptorBindingHandle = ResourceHandle<DescriptorBindingHandleTag>;

} // namespace vrento::resource

export namespace rstd
{

template<typename Tag>
struct Impl<Copy, vrento::resource::ResourceHandle<Tag>> {};

template<typename Tag>
struct Impl<hash::Hash, vrento::resource::ResourceHandle<Tag>>
    : ImplBase<vrento::resource::ResourceHandle<Tag>> {
    template<typename H>
        requires Impled<H, hash::Hasher>
    void hash(H& state) const noexcept {
        hash::hash_into(this->self().index, state);
        hash::hash_into(this->self().generation, state);
    }
};

} // namespace rstd
