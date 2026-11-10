#pragma once
#include "NativeChorus.h"

namespace monofx::core
{
// The firmware reuses its low-memory working area between shared stages.
// Some interpolation paths read entries beyond their current audio span, so
// preserving this workspace is part of the signal algorithm's behavior.
// Each native track owns its own storage; it is never a firmware snapshot.
struct AudioWorkspace
{
    std::array<native::Word,256> x{},y{};
    template<typename State> void importInto(State& state) const noexcept
    {
        std::copy(x.begin(),x.end(),state.mx.begin());std::copy(y.begin(),y.end(),state.my.begin());
    }
    template<typename State> void exportFrom(const State& state) noexcept
    {
        std::copy_n(state.mx.begin(),x.size(),x.begin());std::copy_n(state.my.begin(),y.size(),y.begin());
    }
};
}
