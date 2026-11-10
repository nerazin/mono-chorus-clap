#include <clap/clap.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__)
#include <xmmintrin.h>
#endif

/* A small, independent floating-point reconstruction of the signal flow
 * described in the accompanying reverse-engineering notes. No Elektron ROM
 * or emulator code is included or loaded. */

#define NPARAM 8
#define PI_D 3.14159265358979323846264338327950288
#define Q23 8388608.0

enum { DEL, DEP, SPD, MIX, FB, WID, LP, INP };
static const char *const names[NPARAM] = {"DEL", "DEP", "SPD", "MIX", "FB", "WID", "LP", "INP"};
static const double defaults[NPARAM] = {40, 28, 36, 64, 24, 96, 104, 64};

static const char *const features[] = {
   CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_CHORUS,
   CLAP_PLUGIN_FEATURE_STEREO, NULL
};
static const clap_plugin_descriptor_t descriptor = {
   .clap_version = CLAP_VERSION_INIT,
   .id = "works.reconstruction.monochorus",
   .name = "Mono Chorus",
   .vendor = "Independent DSP",
   .url = "", .manual_url = "", .support_url = "",
   .version = "0.3.0",
   .description = "Stereo six-tap chorus and metallic resonator",
   .features = features
};

typedef struct {
   clap_plugin_t plugin;
   const clap_host_t *host;
   _Atomic uint64_t base_bits[NPARAM];
   double modulation[NPARAM];
   double *buffer[2];
   size_t size, mask, write;
   double sample_rate, sample_scale, block_smooth;
   double sm_dep, sm_wid;
   double del_offset, del_start, del_step;
   double wid_sin, wid_cos, wid_sin_start, wid_cos_start;
   double depth_start, depth_end;
   double osc_a, osc_b, osc_prev_a;
   double pending[2][16];
   unsigned warmup_blocks;
   unsigned vector_pos;
   double lp_state[2], lp_table[128];
} Chorus;

static uint64_t double_bits(double x) {
   uint64_t u;
   memcpy(&u, &x, sizeof u);
   return u;
}
static double bits_double(uint64_t u) {
   double x;
   memcpy(&x, &u, sizeof x);
   return x;
}
static double base_value(const Chorus *s, unsigned id) {
   return bits_double(atomic_load_explicit(&s->base_bits[id], memory_order_relaxed));
}
static void set_base(Chorus *s, unsigned id, double x) {
   atomic_store_explicit(&s->base_bits[id], double_bits(x), memory_order_relaxed);
}
static double clamp(double x, double lo, double hi) {
   return x < lo ? lo : x > hi ? hi : x;
}
static double effective(const Chorus *s, unsigned id) {
   return clamp(base_value(s, id) + s->modulation[id], 0.0, 127.0);
}

/* Original 7-bit feedback low-pass coefficient curve. */
static void build_lp_table(Chorus *s) {
   static const unsigned q[128] = {
      0x004876, 0x004ce3, 0x005196, 0x005692, 0x005bdc, 0x006179, 0x00676d, 0x006dbe,
      0x007472, 0x007b8e, 0x008319, 0x008b1a, 0x009398, 0x009c9a, 0x00a629, 0x00b04c,
      0x00bb0e, 0x00c676, 0x00d291, 0x00df67, 0x00ed06, 0x00fb78, 0x010aca, 0x011b0a,
      0x012c47, 0x013e8f, 0x0151f2, 0x016682, 0x017c51, 0x019370, 0x01abf5, 0x01c5f5,
      0x01e187, 0x01fec1, 0x021dbe, 0x023e98, 0x02616b, 0x028654, 0x02ad74, 0x02d6eb,
      0x0302dd, 0x03316e, 0x0362c5, 0x03970b, 0x03ce6b, 0x040914, 0x044735, 0x048900,
      0x04ceac, 0x05186e, 0x056684, 0x05b928, 0x06109e, 0x066d27, 0x06cf0c, 0x073696,
      0x07a413, 0x0817d4, 0x08922e, 0x091378, 0x099c0e, 0x0a2c4e, 0x0ac49d, 0x0b655e,
      0x0c0efd, 0x0cc1e5, 0x0d7e86, 0x0e4553, 0x0f16c0, 0x0ff345, 0x10db5b, 0x11cf7c,
      0x12d023, 0x13ddcc, 0x14f8f2, 0x16220c, 0x175993, 0x189ff7, 0x19f5a8, 0x1b5b0b,
      0x1cd081, 0x1e565e, 0x1fecec, 0x219466, 0x234cf9, 0x2516bd, 0x26f1b5, 0x28ddcf,
      0x2adadc, 0x2ce891, 0x2f0683, 0x313423, 0x3370bd, 0x35bb79, 0x381350, 0x3a7714,
      0x3ce56a, 0x3f5ccb, 0x41db85, 0x445fba, 0x46e76a, 0x49706c, 0x4bf87c, 0x4e7d40,
      0x50fc49, 0x537323, 0x55df5d, 0x583e8d, 0x5a8e63, 0x5cccab, 0x5ef75c, 0x610c9c,
      0x630acc, 0x64f089, 0x66bcb0, 0x686e62, 0x6a04fd, 0x6b801f, 0x6cdf98, 0x6e236b,
      0x6f4bba, 0x7058c1, 0x714ac1, 0x7221f3, 0x72de71, 0x73801d, 0x74067d, 0x747093,
   };
   for (int v = 0; v < 128; ++v) {
      double omega = -log1p(-(double)q[v]/Q23);
      s->lp_table[v] = -expm1(-omega * 44100.0/s->sample_rate);
   }
}

