#include "chorus_engine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

int main() {
    const double rates[]={8000,22050,32000,44100,47999.5,48000,88200,96000,192000,384000};
    const double controls[8]={40,0,0,0,0,0,127,64};
    const double input[2]={0.125,-0.25};
    for(double rate:rates) {
        void *engine=chorus_engine_create(rate); if(!engine) return 1;
        const unsigned latency=chorus_engine_latency(engine);
        double max_error=0.0,out[2]{};
        const unsigned seconds=rate>192000 ? 3 : 10;
        const unsigned frames=unsigned(rate*seconds);
        const std::clock_t start=std::clock();
        for(unsigned i=0;i<frames;i++) {
            chorus_engine_process(engine,input,controls,out);
            if(i>latency+unsigned(rate*0.08)) {
                if(!std::isfinite(out[0]) || !std::isfinite(out[1])) return 2;
                max_error=std::max(max_error,std::abs(out[0]-input[0]));
                max_error=std::max(max_error,std::abs(out[1]-input[1]));
            }
        }
        const double seconds_cpu=double(std::clock()-start)/CLOCKS_PER_SEC;
        if(max_error>0.00001) {std::fprintf(stderr,"dropout at %g Hz, maximum error %g\n",rate,max_error);return 3;}
        std::printf("PASS %.1f Hz: %u frames, latency=%u, max error=%.3g, CPU=%.3f s\n",rate,frames,latency,max_error,seconds_cpu);
        chorus_engine_destroy(engine);
    }
}
