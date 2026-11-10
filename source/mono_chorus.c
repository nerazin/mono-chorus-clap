#include <clap/clap.h>
#include "chorus_engine.h"
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* CLAP wrapper based on 0.3.0. The chorus DSP uses the independently
 * supplied fixed-point native core, without loading firmware or an emulator. */

#define NPARAM 8

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
   .version = "0.4.0",
   .description = "Stereo six-tap chorus and metallic resonator",
   .features = features
};

typedef struct {
   clap_plugin_t plugin;
   const clap_host_t *host;
   _Atomic uint64_t base_bits[NPARAM];
   double modulation[NPARAM];
   void *engine;
   uint32_t latency;
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

static bool plug_init(const clap_plugin_t *p) {
   (void)p;
   return true;
}
static void plug_destroy(const clap_plugin_t *p) {
   Chorus *s = p->plugin_data;
   chorus_engine_destroy(s->engine);
   free(s);
}
static bool plug_activate(const clap_plugin_t *p, double rate, uint32_t min, uint32_t max) {
   (void)min;
   (void)max;
   Chorus *s = p->plugin_data;
   if (s->engine) return false;
   s->engine = chorus_engine_create(rate);
   if (!s->engine) return false;
   const uint32_t previous = s->latency;
   s->latency = chorus_engine_latency(s->engine);
   if (s->latency != previous && s->host && s->host->get_extension) {
      const clap_host_latency_t *host_latency = s->host->get_extension(s->host, CLAP_EXT_LATENCY);
      if (host_latency && host_latency->changed) host_latency->changed(s->host);
   }
   return true;
}
static void plug_deactivate(const clap_plugin_t *p) {
   Chorus *s = p->plugin_data;
   chorus_engine_destroy(s->engine);
   s->engine = NULL;
}
static bool plug_start(const clap_plugin_t *p) {
   (void)p;
   return true;
}
static void plug_stop(const clap_plugin_t *p) {
   (void)p;
}
static void plug_reset(const clap_plugin_t *p) {
   Chorus *s = p->plugin_data;
   chorus_engine_reset(s->engine);
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

static clap_process_status plug_process(const clap_plugin_t *p, const clap_process_t *pr) {
   Chorus *s = p->plugin_data;
   if (!s->engine || !pr || pr->audio_inputs_count < 1 || pr->audio_outputs_count < 1 ||
       !pr->audio_inputs || !pr->audio_outputs) return CLAP_PROCESS_ERROR;
   const clap_audio_buffer_t *in = &pr->audio_inputs[0];
   clap_audio_buffer_t *out = &pr->audio_outputs[0];
   if (in->channel_count < 2 || out->channel_count < 2 ||
       ((!in->data32 || !out->data32) && (!in->data64 || !out->data64))) return CLAP_PROCESS_ERROR;
   const clap_input_events_t *events = pr->in_events;
   const uint32_t count = events && events->size && events->get ? events->size(events) : 0;
   uint32_t next = 0;
   for (uint32_t frame = 0; frame < pr->frames_count; ++frame) {
      while (next < count) {
         const clap_event_header_t *header = events->get(events, next);
         if (header && header->time > frame) break;
         event(s, header);
         ++next;
      }
      double input[2], output[2], controls[NPARAM];
      if (in->data32) {
         input[0] = in->data32[0][frame];
         input[1] = in->data32[1][frame];
      } else {
         input[0] = in->data64[0][frame];
         input[1] = in->data64[1][frame];
      }
      for (unsigned i = 0; i < NPARAM; ++i) controls[i] = effective(s, i);
      chorus_engine_process(s->engine, input, controls, output);
      if (out->data32) {
         out->data32[0][frame] = (float)output[0];
         out->data32[1][frame] = (float)output[1];
      } else {
         out->data64[0][frame] = output[0];
         out->data64[1][frame] = output[1];
      }
   }
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
static uint32_t latency_get(const clap_plugin_t *p) {
   const Chorus *s = p->plugin_data;
   return s->latency;
}
static const clap_plugin_latency_t latency = {latency_get};

static const void *plug_extension(const clap_plugin_t *p, const char *id) {
   (void)p;
   if (!id) return NULL;
   if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ports;
   if (!strcmp(id, CLAP_EXT_PARAMS)) return &params;
   if (!strcmp(id, CLAP_EXT_STATE)) return &state;
   if (!strcmp(id, CLAP_EXT_REMOTE_CONTROLS) || !strcmp(id, CLAP_EXT_REMOTE_CONTROLS_COMPAT)) return &remote;
   if (!strcmp(id, CLAP_EXT_TAIL)) return &tail;
   if (!strcmp(id, CLAP_EXT_LATENCY)) return &latency;
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
