export module vrento.texture_image;
export import vrento.texture;
export import vrento.image;
import rstd;

export namespace vrento::texture
{
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
        Vec<PreparedImage> result;
        for (const auto& slot : image.slots) {
            if (slot.width <= 0 || slot.height <= 0 || slot.mipmaps.is_empty() ||
                slot.mipmaps.len().to_primitive() > 32)
                return Err(Error { ErrorKind::Invalid });
            TextureDesc desc;
            desc.extent     = { Count(slot.width), Count(slot.height) };
            desc.mip_levels = Count(slot.mipmaps.len().to_primitive());
            desc.format     = image.header.format;
            auto checked    = validate(desc);
            if (checked.is_err()) return Err(checked.unwrap_err());
            auto layout = format_layout(desc.format);
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
                upload.bytes.reserve(usize(source.size.to_primitive()));
                for (Serial i = 0; i < Serial(source.size.to_primitive()); ++i)
                    upload.bytes.push(u8(source.data.get()[i]));
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