static void clear_dsp(Chorus *s) {
   if (s->buffer[0]) memset(s->buffer[0], 0, s->size*sizeof(double));
   if (s->buffer[1]) memset(s->buffer[1], 0, s->size*sizeof(double));
   s->write = 0;
   s->lp_state[0] = s->lp_state[1] = 0;
   s->sm_dep = 0.0;
   s->sm_wid = 0.0;
   s->del_offset = floor(-992.0*effective(s, DEL)/128.0);
   s->del_start = s->del_offset;
   s->del_step = 0;
   s->vector_pos = 0;
   s->wid_sin = 0.0;
   s->wid_cos = 0.0;
   s->wid_sin_start = 0.0;
   s->wid_cos_start = 0.0;
   s->depth_start = s->depth_end = 0.0;
   s->osc_a = -1.0;
   s->osc_b = 0.0;
   s->warmup_blocks = 0;
   memset(s->pending, 0, sizeof s->pending);
}

static bool plug_init(const clap_plugin_t *p) {
   (void)p;
   return true;
}
static void ready_dsp(Chorus *s);
static void plug_destroy(const clap_plugin_t *p) {
   Chorus *s = p->plugin_data;
   free(s->buffer[0]);
   free(s->buffer[1]);
   free(s);
}
static bool plug_activate(const clap_plugin_t *p, double rate, uint32_t min, uint32_t max) {
   (void)min;
   (void)max;
   Chorus *s = p->plugin_data;
   if (!isfinite(rate) || rate < 1000 || rate > 768000) return false;
   size_t count = (size_t)ceil(2048.0 * rate / 44100.0);
   double *l = calloc(count, sizeof(double)), *r = calloc(count, sizeof(double));
   if (!l || !r) {
      free(l);
      free(r);
      return false;
   }
   free(s->buffer[0]);
   free(s->buffer[1]);
   s->buffer[0] = l;
   s->buffer[1] = r;
   s->size = count;
   s->mask = (count && !(count&(count-1))) ? count-1 : 0;
   s->sample_rate = rate;
   s->sample_scale = rate/44100.0;
   s->block_smooth = -expm1(log1p(-167772.0/Q23)*44100.0/rate);
   build_lp_table(s);
   ready_dsp(s);
   return true;
}
static void plug_deactivate(const clap_plugin_t *p) {
   Chorus *s = p->plugin_data;
   free(s->buffer[0]);
   free(s->buffer[1]);
   s->buffer[0] = s->buffer[1] = NULL;
   s->size = 0;
}
static bool plug_start(const clap_plugin_t *p) {
   (void)p;
   return true;
}
static void plug_stop(const clap_plugin_t *p) {
   (void)p;
}
static void plug_reset(const clap_plugin_t *p) {
   ready_dsp(p->plugin_data);
}
static void plug_on_main(const clap_plugin_t *p) {
   (void)p;
}

