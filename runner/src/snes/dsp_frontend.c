/* S-DSP front end (dsp_frontend.h). Mirrors dsp_cycleChannel, dsp_decodeBrr
 * and dsp_write in dsp.c for everything that decides positions. */
#include "dsp_frontend.h"

#include <string.h>

#include "dsp.h"

const int *dsp_rate_values(void);   /* dsp.c */

static inline bool even_at(const DspFrontEnd *fe, uint32_t s) {
  return ((s - fe->even_base) & 1u) == 0;
}

/* The next sample >= from at which voice v has work. */
static uint32_t voice_next(const DspFrontEnd *fe, const DspFeVoice *v, uint32_t from) {
  if (v->pmod) return UINT32_MAX;
  uint32_t next = UINT32_MAX;
  if (v->pitch) {
    /* Counter at sample `from` (no overflow before then: from <= v->next). */
    const uint32_t c = v->counter + (uint32_t)v->pitch * (from - v->t);
    /* Decode on the sample k where c + pitch * (k + 1) first exceeds $FFFF. */
    next = from + (0xffffu - c) / v->pitch;
  }
  if (v->key_on && !v->key_off) {
    const uint32_t e = even_at(fe, from) ? from : from + 1;
    if (e < next) next = e;
  }
  return next;
}

static void refresh_next(DspFrontEnd *fe) {
  uint32_t n = UINT32_MAX;
  for (int i = 0; i < 8; i++)
    if (fe->v[i].next < n) n = fe->v[i].next;
  fe->next = n;
}

/* Bring voice v's counter to the start of sample s (s <= its next event). */
static inline void voice_catch_up(DspFeVoice *v, uint32_t s) {
  if (!v->pmod) v->counter = (uint16_t)(v->counter + (uint32_t)v->pitch * (s - v->t));
  v->t = s;
}

/* ---- Envelope (dsp_cycleChannel / dsp_handleGain) ---------------------- */

static void env_gain(DspFeVoice *v, bool live_gain) {
  if (v->state == 4) {   /* release */
    v->gain -= 8;
    if (v->gain > 0x7ff) v->gain = 0;
    return;
  }
  if (live_gain) {
    switch (v->gain_mode) {
      case 0: v->gain -= 32; if (v->gain > 0x7ff) v->gain = 0; break;
      case 1: v->gain -= ((v->gain - 1) >> 8) + 1; break;
      case 2: v->gain += 32; if (v->gain > 0x7ff) v->gain = 0x7ff; break;
      case 3: v->gain += v->gain < 0x600 ? 32 : 8; if (v->gain > 0x7ff) v->gain = 0x7ff; break;
    }
  } else if (v->state == 0) {
    v->gain += v->rates[0] == 1 ? 1024 : 32;
    if (v->gain > 0x7ff) v->gain = 0x7ff;
  } else {   /* decay, sustain */
    v->gain -= ((v->gain - 1) >> 8) + 1;
  }
  if (v->state == 0 && v->gain >= 0x7e0) {
    v->state = 1;
    if (v->gain > 0x7ff) v->gain = 0x7ff;
  } else if (v->state == 1 && v->gain < v->sustain) {
    v->state = 2;
  }
}

/* The envelope's own step for one sample (after key on/off and reset). */
static inline void env_tick(DspFeVoice *v) {
  const bool releasing = v->state == 4;
  const bool live_gain = !releasing && v->use_gain;
  if (releasing) {
    env_gain(v, live_gain);
  } else if (!(live_gain && v->direct_gain)) {
    const uint16_t rate = live_gain ? v->rates[3] : v->rates[v->state > 2 ? 2 : v->state];
    if (rate != 0) {
      const uint16_t rc = (uint16_t)(v->rate_counter + 1);
      if (rc >= rate) { v->rate_counter = 0; env_gain(v, live_gain); }
      else v->rate_counter = rc;
    }
  } else {
    v->gain = v->gain_value;
  }
}

/* Step samples [te, s) of a voice with no decode or key on among them: key
 * off (even samples) and reset act on each, then the envelope steps. */
static void env_catch_up(DspFrontEnd *fe, DspFeVoice *v, uint32_t s) {
  if (v->pmod) { v->te = s; return; }
  for (uint32_t k = v->te; k < s; k++) {
    if (v->key_off && even_at(fe, k)) v->state = 4;
    if (fe->reset) { v->state = 4; v->gain = 0; }
    env_tick(v);
  }
  v->te = s;
}

