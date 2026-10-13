export module vrento:pass.desc;
import rstd;
export import :pass.content;
import :frame_graph.types;
using namespace rstd::prelude;

export namespace vrento::pass
{
struct Input {
    ContentRef        source;
    fg::ResourceRange range;
    bool              requires_contents { true };
};
struct Output {
    ContentRef         target;
    fg::ResourceRange  range;
    Option<ContentRef> reuse;
    bool               available { true };
};
struct PassIO {
    Vec<Input>  inputs;
    Vec<Output> outputs;
};
struct PassDesc {
    using Trait                  = PassDesc;
    static constexpr bool direct = false;
    template<class Self, class = void>
    struct Api {
        using Trait = PassDesc;
        auto IO() const -> PassIO { return rstd::trait_call<0>(this); }
        auto Validate() const -> Result<empty, fg::Error> { return rstd::trait_call<1>(this); }
    };
    template<class T>
    using Funcs = TraitFuncs<&T::IO, &T::Validate>;
};
template<typename T>
auto try_desc(ref<dyn<PassDesc>> value) -> Option<ref<T>> {
    if (value.concrete_type_id() != rstd::any::TypeId::of<T>()) return None();
    return Some(ref<T>::from_raw_parts(static_cast<const T*>(value.as_raw_ptr())));
}
} // namespace vrento::pass