static void event(Chorus *s, const clap_event_header_t *h) {
   if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
   if (h->type == CLAP_EVENT_PARAM_VALUE && h->size >= sizeof(clap_event_param_value_t)) {
      const clap_event_param_value_t *e = (const void *)h;
      if (e->param_id < NPARAM && e->note_id == -1 && e->port_index == -1 && 
          e->channel == -1 && e->key == -1 && isfinite(e->value))
         set_base(s, e->param_id, clamp(e->value, 0, 127));
   } else if (h->type == CLAP_EVENT_PARAM_MOD && h->size >= sizeof(clap_event_param_mod_t)) {
      const clap_event_param_mod_t *e = (const void *)h;
      if (e->param_id < NPARAM && e->note_id == -1 && e->port_index == -1 && 
          e->channel == -1 && e->key == -1 && isfinite(e->amount))
         s->modulation[e->param_id] = clamp(e->amount, -127, 127);
   }
}

static size_t wrap_index(const Chorus *s, int64_t pos) {
   if (s->mask)
      return (size_t)pos & s->mask;
   int64_t r = pos % (int64_t)s->size;
   return (size_t)(r < 0 ? r + (int64_t)s->size : r);
}

static double read_delay(const Chorus *s, unsigned ch, double delay) {
   /* At unusually low host rates, the scaled minimum delay can be shorter
    * than one sample. Avoid reading the not-yet-written current sample. */
   if (delay < 1.0)
      delay = 1.0;
   double position = (double)s->write - delay;
   double below = floor(position), frac = position - below;
   int64_t pos = (int64_t)below;
   size_t a = wrap_index(s, pos), b = a + 1 == s->size ? 0 : a + 1;
   return s->buffer[ch][a] + frac * (s->buffer[ch][b] - s->buffer[ch][a]);
}

static double q23_floor(double x) {
   return floor(clamp(x, -1.0, 1.0-1.0/Q23)*Q23)/Q23;
}

/* Signal flow, separately for left and right:
 *   tap[k] = delay_line(centre - depth * phase[k])   k = 0, 1, 2
 *   wet = (tap[0] + tap[1] + tap[2]) / 3
 *   lowpass += alpha * (wet - lowpass)
 *   write = ((1 + feedback)/2) * input - feedback * previous_lowpass
 *   output = (1 - mix) * input + mix * wet
 * The 120-degree tap phases come from a quadrature oscillator.  Stereo width
 * rotates the right-channel oscillator.  Delay, depth, and width update every
 * 16 frames with ramps within each block.  The small quantisation and clipped
 * phase terms preserve audible behaviour of the original fixed-point design.
 * Feedback reads the previous low-pass value, and writes are committed after
 * all 16 reads so short delays see the same circular-buffer history. */