void dsp_fe_env_sync(DspFrontEnd *fe, int i) { env_catch_up(fe, &fe->v[i], fe->now); }

uint8_t dsp_fe_envx(DspFrontEnd *fe, int i) {
  env_catch_up(fe, &fe->v[i], fe->now);
  return (uint8_t)(fe->v[i].gain >> 4);
}

static void decode(DspFrontEnd *fe, int i, uint32_t s) {
  DspFeVoice *v = &fe->v[i];
  const uint8_t *ram = fe->aram;
  if (v->prev_flags == 1 || v->prev_flags == 3) {   /* loop (and end) */
    const uint16_t p = (uint16_t)(fe->dir + 4 * v->srcn);
    if (fe->on_read) fe->on_read(fe->ctx, (uint16_t)(p + 2), 2);
    v->offset = (uint16_t)(ram[(uint16_t)(p + 2)] | ram[(uint16_t)(p + 3)] << 8);
    if (v->prev_flags == 1) { v->state = 4; v->gain = 0; }   /* end: release, gain 0 */
    fe->endx |= (uint8_t)(1u << i);
  }
  uint8_t b[9];
  if (fe->on_read) fe->on_read(fe->ctx, v->offset, 9);
  for (int k = 0; k < 9; k++) b[k] = ram[(uint16_t)(v->offset + k)];
  if (fe->on_block) fe->on_block(fe->ctx, i, s, v->offset, b);
  v->prev_flags = b[0] & 3;
  v->offset = (uint16_t)(v->offset + 9);
}

/* Sample s for voice v (its counter is at the start of s). */
static void voice_sample(DspFrontEnd *fe, int i, uint32_t s) {
  DspFeVoice *v = &fe->v[i];
  env_catch_up(fe, v, s);
  const uint32_t nc = (uint32_t)v->counter + v->pitch;
  if (nc > 0xffff) decode(fe, i, s);
  v->counter = (uint16_t)nc;
  if (even_at(fe, s)) {
    if (v->key_off) {
      v->state = 4;
    } else if (v->key_on) {
      v->key_on = false;
      v->prev_flags = 0;
      const uint16_t p = (uint16_t)(fe->dir + 4 * v->srcn);
      if (fe->on_read) fe->on_read(fe->ctx, p, 2);
      v->offset = (uint16_t)(fe->aram[p] | fe->aram[(uint16_t)(p + 1)] << 8);
      if (fe->on_keyon) fe->on_keyon(fe->ctx, i, s, v->offset);
      v->gain = 0;
      v->state = 0;
    }
  }
  if (fe->reset) { v->state = 4; v->gain = 0; }
  env_tick(v);
  v->t = s + 1;
  v->te = s + 1;
}

void dsp_fe_run(DspFrontEnd *fe, uint32_t target) {
  while (fe->next < target) {
    const uint32_t s = fe->next;
    fe->events++;
    for (int i = 0; i < 8; i++) {
      DspFeVoice *v = &fe->v[i];
      if (v->next != s) continue;
      voice_catch_up(v, s);
      voice_sample(fe, i, s);
      v->next = voice_next(fe, v, s + 1);
    }
    refresh_next(fe);
  }
  fe->now = target;
}

void dsp_fe_init(DspFrontEnd *fe, const Dsp *d, const uint8_t *aram, uint32_t now,
                 DspFeBlockFn *on_block, void *ctx) {
  memset(fe, 0, sizeof *fe);
  fe->aram = aram;
  fe->now = now;
  fe->even_base = d->evenCycle ? now : now + 1;
  fe->dir = d->dirPage;
  fe->endx = d->ram[0x7c];
  fe->reset = d->reset;
  fe->on_block = on_block;
  fe->ctx = ctx;
  for (int i = 0; i < 8; i++) {
    const DspChannel *c = &d->channel[i];
    DspFeVoice *v = &fe->v[i];
    v->t = now;
    v->pitch = c->pitch;
    v->counter = c->pitchCounter;
    v->offset = c->decodeOffset;
    v->srcn = c->srcn;
    v->prev_flags = c->previousFlags;
    v->key_on = c->keyOn;
    v->key_off = c->keyOff;
    v->pmod = i > 0 && c->pitchModulation;
    v->te = now;
    v->gain = c->gain;
    v->rate_counter = c->rateCounter;
    v->sustain = c->sustainLevel;
    v->gain_value = c->gainValue;
    for (int k = 0; k < 4; k++) v->rates[k] = c->adsrRates[k];
    v->state = c->adsrState;
    v->gain_mode = c->gainMode;
    v->use_gain = c->useGain;
    v->direct_gain = c->directGain;
    if (v->pmod) fe->untracked |= (uint8_t)(1u << i);
    v->next = voice_next(fe, v, now);
  }
  refresh_next(fe);
}

