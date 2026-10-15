export module vrento:preparation;
import rstd;

using namespace rstd::prelude;

export namespace vrento
{
struct PendingPreparation {};
template<typename T>
class Preparation {
    Option<T> value_;

public:
    Preparation(PendingPreparation) {}
    Preparation(T value): value_(Some(rstd::move(value))) {}
    auto ready() const -> bool { return value_.is_some(); }
    auto take_ready() -> T { return value_.take().unwrap(); }
};

} // namespace vrento
export namespace vrento::vulkan
{
using vrento::PendingPreparation;
using vrento::Preparation;
} // namespace vrento::vulkan
