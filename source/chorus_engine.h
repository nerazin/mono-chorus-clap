#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Created and destroyed outside the audio callback. No firmware is required. */
void *chorus_engine_create(double host_sample_rate);
void chorus_engine_destroy(void *engine);
void chorus_engine_reset(void *engine);
uint32_t chorus_engine_latency(const void *engine);

/* One host frame. Controls have the v0.3.0 range 0..127. */
void chorus_engine_process(void *engine, const double input[2],
                           const double controls[8], double output[2]);

#ifdef __cplusplus
}
#endif