static void process_frame(Chorus *s, const double input[2], double output[2]) {
   if (s->vector_pos == 0) {
      s->del_start = s->del_offset;
      s->osc_prev_a = q23_floor(s->osc_a);
      double target = floor(-16.0 - 992.0 * effective(s, DEL) / 128.0);
      s->del_offset += (target - floor(s->del_offset)) * s->block_smooth;
      s->del_step = (s->del_offset - s->del_start) / 16.0;
      s->sm_dep += (effective(s, DEP) - s->sm_dep) * s->block_smooth;
      s->depth_start = s->depth_end;

      /* The modulation depth is stored at 1/256 sample resolution. */
      s->depth_end = floor(floor((s->del_offset + 16.0) * 256.0) *
                           (-s->sm_dep / 128.0)) / 256.0;
      s->sm_wid += (effective(s, WID) - s->sm_wid) * s->block_smooth;
      s->sm_wid = floor(s->sm_wid * (Q23 / 128.0)) * (128.0 / Q23);
      const double angle = floor(s->sm_wid * 16.0) * (PI_D / 4096.0);
      s->wid_sin_start = s->wid_sin;
      s->wid_cos_start = s->wid_cos;
      s->wid_sin = sin(angle);
      s->wid_cos = cos(angle);
   }

   const double center = -(s->del_start + s->del_step * (s->vector_pos + 1)) * s->sample_scale;
   const double depth = (s->depth_start + (s->depth_end - s->depth_start) *
                         (s->vector_pos / 16.0)) * s->sample_scale;
   const double speed = effective(s, SPD);
   const double step = floor(2390.0 * speed * speed / 16384.0) / Q23 * 44100.0 / s->sample_rate;
   const double m = effective(s, MIX) / 128.0;
   const double q = effective(s, FB) / 128.0 + 64881.0 / Q23;
   const double h = (1.0 + q) * 0.5;
   const double gain = 4.0 * pow(effective(s, INP) / 128.0, 2.0);
   const double p = effective(s, LP);
   int lpi = (int)p;
   const double alpha = lpi >= 127 ? s->lp_table[127] :
      s->lp_table[lpi] + (p - lpi) * (s->lp_table[lpi + 1] - s->lp_table[lpi]);

   const double A = q23_floor(s->osc_a);
   const double B = q23_floor(s->osc_b);
   const double t = (double)s->vector_pos / 16.0;
   const double cw = s->wid_cos_start + t * (s->wid_cos - s->wid_cos_start);
   const double sw = s->wid_sin_start + t * (s->wid_sin - s->wid_sin_start);
   const double aa[2] = {A, q23_floor(cw * A - sw * B)};
   const double bb[2] = {B, q23_floor(cw * B + sw * A)};

   for (unsigned c = 0; c < 2; ++c) {
      const double phase[3] = {
         bb[c],
         q23_floor(-0.5 * bb[c] + 0.866025447845459 * aa[c]),
         q23_floor(-0.5 * bb[c] - 0.866025447845459 * aa[c])
      };
      double wet = 0;
      for (int i = 0; i < 3; ++i)
         wet += read_delay(s, c, center - depth * phase[i]);
      wet = (s->warmup_blocks < 128) ? 0.0 : wet / 3.0;

      double x = (isfinite(input[c]) && fabs(input[c]) >= 1e-25)
         ? clamp(input[c] * gain, -1.0, 1.0 - 1.0 / Q23) : 0.0;
      const double old = s->lp_state[c];
      double next = old + alpha * (wet - old);
      s->lp_state[c] = fabs(next) < 1e-25 ? 0.0 : next;

      /* Approximate the original 24-bit bounded delay memory. */
      s->pending[c][s->vector_pos] = clamp(h * x - q * old, -1.0, 1.0);
      output[c] = clamp((1.0 - m) * x + m * wet, -1.0, 1.0 - 1.0 / Q23);
   }

   s->osc_b -= s->osc_prev_a * step;
   s->osc_prev_a = A;
   s->osc_a += B * step;
   if (s->vector_pos == 15) {
      for (unsigned c = 0; c < 2; c++)
         for (unsigned i = 0; i < 16; i++)
            s->buffer[c][wrap_index(s, (int64_t)s->write - 15 + i)] = s->pending[c][i];
      s->osc_a = q23_floor(s->osc_a);
      s->osc_b = q23_floor(s->osc_b);
      if (s->warmup_blocks < 128)
         s->warmup_blocks++;
   }
   if (++s->write == s->size)
      s->write = 0;
   s->vector_pos = (s->vector_pos + 1) & 15u;
}

/* Initialise the delay and modulation before the host sends its first note.
 * This represents an already-running effect and avoids losing the first hit to
 * the original processor's short initial wet-output gate. */
static void ready_dsp(Chorus *s) {
   clear_dsp(s);
   if (!s->size)
      return;
   const double silence[2] = {0.0, 0.0};
   double out[2];
   for (unsigned i = 0; i < 2048; i++)
      process_frame(s, silence, out);
}

static clap_process_status plug_process(const clap_plugin_t *p, const clap_process_t *pr) {
   Chorus *s = p->plugin_data;
   if (!s->size || !pr || pr->audio_inputs_count < 1 || pr->audio_outputs_count < 1 || 
       !pr->audio_inputs || !pr->audio_outputs) return CLAP_PROCESS_ERROR;
   const clap_audio_buffer_t *in = &pr->audio_inputs[0];
   clap_audio_buffer_t *out = &pr->audio_outputs[0];
   if (in->channel_count < 2 || out->channel_count < 2 || 
       ((!in->data32 || !out->data32) && (!in->data64 || !out->data64))) return CLAP_PROCESS_ERROR;
   const clap_input_events_t *ev = pr->in_events;
   uint32_t n = ev && ev->size ? ev->size(ev) : 0, ix = 0;
#if defined(__SSE2__)
   const unsigned saved_fp_mode = _mm_getcsr();
   _mm_setcsr(saved_fp_mode | 0x8040u);
   /* flush-to-zero and denormals-are-zero */
#elif defined(__aarch64__)
   uint64_t saved_fp_mode;
   __asm__ volatile("mrs %0, fpcr" : "=r"(saved_fp_mode));
   uint64_t rt_fp_mode = saved_fp_mode | (1ull<<24);
   __asm__ volatile("msr fpcr, %0" : : "r"(rt_fp_mode));
#endif
   for (uint32_t i = 0; i < pr->frames_count; ++i) {
      while (ix < n) {
         const clap_event_header_t *h = ev->get(ev, ix);
         if (h && h->time > i) break;
         event(s, h);
         ++ix;
      }
      double x[2], y[2];
      if (in->data32) {
         x[0] = in->data32[0][i];
         x[1] = in->data32[1][i];
      }
      else {
         x[0] = in->data64[0][i];
         x[1] = in->data64[1][i];
      }
      process_frame(s, x, y);
      if (out->data32) {
         out->data32[0][i] = (float)y[0];
         out->data32[1][i] = (float)y[1];
      }
      else {
         out->data64[0][i] = y[0];
         out->data64[1][i] = y[1];
      }
   }
#if defined(__SSE2__)
   _mm_setcsr(saved_fp_mode);
#elif defined(__aarch64__)
   __asm__ volatile("msr fpcr, %0" : : "r"(saved_fp_mode));
#endif
   out->constant_mask = 0;
   return CLAP_PROCESS_CONTINUE;
}