uint16_t dsp_fe_counter(const DspFrontEnd *fe, int i) {
  const DspFeVoice *v = &fe->v[i];
  return (uint16_t)(v->counter + (uint32_t)v->pitch * (fe->now - v->t));
}

/* Voice i's timing changes at fe->now: bring it current, apply, re-plan. */
#define VOICE_CHANGE(i, stmt)                                                \
  do {                                                                       \
    DspFeVoice *v_ = &fe->v[i];                                              \
    voice_catch_up(v_, fe->now);                                             \
    env_catch_up(fe, v_, fe->now);                                           \
    stmt;                                                                    \
    v_->next = voice_next(fe, v_, fe->now);                                  \
  } while (0)

void dsp_fe_write(DspFrontEnd *fe, uint8_t adr, uint8_t val) {
  const int ch = adr >> 4;
  switch (adr & 0x0f) {
    case 0x02:
      if (adr < 0x80) VOICE_CHANGE(ch, v_->pitch = (uint16_t)((v_->pitch & 0x3f00) | val));
      break;
    case 0x03:
      if (adr < 0x80)
        VOICE_CHANGE(ch, v_->pitch = (uint16_t)(((v_->pitch & 0x00ff) | (val << 8)) & 0x3fff));
      break;
    case 0x04:
      if (adr < 0x80) fe->v[ch].srcn = val;   /* read at the next loop or key on */
      break;
    case 0x05:
      if (adr < 0x80) {
        const int *rv = dsp_rate_values();
        VOICE_CHANGE(ch, (v_->rates[0] = (uint16_t)rv[(val & 0xf) * 2 + 1],
                          v_->rates[1] = (uint16_t)rv[((val & 0x70) >> 4) * 2 + 16],
                          v_->use_gain = (val & 0x80) == 0));
      }
      break;
    case 0x06:
      if (adr < 0x80) {
        const int *rv = dsp_rate_values();
        VOICE_CHANGE(ch, (v_->rates[2] = (uint16_t)rv[val & 0x1f],
                          v_->sustain = (uint16_t)((((val & 0xe0) >> 5) + 1) * 0x100)));
      }
      break;
    case 0x07:
      if (adr < 0x80) {
        const int *rv = dsp_rate_values();
        VOICE_CHANGE(ch, {
          v_->direct_gain = (val & 0x80) == 0;
          if (val & 0x80) {
            v_->gain_mode = (uint8_t)((val & 0x60) >> 5);
            v_->rates[3] = (uint16_t)rv[val & 0x1f];
          } else {
            v_->gain_value = (uint16_t)((val & 0x7f) * 16);
          }
        });
      }
      break;
    default:
      break;
  }
  switch (adr) {
    case 0x4c:
      for (int i = 0; i < 8; i++) VOICE_CHANGE(i, v_->key_on = (val >> i) & 1);
      break;
    case 0x5c:
      for (int i = 0; i < 8; i++) VOICE_CHANGE(i, v_->key_off = (val >> i) & 1);
      break;
    case 0x7c:
      fe->endx = 0;   /* any write clears ENDX */
      break;
    case 0x2d:
      for (int i = 1; i < 8; i++)
        if (((val >> i) & 1) && !fe->v[i].pmod) {
          VOICE_CHANGE(i, v_->pmod = true);
          fe->untracked |= (uint8_t)(1u << i);
        }
      /* Clearing PMON does not resume tracking: the counter is unknown. */
      break;
    case 0x5d:
      fe->dir = (uint16_t)(val << 8);
      break;
    case 0x6c:
      for (int i = 0; i < 8; i++) VOICE_CHANGE(i, (void)0);   /* envelopes to now first */
      fe->reset = (val & 0x80) != 0;
      break;
    default:
      break;
  }
  refresh_next(fe);
}
