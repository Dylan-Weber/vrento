export module vrento:uniform.binding;
export import :uniform.source;
export import :uniform.buffer;
import :resource;
import rstd;

using namespace rstd::prelude;

export namespace vrento
{
struct BoundUniformOutput {
    UniformOutputId output;
    usize           slot_index {};
};

struct BoundUniformSource {
    UniformSourceOwner                    owner;
    ref<dyn<UniformSource>>               source;
    i32                                   priority {};
    Vec<BoundUniformOutput>               outputs;
    Option<Box<dyn<UniformBindingLease>>> lease;
    u64                                   version {};
    bool                                  evaluated { false };
};

auto PrepareUniformSource(const UniformBufferLayout&, UniformSourceOwner, i32 priority,
                          ShaderMatrixConvention, ShaderMatrixAbi)
    -> Result<Option<BoundUniformSource>, UniformBufferUpdateError>;

struct UniformParameter {
    ref<str>         name;
    UniformValueView value;
};

// How often uniform blocks were evaluated, uploaded, or found equal to the block already uploaded
// (and so not uploaded) or unchanged by their sources' versions; process-wide, for diagnostics.
struct UniformUpdateStats {
    rstd::uint64_t updates {}, evaluated {}, uploaded {}, identical {};
};
auto uniform_update_stats() -> UniformUpdateStats;

class UniformBinding {
public:
    UniformBinding(resource::BufferUseHandle, UniformBufferLayout, Vec<BoundUniformSource>,
                   ShaderMatrixConvention, ShaderMatrixAbi);
    auto Buffer() const -> resource::BufferUseHandle { return m_buffer; }
    auto SetParameters(slice<UniformParameter>) -> Result<empty, UniformBufferUpdateError>;
    auto Update(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<resource::BufferContentWriter>>)
        -> Result<empty, UniformBufferUpdateError>;

private:
    resource::BufferUseHandle m_buffer;
    UniformBufferLayout       m_layout;
    Vec<BoundUniformSource>   m_sources;
    Vec<u8>                   m_base;
    // Kept between updates: a binding is updated every frame it changes, and a block and a list of
    // versions allocated each time were most of its allocations.
    Vec<u8>                   m_block;
    Vec<u8>                   m_uploaded;
    bool                      m_has_uploaded { false };
    Vec<u64>                  m_versions;
    ShaderMatrixConvention    m_convention;
    ShaderMatrixAbi           m_abi;
    bool                      m_dirty { true };
};
} // namespace vrento
