#include "Chorus.h"
#include "ChorusTables.generated.h"
#include "AudioWorkspace.h"

namespace monofx::core
{
const native::Tables& chorusTables() noexcept { return data::chorusTables; }

void Chorus::setControls(const ChorusControls& controls) noexcept
{
    for(unsigned i=0;i<8;++i) frame.y[4+i]=native::word(controls[i]);
}

void Chorus::reset(const ChorusControls& controls) noexcept
{
    frame={};
    scratch={};
    setControls(controls);
    // P:0x147661 after common track-memory clear. Ring addresses are replaced
    // by a zero-based index into this instance's two 2048-word channel rings.
    frame.x[0x11]=0x800000;
    frame.x[0x1d]=native::limited(native::multiply(0xfffc20,controls[0]));
    frame.y[0x10]=MachineMemory::ringBase;
    frame.y[0x1b]=0x14a000;
}

void Chorus::activate(const ChorusControls& controls,const MachineMemory& memory) noexcept
{
    std::copy_n(memory.x.begin(),frame.x.size(),frame.x.begin());
    std::copy_n(memory.y.begin(),frame.y.size(),frame.y.begin());
    std::copy_n(memory.ring.begin(),frame.delay.size(),frame.delay.begin());
    setControls(controls);
    frame.y[0x10]=MachineMemory::ringBase;
    frame.x[0x11]=0x800000;frame.x[0x12]=0;
    frame.x[0x1d]=native::limited(native::multiply(0xfffc20,controls[0]));
    frame.y[0x1b]=0x14a000;frame.y[0x21]=frame.y[0x20]=frame.y[0x1d]=0;
    frame.x[0x22]=frame.y[0x22]=0;
}
void Chorus::save(MachineMemory& memory) const noexcept
{
    std::copy(frame.x.begin(),frame.x.end(),memory.x.begin());
    std::copy(frame.y.begin(),frame.y.end(),memory.y.begin());
    std::copy(frame.delay.begin(),frame.delay.end(),memory.ring.begin());
}

const StereoBlock& Chorus::process(const StereoBlock& input,AudioWorkspace* workspace,double tunedDelay,double harmonicity) noexcept
{
    frame.input=input;
    if(workspace)for(unsigned i=0;i<3;++i)scratch[3+i]=workspace->y[0xad+i];
    native::process(frame,chorusTables(),&scratch,workspace ? &workspace->x : nullptr,workspace ? &workspace->y : nullptr,tunedDelay,harmonicity);
    return frame.output;
}
}
