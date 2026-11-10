#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cmath>

namespace monofx::native
{
using Word = uint32_t;
using Acc = int64_t;
inline int32_t signed24(Word value) { return static_cast<int32_t>((value & 0xffffff) ^ 0x800000) - 0x800000; }
inline Word word(Acc value) { return static_cast<Word>(value) & 0xffffff; }
inline Acc fraction(Word value) { return Acc(signed24(value)) * 0x1000000LL; }
inline Acc multiply(Word a,Word b) { return Acc(signed24(a)) * signed24(b) * 2; }
inline Word limited(Acc value)
{
    return word(std::clamp<Acc>(value >> 24,-0x800000,0x7fffff));
}
inline Acc longValue(Word high,Word low)
{
    return fraction(high) + (low & 0xffffff);
}

struct Tables
{
    std::array<Word,128> lowpass{};
    std::array<Word,4096> sine{};
};

struct Frame
{
    std::array<Word,48> x{},y{};
    std::array<Word,32> input{},output{};
    std::array<Word,4096> delay{};
};

// One native 16-frame stereo invocation. Parameters and persistent state use
// the firmware's 24-bit representation. No emulator types or opcode execution.
inline void process(Frame& state,const Tables& tables,std::array<Word,6>* sharedScratch=nullptr,
                    std::array<Word,256>* workspaceX=nullptr,std::array<Word,256>* workspaceY=nullptr,double tunedDelay=-1,double harmonicity=0)
{
    auto& x=state.x; auto& y=state.y;
    const Word gate = y[0x21] < 128 ? 0 : 0x7fffff;
    if(y[0x21]<128) ++y[0x21];
    y[0x20]=gate;

    std::array<Word,32> dry{},wet{},feedbackSample{};
    const auto inputGain=limited(multiply(y[0xb],y[0xb]));
    for(unsigned i=0;i<32;++i) dry[i]=limited(multiply(state.input[i],inputGain)*4);
    if(workspaceY)std::copy(dry.begin(),dry.end(),workspaceY->begin());

    const auto speed=limited(multiply(limited(multiply(y[6],y[6])),2390));
    std::array<std::array<Word,16>,6> oscillator{};
    std::array<Word,16> quadrature{};
    Acc a=fraction(x[0x11]),b=fraction(x[0x12]);
    Word previousA=limited(a);
    for(unsigned i=0;i<16;++i)
    {
        const auto previousB=limited(b);
        oscillator[0][i]=previousB;
        // P:1476a5 enables arithmetic saturation for this recurrence. It
        // clips the full 48-bit accumulator after each MAC, not just the
        // later 24-bit memory transfer. The distinction appears at peaks.
        b=std::clamp<Acc>(b-multiply(previousA,speed),-0x800000000000LL,0x7fffffffffffLL);
        previousA=limited(a);
        quadrature[i]=previousA;
        a=std::clamp<Acc>(a+multiply(previousB,speed),-0x800000000000LL,0x7fffffffffffLL);
    }
    x[0x11]=limited(a);x[0x12]=limited(b);

    a=fraction(y[0x24]);
    a+=multiply(0x028f5c,y[9]);
    a+=multiply(0xfd70a4,y[0x24]);
    y[0x24]=limited(a);
    const auto tableIndex=limited(multiply(2048,limited(a)));
    a=fraction(tables.sine[std::min<Word>(tableIndex,2047)]);
    b=fraction(tables.sine[std::min<Word>(tableIndex+2048,4095)]);
    Word previousSin=y[0x26],previousCos=y[0x25];
    y[0x26]=limited(a);y[0x25]=limited(b);
    const auto deltaSin=limited(a-fraction(previousSin));
    const auto deltaCos=limited(b-fraction(previousCos));
    a=fraction(previousSin);b=fraction(previousCos);
    std::array<Word,16> rightQuadrature{};
    for(unsigned i=0;i<16;++i)
    {
        const auto sin=limited(a),cos=limited(b);
        if(workspaceY)(*workspaceY)[0x80+i]=sin;
        a+=multiply(deltaSin,0x080000);b+=multiply(deltaCos,0x080000);
        oscillator[3][i]=limited(multiply(cos,oscillator[0][i])+multiply(sin,quadrature[i]));
        rightQuadrature[i]=limited(multiply(quadrature[i],cos)-multiply(oscillator[0][i],sin));
    }
    for(unsigned i=0;i<16;++i)
    {
        oscillator[1][i]=limited(multiply(0xc00000,oscillator[0][i])+multiply(0x6ed9ec,quadrature[i]));
        oscillator[2][i]=limited(multiply(0xc00000,oscillator[0][i])+multiply(0x912614,quadrature[i]));
        oscillator[4][i]=limited(multiply(0xc00000,oscillator[3][i])+multiply(0x6ed9ec,rightQuadrature[i]));
        oscillator[5][i]=limited(multiply(0xc00000,oscillator[3][i])+multiply(0x912614,rightQuadrature[i]));
    }

    const Acc previousDelay=longValue(x[0x1d],y[0x1d]);
    if(workspaceX)(*workspaceX)[0xff]=x[0x1d];
    if(workspaceY)(*workspaceY)[0xff]=y[0x1d];
    const auto targetDelay=limited(multiply(0xfffc20,y[4])+fraction(0xfffff0));
    b=previousDelay+multiply(0x028f5c,targetDelay)+multiply(0xfd70a4,limited(previousDelay));
    // Optional musical tuning extends only the delay address precision. The
    // ordinary firmware control path above remains bit-exact when disabled.
    if(tunedDelay>=0)b=-Acc(tunedDelay*16777216.);
    x[0x1d]=word(b>>24);y[0x1d]=word(b);
    a=longValue(x[0x22],y[0x22]);
    a+=multiply(0xfd70a4,y[5])+multiply(0xfd70a4,limited(a));
    x[0x22]=word(a>>24);y[0x22]=word(a);
    const auto depth=limited(multiply(limited(a),limited((b-fraction(0xfffff0))*256)));
    const Word oldDepth=y[0x23];y[0x23]=depth;
    const auto depthDelta=limited(fraction(depth)-fraction(oldDepth));
    a=fraction(oldDepth);
    std::array<Word,16> depths{};
    for(unsigned i=0;i<16;++i) { depths[i]=limited(a);a+=multiply(depthDelta,0x080000); }
    const Acc delayStep=(b-previousDelay+fraction(16))>>4;
    std::array<std::array<Word,16>,6> taps{};
    const auto position=y[0x10]&2047;
    for(unsigned tap=0;tap<6;++tap)
    {
        Acc base=previousDelay;
        for(unsigned i=0;i<16;++i)
        {
            const Acc address=(multiply(oscillator[tap][i],depths[i])>>8)+base;
            const auto offset=word(address>>24);
            const auto fractionBits=word(address);
            if(workspaceX)(*workspaceX)[0x20+tap*16+i]=offset;
            if(workspaceY)(*workspaceY)[0x20+tap*16+i]=fractionBits;
            const auto ring=(tap>=3 ? 2048 : 0);
            const auto at=(position+offset)&2047;
            const Word first=state.delay[ring+at],second=state.delay[ring+((at+1)&2047)];
            const Acc interpolated=(Acc(signed24(second))-signed24(first))*fractionBits;
            taps[tap][i]=limited(fraction(first)+interpolated);
            if(workspaceX)(*workspaceX)[0x80+tap*16+i]=taps[tap][i];
            base+=delayStep;
        }
    }
    const auto tapGain=limited(multiply(0x2aaaab,gate));
    if(workspaceY){(*workspaceY)[0x3e]=tapGain;(*workspaceY)[0x3f]=limited(delayStep);}
    for(unsigned i=0;i<16;++i) for(unsigned c=0;c<2;++c)
        wet[i*2+c]=limited(multiply(tapGain,taps[c*3][i])+multiply(tapGain,taps[c*3+1][i])+multiply(tapGain,taps[c*3+2][i]));
    if(workspaceY)std::copy(wet.begin(),wet.end(),workspaceY->begin()+0x40);

    auto loopWet=wet;
    if(harmonicity>0) {
        const double h=std::clamp(harmonicity,0.,1.);
        for(unsigned c=0;c<2;++c){ Acc base=previousDelay;
            for(unsigned i=0;i<16;++i){
                auto at=(position+word(base>>24))&2047;
                const auto first=state.delay[c*2048+at],second=state.delay[c*2048+((at+1)&2047)];
                const auto tap=limited(fraction(first)+(Acc(signed24(second))-signed24(first))*word(base));
                const auto stationary=limited(multiply(tapGain,tap)*3);
                loopWet[2*i+c]=word(int64_t(std::llround(signed24(wet[2*i+c])*(1-h)+signed24(stationary)*h)));
                base+=delayStep;
            }
        }
    }
    const auto alpha=tables.lowpass[std::min<Word>(127,y[0xa]>>16)];
    a=fraction(y[0x1e]);b=fraction(y[0x1f]);
    for(unsigned i=0;i<16;++i)
    {
        const auto oldA=limited(a),oldB=limited(b);
        feedbackSample[2*i]=oldA;feedbackSample[2*i+1]=oldB;
        a+=multiply(alpha,loopWet[2*i])-multiply(alpha,oldA);
        b+=multiply(alpha,loopWet[2*i+1])-multiply(alpha,oldB);
    }
    y[0x1e]=limited(a);y[0x1f]=limited(b);
    // P:147798/14779a leave feedback samples in Y:60..7f. The shared
    // amplification loop later consumes Y:6d..6f as fractional low words.
    // Preserve the historical Frame ABI; this is an explicit stage output.
    if(sharedScratch) for(unsigned i=0;i<3;++i) (*sharedScratch)[i]=feedbackSample[13+i];
    if(workspaceY)std::copy(feedbackSample.begin(),feedbackSample.end(),workspaceY->begin()+0x60);
    const Acc feedback=-fraction(y[8])-fraction(0xfd71);
    const auto feedbackGain=limited(feedback);
    const auto injectionGain=limited((fraction(0x7fffff)-feedback)>>1);
    for(unsigned i=0;i<16;++i) for(unsigned c=0;c<2;++c)
        state.delay[c*2048+((position+i)&2047)]=limited(multiply(dry[i*2+c],injectionGain)+multiply(feedbackSample[i*2+c],feedbackGain));
    y[0x10]=(y[0x10]&~2047u)|((position+16)&2047);
    const auto wetGain=y[7],dryGain=word(0x7fffff-wetGain);
    for(unsigned i=0;i<32;++i) state.output[i]=limited(multiply(dryGain,dry[i])+multiply(wetGain,wet[i]));
}
}
