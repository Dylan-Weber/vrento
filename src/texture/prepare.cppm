export module vrento:texture.prepare;
export import :texture.core;
export import :texture.image;
import rstd;

export namespace vrento::texture
{
inline auto describe_image_slot(const ImageHeader& header, ImageSlotDescription slot)
    -> Result<TextureDesc, Error> {
    if (slot.width <= 0 || slot.height <= 0 || slot.mip_count == usize() ||
        slot.mip_count > usize(32))
        return Err(Error { ErrorKind::Invalid });
    TextureDesc desc;
    desc.extent     = { Count(slot.width), Count(slot.height) };
    desc.mip_levels = header.kind == ImageKind::Video ? 1 : Count(slot.mip_count.to_primitive());
    desc.format     = header.kind == ImageKind::Video ? TextureFormat::RGBA8 : header.format;
    auto checked    = validate(desc);
    if (checked.is_err()) return Err(checked.unwrap_err());
    return Ok(rstd::move(desc));
}

inline auto describe_image(const ImageHeader& header, slice<ImageSlotDescription> slots)
    -> Result<Vec<TextureDesc>, Error> {
    if (slots.is_empty()) return Err(Error { ErrorKind::Invalid });
    Vec<TextureDesc> result;
    for (const auto& slot : slots) {
        auto desc = describe_image_slot(header, slot);
        if (desc.is_err()) return Err(desc.unwrap_err());
        result.push(rstd::move(desc).unwrap());
    }
    return Ok(rstd::move(result));
}

inline auto describe_image(const vrento::Image& image) -> Result<Vec<TextureDesc>, Error> {
    if (image.slots.is_empty()) return Err(Error { ErrorKind::Invalid });
    Vec<TextureDesc> result;
    for (const auto& slot : image.slots) {
        auto desc =
            describe_image_slot(image.header, { slot.width, slot.height, slot.mipmaps.len() });
        if (desc.is_err()) return Err(desc.unwrap_err());
        result.push(rstd::move(desc).unwrap());
    }
    return Ok(rstd::move(result));
}
struct PreparedImage {
    TextureViewRef     view;
    SamplerRef         sampler;
    Vec<TextureUpload> uploads;
};
class ImagePrepareContext {
    mut_ref<dyn<TextureDevice>> device_;

public:
    explicit ImagePrepareContext(mut_ref<dyn<TextureDevice>> device): device_(device) {}
    auto PrepareImage(const vrento::Image& image) -> Result<Vec<PreparedImage>, Error> {
        if (image.header.kind != ImageKind::Pixels || image.slots.is_empty())
            return Err(Error { ErrorKind::Unsupported });
        auto descriptions = describe_image(image);
        if (descriptions.is_err()) return Err(descriptions.unwrap_err());
        Vec<PreparedImage> result;
        for (usize index {}; index < image.slots.len(); ++index) {
            const auto& slot   = image.slots[index];
            const auto& desc   = (*descriptions)[index];
            auto        layout = format_layout(desc.format);
            for (Count mip = 0; mip < desc.mip_levels; ++mip) {
                auto&  source = slot.mipmaps[usize(mip)];
                auto   extent = mip_extent(desc, mip);
                Serial row =
                    ((Serial(extent.width) + layout.block_width - 1) / layout.block_width) *
                    layout.block_bytes;
                Serial rows =
                    (Serial(extent.height) + layout.block_height - 1) / layout.block_height;
                if (rows > ~Serial {} / row) return Err(Error { ErrorKind::Invalid });
                Serial size = row * rows;
                if (source.width != rstd::int32_t(extent.width) ||
                    source.height != rstd::int32_t(extent.height) || ! source.data ||
                    source.size.to_primitive() < 0 || Serial(source.size.to_primitive()) != size)
                    return Err(Error { ErrorKind::Invalid });
            }
            auto allocated = device_->CreateTexture(desc);
            if (allocated.is_err()) return Err(allocated.unwrap_err());
            auto view =
                device_->CreateView(allocated.unwrap(), { Dimension::D2, { layout.aspect } });
            if (view.is_err()) return Err(view.unwrap_err());
            auto sampler = device_->CreateSampler({ image.header.sample });
            if (sampler.is_err()) return Err(sampler.unwrap_err());
            PreparedImage prepared { view.unwrap(), sampler.unwrap() };
            for (Count mip = 0; mip < desc.mip_levels; ++mip) {
                auto&         source = slot.mipmaps[usize(mip)];
                TextureUpload upload;
                upload.target = prepared.view;
                upload.mip    = mip;
                upload.width  = Count(source.width);
                upload.height = Count(source.height);
                upload.bytes  = Vec<u8>::from(
                    slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(source.data.get()),
                                              usize(source.size.to_primitive())));
                prepared.uploads.push(rstd::move(upload));
            }
            result.push(rstd::move(prepared));
        }
        return Ok(rstd::move(result));
    }
};
struct TextureContentPreparer {
    using Trait                  = TextureContentPreparer;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = TextureContentPreparer;
        auto PrepareImage(const vrento::Image& image) -> Result<Vec<PreparedImage>, Error> {
            return rstd::trait_call<0>(this, image);
        }
    };
    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::PrepareImage>;
};
} // namespace vrento::texture
export namespace rstd
{
template<>
struct Impl<vrento::texture::TextureContentPreparer, vrento::texture::ImagePrepareContext>
    : ImplBase<vrento::texture::ImagePrepareContext> {
    auto PrepareImage(const vrento::Image& image)
        -> Result<prelude::Vec<vrento::texture::PreparedImage>, vrento::texture::Error> {
        return self().PrepareImage(image);
    }
};
} // namespace rstd
