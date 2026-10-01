#pragma once
#ifdef _MSC_VER
#include <iterator>
namespace stdext {
    template <class _Iterator>
    constexpr auto make_checked_array_iterator(_Iterator _Iter, size_t) {
        return _Iter;
    }
}
#endif
