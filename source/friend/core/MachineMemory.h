#pragma once
#include "NativeChorus.h"

namespace monofx::core
{
// One track reuses the same machine storage on assignment. Initializers only
// replace the fields they own; feedback/filter history in other fields remains
// observable during transitions. This storage contains native state only.
struct MachineMemory
{
    static constexpr unsigned base=0x528,size=0x99,ringBase=0x114000;
    std::array<native::Word,size> x{},y{};
    std::array<native::Word,16384> ring{};
};
}
