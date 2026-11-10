#pragma once
#include "NativeChorus.h"
#include "MachineMemory.h"

namespace monofx::core
{
using ChorusControls = std::array<native::Word,8>;
using StereoBlock = std::array<native::Word,32>;
using MachineScratch = std::array<native::Word,6>;
struct AudioWorkspace;

const native::Tables& chorusTables() noexcept;

// Owns the chorus algorithm's complete persistent state. Controls are words at
// the DSP commit boundary; MIDI/knob admission belongs to the shared controller.
class Chorus
{
public:
    void reset(const ChorusControls& controls) noexcept;
    void activate(const ChorusControls&,const MachineMemory&) noexcept;
    void save(MachineMemory&) const noexcept;
    void setControls(const ChorusControls& controls) noexcept;
    const StereoBlock& process(const StereoBlock& input,AudioWorkspace* =nullptr,double tunedDelay=-1,double harmonicity=0) noexcept;
    const native::Frame& state() const noexcept { return frame; }
    const MachineScratch& sharedScratch() const noexcept { return scratch; }

private:
    native::Frame frame{};
    MachineScratch scratch{};
};
}