static uint32_t ports_count(const clap_plugin_t *p, bool input) {
   (void)p;
   (void)input;
   return 1;
}
static bool ports_get(const clap_plugin_t *p, uint32_t i, bool input, clap_audio_port_info_t *info) {
   (void)p;
   if (i || !info) return false;
   memset(info, 0, sizeof *info);
   info->id = 0;
   snprintf(info->name, sizeof info->name, "%s", input ? "Stereo In":"Stereo Out");
   info->flags = CLAP_AUDIO_PORT_IS_MAIN|CLAP_AUDIO_PORT_SUPPORTS_64BITS|CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE;
   info->channel_count = 2;
   info->port_type = CLAP_PORT_STEREO;
   info->in_place_pair = 0;
   return true;
}
static const clap_plugin_audio_ports_t ports = {ports_count, ports_get};

static uint32_t params_count(const clap_plugin_t *p) {
   (void)p;
   return NPARAM;
}
static bool params_info(const clap_plugin_t *p, uint32_t i, clap_param_info_t *info) {
   (void)p;
   if (i >= NPARAM || !info) return false;
   memset(info, 0, sizeof *info);
   info->id = i;
   info->flags = CLAP_PARAM_IS_AUTOMATABLE|CLAP_PARAM_IS_MODULATABLE|CLAP_PARAM_REQUIRES_PROCESS;
   snprintf(info->name, sizeof info->name, "%s", names[i]);
   info->min_value = 0;
   info->max_value = 127;
   info->default_value = defaults[i];
   return true;
}
static bool params_get(const clap_plugin_t *p, clap_id id, double *v) {
   if (id >= NPARAM || !v) return false;
   *v=base_value(p->plugin_data,id); return true;
}
static bool params_to_text(const clap_plugin_t *p, clap_id id, double v, char *out, uint32_t cap) {
   (void)p;
   if (id >= NPARAM || !out || !cap || !isfinite(v)) return false;
   snprintf(out, cap, "%.1f", v);
   return true;
}
static bool params_from_text(const clap_plugin_t *p, clap_id id, const char *text, double *v) {
   (void)p;
   if (id >= NPARAM || !text || !v) return false;
   char *end = NULL;
   double x = strtod(text, &end);
   if (end == text || !isfinite(x)) return false;
   *v=clamp(x,0,127); return true;
}
static void params_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) {
   (void)out;
   if (!in || !in->size || !in->get) return;
   uint32_t n = in->size(in);
   for (uint32_t i = 0; i < n; ++i) event(p->plugin_data, in->get(in, i));
}
static const clap_plugin_params_t params = {params_count, params_info, params_get, params_to_text, params_from_text, params_flush};

