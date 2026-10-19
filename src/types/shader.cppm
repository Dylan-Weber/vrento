export module vrento:types.shader;
import rstd;

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace vrento
{

class ShaderCode {
    struct Storage {
        const Vec<rstd::uint32_t> words;
        explicit Storage(Vec<rstd::uint32_t> value): words(rstd::move(value)) {}
    };
    Option<Arc<Storage>> storage_;

public:
    ShaderCode() = default;

    static auto from(Vec<rstd::uint32_t> words) -> ShaderCode {
        ShaderCode result;
        if (! words.is_empty()) result.storage_ = Some(Arc<Storage>::make(rstd::move(words)));
        return result;
    }
    static auto from(slice<rstd::uint32_t> words) -> ShaderCode {
        return from(Vec<rstd::uint32_t>::from(words));
    }
    auto as_slice() const -> slice<rstd::uint32_t> {
        return storage_ ? (*storage_)->words.as_slice() : slice<rstd::uint32_t> {};
    }
    auto len() const -> usize { return as_slice().len(); }
    auto capacity() const -> usize { return storage_ ? (*storage_)->words.capacity() : usize(); }
    auto is_empty() const -> bool { return len() == usize(); }
    auto data() const -> const rstd::uint32_t* { return as_slice().as_raw_ptr(); }
    auto begin() const -> const rstd::uint32_t* { return data(); }
    auto end() const -> const rstd::uint32_t* {
        return is_empty() ? data() : data() + len().to_primitive();
    }
    auto operator[](usize index) const -> const rstd::uint32_t& { return as_slice()[index]; }
    auto clone() const -> ShaderCode {
        ShaderCode result;
        if (storage_) result.storage_ = Some(storage_->clone());
        return result;
    }
    void clone_from(const ShaderCode& other) { *this = other.clone(); }

    friend bool operator==(const ShaderCode& lhs, const ShaderCode& rhs) {
        if (lhs.data() == rhs.data()) return true;
        if (lhs.len() != rhs.len()) return false;
        return rstd::mem::memcmp(
                   lhs.data(), rhs.data(), lhs.len() * usize(sizeof(rstd::uint32_t))) == 0;
    }
};

enum class ShaderType
{
    VERTEX,
    GEOMETRY,
    FRAGMENT
};
enum class ShaderScalarKind
{
    Unknown,
    Float,
    SignedInteger,
    UnsignedInteger,
    Boolean
};
enum class ShaderMatrixMajor
{
    None,
    Row,
    Column
};
enum class ShaderMatrixConvention
{
    ColumnVector,
    RowVector
};
enum class ShaderMatrixAbi
{
    NativeSpirv,
    Hlsl
};

} // namespace vrento