static bool stream_write(const clap_ostream_t *s, const void *buf, uint64_t n) {
   const uint8_t *p = buf;
   while (n) {
      int64_t done = s->write(s, p, n);
      if (done <= 0 || (uint64_t)done > n) return false;
      p += done;
      n -= (uint64_t)done;
   }
   return true;
}
static bool stream_read(const clap_istream_t *s, void *buf, uint64_t n) {
   uint8_t *p = buf;
   while (n) {
      int64_t done = s->read(s, p, n);
      if (done <= 0 || (uint64_t)done > n) return false;
      p += done;
      n -= (uint64_t)done;
   }
   return true;
}
static bool state_save(const clap_plugin_t *p, const clap_ostream_t *out) {
   if (!out || !out->write) return false;
   Chorus *s = p->plugin_data;
   const char magic[8] = {'M', 'C', 'H', 'O', 'R', 0, 0, 1};
   if (!stream_write(out, magic, sizeof magic)) return false;
   for (unsigned i = 0; i < NPARAM; ++i) {
      uint64_t u = double_bits(base_value(s, i));
      uint8_t bytes[8];
      for (unsigned j = 0; j < 8; ++j) bytes[j] = (uint8_t)(u>>(8*j));
      if (!stream_write(out, bytes, 8)) return false;
   }
   return true;
}
static bool state_load(const clap_plugin_t *p, const clap_istream_t *in) {
   if (!in || !in->read) return false;
   Chorus *s = p->plugin_data;
   char magic[8];
   double v[NPARAM];
   const char expected[8] = {'M', 'C', 'H', 'O', 'R', 0, 0, 1};
   if (!stream_read(in, magic, 8) || memcmp(magic, expected, 8)) return false;
   for (unsigned i = 0; i < NPARAM; ++i) {
      uint8_t bytes[8];
      uint64_t u = 0;
      if (!stream_read(in, bytes, 8)) return false;
      for (unsigned j = 0; j < 8; ++j) u|=(uint64_t)bytes[j]<<(8*j);
      v[i] = bits_double(u);
      if (!isfinite(v[i]) || v[i] < 0 || v[i] > 127) return false;
   }
   for (unsigned i = 0; i < NPARAM; ++i) set_base(s, i, v[i]);
   if (s->host && s->host->get_extension) {
      const clap_host_params_t *hp = s->host->get_extension(s->host, CLAP_EXT_PARAMS);
      if (hp && hp->rescan) hp->rescan(s->host, CLAP_PARAM_RESCAN_VALUES);
   }
   return true;
}
static const clap_plugin_state_t state = {state_save, state_load};

static uint32_t remote_count(const clap_plugin_t *p) {
   (void)p;
   return 1;
}
static bool remote_get(const clap_plugin_t *p, uint32_t i, clap_remote_controls_page_t *page) {
   (void)p;
   if (i || !page) return false;
   memset(page, 0, sizeof *page);
   page->page_id = 0;
   snprintf(page->section_name, sizeof page->section_name, "Chorus");
   snprintf(page->page_name, sizeof page->page_name, "Main");
   for (unsigned k = 0; k < NPARAM; ++k) page->param_ids[k] = k;
   return true;
}
static const clap_plugin_remote_controls_t remote = {remote_count, remote_get};
static uint32_t tail_get(const clap_plugin_t *p) {
   (void)p;
   return UINT32_MAX;
}
static const clap_plugin_tail_t tail = {tail_get};

static const void *plug_extension(const clap_plugin_t *p, const char *id) {
   (void)p;
   if (!id) return NULL;
   if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ports;
   if (!strcmp(id, CLAP_EXT_PARAMS)) return &params;
   if (!strcmp(id, CLAP_EXT_STATE)) return &state;
   if (!strcmp(id, CLAP_EXT_REMOTE_CONTROLS) || !strcmp(id, CLAP_EXT_REMOTE_CONTROLS_COMPAT)) return &remote;
   if (!strcmp(id, CLAP_EXT_TAIL)) return &tail;
   return NULL;
}
static uint32_t factory_count(const clap_plugin_factory_t *f) {
   (void)f;
   return 1;
}
static const clap_plugin_descriptor_t *factory_desc(const clap_plugin_factory_t *f, uint32_t i) {
   (void)f;
   return i ? NULL : &descriptor;
}
static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
   (void)f;
   if (!host || !id || strcmp(id, descriptor.id) || !clap_version_is_compatible(host->clap_version)) return NULL;
   Chorus *s = calloc(1, sizeof *s);
   if (!s) return NULL;
   s->host = host;
   for (unsigned i = 0; i < NPARAM; ++i) atomic_init(&s->base_bits[i], double_bits(defaults[i]));
   s->plugin = (clap_plugin_t) {
      .desc = &descriptor, .plugin_data = s, .init = plug_init, .destroy = plug_destroy,
      .activate = plug_activate, .deactivate = plug_deactivate, .start_processing = plug_start,
      .stop_processing = plug_stop, .reset = plug_reset, .process = plug_process,
      .get_extension = plug_extension, .on_main_thread = plug_on_main
   };
   return &s->plugin;
}
static const clap_plugin_factory_t factory = {factory_count, factory_desc, factory_create};
static bool entry_init(const char *path) {
   (void)path;
   return true;
}
static void entry_deinit(void) {
}
static const void *entry_factory(const char *id) {
   return id && !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &factory : NULL;
}
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_factory};
