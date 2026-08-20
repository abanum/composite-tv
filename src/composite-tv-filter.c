/*
Composite TV — OBS video filter
Copyright (C) 2026

Ported from the WebGL2 "NTSC Snow Simulator" (https://github.com/abanum/ZAA, MIT).
Models the NTSC receiver signal chain: encode -> Rician detect -> decode -> CRT.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "composite-tv-filter.h"
#include "composite-tv-signal.h"
#include "composite-tv-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>
#include <util/threading.h>
#include <math.h>
#include <string.h>

#include <plugin-support.h>

#define NS_PI 3.14159265358979323846

/* Fixed NTSC-M internal geometry (4fsc sampling). */
#define FIELD_W 754    /* active samples per line   */
#define FIELD_H 243    /* active lines per field    */
#define ACTIVE_LINES 486
#define TOTAL_LINES 525
/* Vertical blanking is whatever the frame is not active picture: 39 lines. */
#define VBLANK_LINES (TOTAL_LINES - ACTIVE_LINES)
#define SAMPLE_RATE_HZ 14318180.0 /* 4 * 3.579545 MHz */

/* Full-line signal raster: 910 samples per line (63.556us), a field of 20 VBI
 * rows plus the 243 active rows. Must match the SIG_* defines in the effect,
 * including its two invariants: the 156-sample active offset is a multiple of
 * four (so the subcarrier phase agrees between active and signal coordinates)
 * and the VBI row count is even (so the per-line phase alternation does). */
#define SIG_W 910
#define SIG_H 263
#define SIG_VBI_ROWS 20
#define LINE_PERIOD_US (SIG_W * 1.0e6 / SAMPLE_RATE_HZ) /* 63.556 */
#define PACK_H 264 /* signal rows plus one header row for the outside decoder */
#define BURST_PHASE_RAD (57.0 * NS_PI / 180.0)
#define CHROMA_DEMOD_LPF_HZ 600000.0
/* Settings are in MHz; the shader wants cutoffs normalised to the sample rate. */
#define MHZ_TO_NORM (1.0e6 / SAMPLE_RATE_HZ)

/* Tape dropout, from published VHS measurements. Nothing shorter than about
 * 0.5us can happen - the tape signal cannot fall away faster than that - and
 * the count falls off logarithmically as they get longer, so the short ones
 * dominate and the 300us ones are rare. */
#define DROPOUT_MIN_US 0.5
#define DROPOUT_MAX_US 200.0

/* FM sound carrier, as broadcast: 4.5 MHz above the vision carrier, +/-25 kHz
 * deviation, 75 us pre-emphasis. FM has memory - the phase is the integral of
 * the audio - so the carrier is synthesized on the CPU and handed to the
 * encoder as a texture. */
#define AUDIO_CARRIER_HZ 4.5e6
#define AUDIO_DEVIATION_HZ 25000.0
#define AUDIO_PREEMPH_TAU_S 75.0e-6
#define AUD_RING_LEN 65536 /* power of two */
#define AUD_PULL_MAX 2048  /* per-field cap; 48 kHz needs ~801 */

/* Power on/off envelope (seconds), ported from the reference PowerEnvelope. */
#define WARMUP_SEC 1.5f
#define COLLAPSE_V_SEC 0.13f
#define COLLAPSE_H_SEC 0.28f
#define AFTERGLOW_SEC 0.55f

enum power_state { POWER_OFF = 0, POWER_WARMING, POWER_ON, POWER_SHUTTING };

struct composite_tv {
	obs_source_t *source;
	gs_effect_t *effect;

	/* intermediate render targets */
	gs_texrender_t *input;
	gs_texrender_t *composite;
	gs_texrender_t *detector;
	gs_texrender_t *field[2];   /* ping-pong decoded fields (interlace weave) */
	gs_texrender_t *display[2]; /* ping-pong final frames (phosphor feedback) */
	int field_idx;
	int display_idx;

	/* animation state. field_counter drives the four-field sequence: its low
	 * bit is the line parity and the field base phase. */
	uint64_t last_time;
	double chroma_phase_acc;
	uint32_t field_counter;

	/* power on/off envelope */
	bool powered;                    /* target state (from settings / dock) */
	enum power_state power_state;
	float power_elapsed;             /* seconds in current state */

	/* parameters. Frequencies are stored already normalised to the sample
	 * rate, and plain gates (colour killer, glitch group) already applied. */
	float field_strength;
	float noise_floor;
	int yc_mode;
	float cutoff_i;
	float cutoff_q;
	float enc_chroma_gain;
	float peaking;
	int aspect_mode;
	int screen_aspect;
	float agc_level;
	float agc_jitter;
	float h_hold; /* how far the H-HOLD knob sits off its sweet spot */
	float if_cutoff;
	float luma_cutoff;
	float chroma_gain;
	float chroma_drift;

	/* FIR weight rotors, (cos, sin) of 2*pi*cutoff - the shader generates
	 * its windowed-sinc taps by rotation recurrence instead of per-tap
	 * sin/cos, and these are the per-tap steps. */
	float rot_y[2];
	float rot_i[2];
	float rot_q[2];
	float rot_c[2];
	float rot_if[2];
	float noise_norm_inv; /* 1/sqrt(sum of squared IF taps) */
	float contrast;
	float brightness;
	bool interlace;
	int scan_lines;      /* drawn lines per frame: 486 / 243 / 162 */
	bool line_skip;
	float gap_level;
	float persistence;
	float spot_v;
	float spot_h;
	float scanline;
	float curvature;
	float vignette;
	float overscan;

	/* glitches */
	float ghost_gain;
	float ghost_delay;
	float v_roll_speed;
	float h_jitter;
	float flagging;
	float flag_wave; /* tension wobble: the bend waves ("flagwaving") */
	float head_switch;
	float dropout;
	int dropout_mode; /* 0 none, 1 grey restorer, 2 1H delay line */
	float tape_damage;
	float tape_damage_pos;
	float damage_speed;
	float beat_gain;
	float beat_norm;
	float burst_len; /* reception burst duration */
	float sweep_sec; /* playback burst: seconds for the crease to cross once */

	/* glitch animation state. The two halves of the fault set run their own
	 * envelopes so a reception burst and a playback burst can overlap. */
	double v_roll_pos;
	double damage_pos; /* drift of the crease, 0..1 of picture height */
	double beat_phase;
	double flag_phase1; /* the two slow oscillators of the tension wobble */
	double flag_phase2;
	double afc_phase1; /* wander of the line oscillator's free-run error */
	double afc_phase2;
	float fading;       /* depth of the propagation fade */
	double fade_phase1; /* duct / scatter / polarization: slow and deep   */
	double fade_phase2; /* interference (two-ray): the seconds-scale beat */
	double fade_phase3; /* scintillation: the fast flutter, two of them   */
	double fade_phase4;
	double fade_phase5;       /* wander of the interfering path's excess delay  */
	float burst_rx;           /* 1 -> 0 reception burst */
	float burst_pb;           /* 1 -> 0 playback burst */
	long long glitch_pulse;   /* bumped by the dock to fire a reception burst */
	long long playback_pulse; /* and this one for a playback burst */
	obs_hotkey_id glitch_hotkey;
	obs_hotkey_id playback_hotkey;

	/* degauss */
	float degauss_len;
	float degauss_strength;
	float degauss;        /* 1 -> 0 envelope */
	double degauss_phase;
	long long degauss_pulse;
	obs_hotkey_id degauss_hotkey;
	obs_hotkey_id power_hotkey;

	/* FM sound carrier. The ring is fed from the audio thread and drained on
	 * the graphics thread, one field's worth at a time. */
	obs_weak_source_t *aud_weak;
	char *aud_name;
	pthread_mutex_t aud_mutex;
	float aud_ring[AUD_RING_LEN];
	uint64_t aud_write; /* absolute sample counters; masked on access */
	uint64_t aud_read;
	double fm_phase;          /* carrier phase, continuous across fields */
	double aud_need;          /* fractional samples owed to the next field */
	float pre_x1;             /* pre-emphasis differentiator state */
	float audio_gain;         /* carrier amplitude in video units, 0 = off */
	uint32_t aud_field_count; /* audio samples carried by this field */
	gs_texture_t *audio_tex;
	float *audio_buf;

	/* signal output for an outside decoder */
	int signal_format; /* 0 off, 1 16-bit R+G, 2 8-bit grayscale */
	int signal_tap;    /* 0 encoder output, 1 detector output */
	gs_texrender_t *pack;

	/* receiver tracking: per-row sync/burst measurements and the AFC */
	int color_killer_mode; /* 0 auto (burst-driven), 1 off, 2 forced mono */
	gs_stagesurf_t *dbg_stage; /* TEMPORARY: readback for tracking diagnosis */
	uint64_t dbg_last;
	gs_texrender_t *track;
	gs_texrender_t *flywheel[2]; /* ping-pong: the oscillator state carries across fields */
	int fly_idx;

	/* debug waveform overlay */
	bool scope_on;
	float scope_line;

	/* inspection zoom */
	float preview_zoom;
	float zoom_x;
	float zoom_y;

	/* shadow mask / aperture grille */
	int mask_type;      /* 0 off, 1 slot, 2 dot, 3 grille */
	float mask_strength;
	float mask_width;   /* phosphor stripe half-width (fill factor) */
	float mask_pitch;   /* RGB triads across the tube width */
	float wire_width;   /* damper wire shadow width, in triad pitches */
};

/* ---- small effect-parameter setters ---------------------------------- */

static inline void set_f(gs_effect_t *e, const char *n, float v)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, n);
	if (p)
		gs_effect_set_float(p, v);
}

static inline void set_i(gs_effect_t *e, const char *n, int v)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, n);
	if (p)
		gs_effect_set_int(p, v);
}

static inline void set_v2(gs_effect_t *e, const char *n, float x, float y)
{
	struct vec2 v;
	vec2_set(&v, x, y);
	gs_eparam_t *p = gs_effect_get_param_by_name(e, n);
	if (p)
		gs_effect_set_vec2(p, &v);
}

static inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static inline float smoothstepf(float e0, float e1, float x)
{
	float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

/* (cos, sin) of the per-tap angle for a normalised cutoff. */
static inline void fir_rotor(float rot[2], float fcn)
{
	rot[0] = (float)cos(2.0 * NS_PI * (double)fcn);
	rot[1] = (float)sin(2.0 * NS_PI * (double)fcn);
}

/* One windowed-sinc tap, mirroring the shader's weight formula. */
static double fir_tap(int k, double fcn, int radius)
{
	double s = 1.0;
	if (k != 0) {
		double x = 2.0 * NS_PI * fcn * (double)k;
		s = sin(x) / x;
	}
	return 2.0 * fcn * s * (0.54 + 0.46 * cos(NS_PI * (double)k / (double)(radius + 1)));
}

/* The detector divides its band-limited noise by the RMS of the tap weights;
 * that constant only depends on the IF cutoff, so it is computed here. */
static float detect_norm_inv(float fcn)
{
	double sum = 0.0;
	for (int k = -6; k <= 6; ++k) {
		double w = fir_tap(k, (double)fcn, 6);
		sum += w * w;
	}
	return (float)(1.0 / sqrt(sum > 1.0e-6 ? sum : 1.0e-6));
}

/* Depth of the vertical blanking interval in drawn lines. Fixed by the NTSC
 * standard - it is a property of the signal, not something a receiver fault
 * changes - so it only has to be rescaled when the frame is drawn with a
 * different line count. */
static inline float bar_lines(const struct composite_tv *f)
{
	return (float)f->scan_lines * ((float)VBLANK_LINES / (float)ACTIVE_LINES);
}

static inline void set_tex(gs_effect_t *e, const char *n, gs_texture_t *t)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, n);
	if (p)
		gs_effect_set_texture(p, t);
}

/* ---- power on/off envelope ------------------------------------------- */

static inline float ease_out_expo(float x)
{
	return x >= 1.0f ? 1.0f : 1.0f - powf(2.0f, -10.0f * x);
}

static void power_advance(struct composite_tv *f, float dt)
{
	/* edge-triggered transitions from the target 'powered' state */
	if (f->powered && (f->power_state == POWER_OFF || f->power_state == POWER_SHUTTING)) {
		f->power_state = POWER_WARMING;
		f->power_elapsed = 0.0f;
	} else if (!f->powered && (f->power_state == POWER_ON || f->power_state == POWER_WARMING)) {
		f->power_state = POWER_SHUTTING;
		f->power_elapsed = 0.0f;
	}

	f->power_elapsed += dt;
	if (f->power_state == POWER_WARMING && f->power_elapsed >= WARMUP_SEC) {
		f->power_state = POWER_ON;
	} else if (f->power_state == POWER_SHUTTING &&
		   f->power_elapsed >= COLLAPSE_V_SEC + COLLAPSE_H_SEC + AFTERGLOW_SEC) {
		f->power_state = POWER_OFF;
	}
}

/* What the display stage needs to know about the power envelope. Deflection is
 * normal and there is no flash unless we are shutting down, so that is the
 * initialiser and only the interesting cases assign. */
struct power_gfx {
	float warmup;
	float collapse_v;
	float collapse_h;
	float flash;
};

static struct power_gfx power_uniforms(const struct composite_tv *f)
{
	struct power_gfx g = {0.0f, 1.0f, 1.0f, 0.0f};
	float t = f->power_elapsed;

	if (f->power_state == POWER_WARMING) {
		float tn = clampf(t / WARMUP_SEC, 0.0f, 1.0f);
		g.warmup = tn * tn * (3.0f - 2.0f * tn);
	} else if (f->power_state == POWER_ON) {
		g.warmup = 1.0f;
	} else if (f->power_state == POWER_SHUTTING) {
		g.warmup = 1.0f;
		if (t < COLLAPSE_V_SEC) {
			float p = ease_out_expo(t / COLLAPSE_V_SEC);
			g.collapse_v = 1.0f - p;
			g.flash = p * 0.6f;
		} else if (t < COLLAPSE_V_SEC + COLLAPSE_H_SEC) {
			float p = ease_out_expo((t - COLLAPSE_V_SEC) / COLLAPSE_H_SEC);
			g.collapse_v = 0.0f;
			g.collapse_h = 1.0f - p;
			g.flash = 0.6f + p * 0.4f;
		} else {
			float p = (t - COLLAPSE_V_SEC - COLLAPSE_H_SEC) / AFTERGLOW_SEC;
			float fade = expf(-4.0f * (p > 1.0f ? 1.0f : p));
			g.warmup = fade;
			g.collapse_v = 0.0f;
			g.collapse_h = 0.0f;
			g.flash = fade;
		}
	}
	return g;
}

/* ---- FM sound carrier: audio tap ------------------------------------- */

/* Runs on the audio thread: downmix to mono and push into the ring. Nothing
 * else happens here - the graphics thread drains and synthesizes. */
static void ntsc_audio_cb(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	struct composite_tv *f = param;
	UNUSED_PARAMETER(source);
	if (!audio || !audio->frames)
		return;
	const float *l = (const float *)audio->data[0];
	const float *r = (const float *)audio->data[1];

	pthread_mutex_lock(&f->aud_mutex);
	for (uint32_t i = 0; i < audio->frames; i++) {
		float smp = 0.0f;
		if (!muted && l)
			smp = r ? 0.5f * (l[i] + r[i]) : l[i];
		f->aud_ring[f->aud_write & (AUD_RING_LEN - 1)] = smp;
		f->aud_write++;
	}
	/* Bound the backlog so the carrier never runs seconds behind. */
	if (f->aud_write - f->aud_read > AUD_RING_LEN / 2)
		f->aud_read = f->aud_write - AUD_RING_LEN / 2;
	pthread_mutex_unlock(&f->aud_mutex);
}

static void ntsc_detach_audio(struct composite_tv *f)
{
	if (f->aud_weak) {
		obs_source_t *src = obs_weak_source_get_source(f->aud_weak);
		if (src) {
			obs_source_remove_audio_capture_callback(src, ntsc_audio_cb, f);
			obs_source_release(src);
		}
		obs_weak_source_release(f->aud_weak);
		f->aud_weak = NULL;
	}
}

static void ntsc_attach_audio(struct composite_tv *f, const char *name)
{
	ntsc_detach_audio(f);
	if (!name || !*name)
		return;
	obs_source_t *src = obs_get_source_by_name(name);
	if (!src)
		return;
	obs_source_add_audio_capture_callback(src, ntsc_audio_cb, f);
	f->aud_weak = obs_source_get_weak_source(src);
	obs_source_release(src);
}

/* ---- OBS source callbacks -------------------------------------------- */

static const char *ntsc_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("CompositeTV.Name");
}

static void ntsc_update(void *data, obs_data_t *s)
{
	struct composite_tv *f = data;
	f->field_strength = (float)obs_data_get_double(s, "field_strength");
	f->noise_floor = (float)obs_data_get_double(s, "noise_floor");
	f->yc_mode = (int)obs_data_get_int(s, "yc_mode");
	f->cutoff_i = (float)(obs_data_get_double(s, "chroma_band_i") * MHZ_TO_NORM);
	f->cutoff_q = (float)(obs_data_get_double(s, "chroma_band_q") * MHZ_TO_NORM);
	f->enc_chroma_gain = (float)obs_data_get_double(s, "enc_chroma_gain");
	f->aspect_mode = (int)obs_data_get_int(s, "aspect_mode");
	f->screen_aspect = (int)obs_data_get_int(s, "screen_aspect");
	f->powered = obs_data_get_bool(s, "power");
	f->agc_level = (float)obs_data_get_double(s, "agc_level");
	f->agc_jitter = (float)obs_data_get_double(s, "agc_jitter");
	f->h_hold = (float)obs_data_get_double(s, "h_hold");
	f->if_cutoff = (float)(obs_data_get_double(s, "if_bandwidth") * 0.5 * MHZ_TO_NORM);
	f->luma_cutoff = (float)(obs_data_get_double(s, "luma_bandwidth") * MHZ_TO_NORM);
	/* Colour killer. Scenes saved before it became a mode carry the old
	 * checkbox; a ticked one meant "always monochrome", which is the forced
	 * mode. Auto (the default) is decided per line by the measured burst in
	 * the shader; only the forced mode still folds into the gain here. */
	if (!obs_data_has_user_value(s, "color_killer_mode") && obs_data_get_bool(s, "color_killer"))
		obs_data_set_int(s, "color_killer_mode", 2);
	f->color_killer_mode = (int)obs_data_get_int(s, "color_killer_mode");
	f->chroma_gain = (f->color_killer_mode == 2) ? 0.0f : (float)obs_data_get_double(s, "chroma_gain");
	f->chroma_drift = (float)obs_data_get_double(s, "chroma_drift");

	/* Everything the shader's FIR recurrences need from the cutoffs. */
	fir_rotor(f->rot_y, f->luma_cutoff);
	fir_rotor(f->rot_i, f->cutoff_i);
	fir_rotor(f->rot_q, f->cutoff_q);
	fir_rotor(f->rot_c, (float)(CHROMA_DEMOD_LPF_HZ / SAMPLE_RATE_HZ));
	fir_rotor(f->rot_if, f->if_cutoff);
	f->noise_norm_inv = detect_norm_inv(f->if_cutoff);

	f->contrast = (float)obs_data_get_double(s, "contrast");
	f->brightness = (float)obs_data_get_double(s, "brightness");
	f->interlace = obs_data_get_bool(s, "interlace");
	f->scan_lines = (int)obs_data_get_int(s, "scan_lines");
	if (f->scan_lines < 1)
		f->scan_lines = ACTIVE_LINES;
	f->line_skip = obs_data_get_bool(s, "line_skip");
	/* Gate the gap depth here so the render path can just read it. */
	f->gap_level = f->line_skip ? (float)obs_data_get_double(s, "gap_level") : 1.0f;
	f->persistence = (float)obs_data_get_double(s, "persistence");
	f->spot_v = (float)obs_data_get_double(s, "spot_v");
	f->spot_h = (float)obs_data_get_double(s, "spot_h");
	f->scanline = (float)obs_data_get_double(s, "scanline");
	f->curvature = (float)obs_data_get_double(s, "curvature");
	f->vignette = (float)obs_data_get_double(s, "vignette");
	f->overscan = (float)obs_data_get_double(s, "overscan");

	/* Scenes saved before the faults were split in two carry a single switch
	 * for both halves, so hand the new one the old one's state - otherwise
	 * every tape fault would quietly switch itself off on upgrade. */
	if (!obs_data_has_user_value(s, "playback_enable") && obs_data_get_bool(s, "glitch_enable"))
		obs_data_set_bool(s, "playback_enable", true);

	/* Each half is gated by its own group toggle, so apply the gates once
	 * here and let the render path just read the values. */
	const bool rx = obs_data_get_bool(s, "glitch_enable");
	const bool pb = obs_data_get_bool(s, "playback_enable");
	f->ghost_gain = rx ? (float)obs_data_get_double(s, "ghost_gain") : 0.0f;
	f->ghost_delay = rx ? (float)obs_data_get_double(s, "ghost_delay") : 0.0f;
	f->v_roll_speed = rx ? (float)obs_data_get_double(s, "v_roll_speed") : 0.0f;
	f->h_jitter = rx ? (float)obs_data_get_double(s, "h_jitter") : 0.0f;
	f->fading = rx ? (float)obs_data_get_double(s, "fading") : 0.0f;
	f->beat_gain = rx ? (float)obs_data_get_double(s, "beat_gain") : 0.0f;
	f->beat_norm = (float)(obs_data_get_double(s, "beat_freq") * MHZ_TO_NORM);
	f->burst_len = (float)obs_data_get_double(s, "burst_len");

	f->flagging = pb ? (float)obs_data_get_double(s, "flagging") : 0.0f;
	f->flag_wave = pb ? (float)obs_data_get_double(s, "flag_wave") : 0.0f;
	f->head_switch = pb ? (float)obs_data_get_double(s, "head_switch") : 0.0f;
	f->peaking = pb ? (float)obs_data_get_double(s, "peaking") : 0.0f;
	f->dropout = pb ? (float)obs_data_get_double(s, "dropout") : 0.0f;
	f->dropout_mode = (int)obs_data_get_int(s, "dropout_mode");
	f->tape_damage = pb ? (float)obs_data_get_double(s, "tape_damage") : 0.0f;
	f->tape_damage_pos = (float)obs_data_get_double(s, "tape_damage_pos");
	f->damage_speed = pb ? (float)obs_data_get_double(s, "tape_damage_drift") : 0.0f;
	f->sweep_sec = (float)obs_data_get_double(s, "sweep_sec");

	f->degauss_len = (float)obs_data_get_double(s, "degauss_len");
	f->degauss_strength = (float)obs_data_get_double(s, "degauss_strength");
	f->signal_format = (int)obs_data_get_int(s, "signal_format");
	f->signal_tap = (int)obs_data_get_int(s, "signal_tap");

	/* FM sound carrier: (re)tap the chosen source. The weak reference makes
	 * deletion safe; a source recreated under the same name is picked up on
	 * the next settings change, because the expiry check runs here too. */
	f->audio_gain = (float)obs_data_get_double(s, "audio_carrier_gain");
	const char *an = obs_data_get_string(s, "carrier_audio_source");
	bool aud_renamed = !f->aud_name || strcmp(f->aud_name, an) != 0;
	if (aud_renamed) {
		bfree(f->aud_name);
		f->aud_name = bstrdup(an);
	}
	if (aud_renamed || (f->aud_name[0] && (!f->aud_weak || obs_weak_source_expired(f->aud_weak))))
		ntsc_attach_audio(f, f->aud_name);

	f->scope_on = obs_data_get_bool(s, "scope_on");
	f->scope_line = (float)obs_data_get_double(s, "scope_line");
	f->preview_zoom = (float)obs_data_get_double(s, "preview_zoom");
	f->zoom_x = (float)obs_data_get_double(s, "zoom_x");
	f->zoom_y = (float)obs_data_get_double(s, "zoom_y");
	f->mask_type = (int)obs_data_get_int(s, "mask_type");
	f->mask_strength = (float)obs_data_get_double(s, "mask_strength");
	f->mask_width = (float)obs_data_get_double(s, "mask_width");
	if (f->mask_width < 0.05f)
		f->mask_width = 0.62f;
	f->mask_pitch = (float)obs_data_get_double(s, "mask_pitch");
	if (f->mask_pitch < 1.0f)
		f->mask_pitch = 450.0f;
	/* The guard only catches a missing key (which reads as 0.0); the slider
	 * itself goes down to 0.01 for a hairline wire. */
	f->wire_width = (float)obs_data_get_double(s, "wire_width");
	if (f->wire_width < 0.005f)
		f->wire_width = 0.01f;

	/* The dock fires a burst by incrementing one of these counters. */
	long long pulse = obs_data_get_int(s, "glitch_pulse");
	if (pulse != f->glitch_pulse) {
		f->glitch_pulse = pulse;
		f->burst_rx = 1.0f;
	}
	long long ppulse = obs_data_get_int(s, "playback_pulse");
	if (ppulse != f->playback_pulse) {
		f->playback_pulse = ppulse;
		f->burst_pb = 1.0f;
	}
	long long dpulse = obs_data_get_int(s, "degauss_pulse");
	if (dpulse != f->degauss_pulse) {
		f->degauss_pulse = dpulse;
		f->degauss = 1.0f;
	}
}

/* Hotkey: fire a momentary reception burst. */
static void ntsc_glitch_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct composite_tv *f = data;
	f->burst_rx = 1.0f;
}

/* Hotkey: fire a momentary playback burst - the tape faults. */
static void ntsc_playback_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct composite_tv *f = data;
	f->burst_pb = 1.0f;
}

/* Hotkey: run the degauss coil. */
static void ntsc_degauss_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct composite_tv *f = data;
	f->degauss = 1.0f;
}

/* Hotkey: toggle mains power. Routed through the settings so the properties
 * dialog and the dock stay in sync with the new state. */
static void ntsc_power_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct composite_tv *f = data;
	obs_data_t *s = obs_data_create();
	obs_data_set_bool(s, "power", !f->powered);
	obs_source_update(f->source, s);
	obs_data_release(s);
}

/* The hotkey settings UI only lists sources it shows elsewhere (scenes and
 * regular sources), never the filter's own private source, so the hotkeys
 * must be registered on the parent the filter is attached to. */
static void ntsc_filter_add(void *data, obs_source_t *parent)
{
	struct composite_tv *f = data;
	if (f->glitch_hotkey != OBS_INVALID_HOTKEY_ID)
		return;
	f->glitch_hotkey = obs_hotkey_register_source(parent, "composite_tv.glitch",
						      obs_module_text("CompositeTV.Hotkey.Glitch"),
						      ntsc_glitch_hotkey, f);
	f->playback_hotkey = obs_hotkey_register_source(parent, "composite_tv.playback",
							obs_module_text("CompositeTV.Hotkey.Playback"),
							ntsc_playback_hotkey, f);
	f->degauss_hotkey = obs_hotkey_register_source(parent, "composite_tv.degauss",
						       obs_module_text("CompositeTV.Hotkey.Degauss"),
						       ntsc_degauss_hotkey, f);
	f->power_hotkey = obs_hotkey_register_source(parent, "composite_tv.power",
						     obs_module_text("CompositeTV.Hotkey.Power"),
						     ntsc_power_hotkey, f);
}

static void ntsc_unregister_hotkeys(struct composite_tv *f)
{
	if (f->glitch_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(f->glitch_hotkey);
	if (f->playback_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(f->playback_hotkey);
	if (f->degauss_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(f->degauss_hotkey);
	if (f->power_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(f->power_hotkey);
	f->glitch_hotkey = OBS_INVALID_HOTKEY_ID;
	f->playback_hotkey = OBS_INVALID_HOTKEY_ID;
	f->degauss_hotkey = OBS_INVALID_HOTKEY_ID;
	f->power_hotkey = OBS_INVALID_HOTKEY_ID;
}

static void ntsc_filter_remove(void *data, obs_source_t *parent)
{
	UNUSED_PARAMETER(parent);
	ntsc_unregister_hotkeys(data);
}

static void *ntsc_create(obs_data_t *settings, obs_source_t *source)
{
	struct composite_tv *f = bzalloc(sizeof(struct composite_tv));
	f->source = source;
	/* Registered on the parent in ntsc_filter_add; 0 is a valid hotkey id,
	 * so the zeroed struct must not be left as-is. */
	f->glitch_hotkey = OBS_INVALID_HOTKEY_ID;
	f->playback_hotkey = OBS_INVALID_HOTKEY_ID;
	f->degauss_hotkey = OBS_INVALID_HOTKEY_ID;
	f->power_hotkey = OBS_INVALID_HOTKEY_ID;
	pthread_mutex_init(&f->aud_mutex, NULL);

	char *path = obs_module_file("effects/composite-tv.effect");
	obs_enter_graphics();
	char *errors = NULL;
	f->effect = gs_effect_create_from_file(path, &errors);
	obs_leave_graphics();
	if (!f->effect) {
		obs_log(LOG_ERROR, "Failed to load composite-tv.effect: %s", errors ? errors : "(unknown)");
	}
	bfree(errors);
	bfree(path);

	ntsc_update(f, settings);
	/* Start already settled in the target state, with nothing animating: the
	 * saved pulse counters are momentary events that were fired in an earlier
	 * session, so adopting their values must not replay them now. */
	f->power_state = f->powered ? POWER_ON : POWER_OFF;
	f->power_elapsed = 0.0f;
	f->burst_rx = 0.0f;
	f->burst_pb = 0.0f;
	f->degauss = 0.0f;
	return f;
}

static void free_texrender(gs_texrender_t **tr)
{
	if (*tr) {
		gs_texrender_destroy(*tr);
		*tr = NULL;
	}
}

static void ntsc_destroy(void *data)
{
	struct composite_tv *f = data;
	ntsc_unregister_hotkeys(f);
	/* Detach before tearing anything down: after this the audio thread can
	 * no longer walk into the ring or the mutex, and no signal source can
	 * find the pack texture any more. Freeing the textures below happens
	 * inside obs_enter_graphics(), which cannot interleave with a render
	 * that already picked the pointer up. */
	ntsc_detach_audio(f);
	ctv_signal_unpublish(f);
	obs_enter_graphics();
	if (f->effect)
		gs_effect_destroy(f->effect);
	free_texrender(&f->input);
	free_texrender(&f->composite);
	free_texrender(&f->detector);
	free_texrender(&f->field[0]);
	free_texrender(&f->field[1]);
	free_texrender(&f->display[0]);
	free_texrender(&f->display[1]);
	free_texrender(&f->pack);
	free_texrender(&f->track);
	free_texrender(&f->flywheel[0]);
	free_texrender(&f->flywheel[1]);
	if (f->dbg_stage)
		gs_stagesurface_destroy(f->dbg_stage);
	if (f->audio_tex)
		gs_texture_destroy(f->audio_tex);
	obs_leave_graphics();
	pthread_mutex_destroy(&f->aud_mutex);
	bfree(f->audio_buf);
	bfree(f->aud_name);
	bfree(f);
}

/* Ensure a texrender exists with the requested format. */
static void ensure_tr(gs_texrender_t **tr, enum gs_color_format fmt)
{
	if (!*tr)
		*tr = gs_texrender_create(fmt, GS_ZS_NONE);
}

/* Start drawing into a texrender with the pipeline state every pass expects.
 * The full-screen passes overwrite every pixel with ONE/ZERO blending, so only
 * the input capture - where the source need not cover the surface - asks for
 * a clear. */
static bool target_begin(gs_texrender_t *tr, uint32_t w, uint32_t h, bool clear)
{
	if (!tr)
		return false;
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, w, h))
		return false;

	if (clear) {
		struct vec4 black;
		vec4_zero(&black);
		gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
	}
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	return true;
}

static void target_end(gs_texrender_t *tr)
{
	gs_blend_state_pop();
	gs_texrender_end(tr);
}

static void apply_params(struct composite_tv *f, gs_effect_t *e, uint32_t cx, uint32_t cy);

/* Render one full-screen technique into a texrender; returns its texture.
 * Applying the parameters in here - rather than at each call site - is what
 * makes the libobs quirk described on apply_params() impossible to trip over. */
static gs_texture_t *run_pass(struct composite_tv *f, gs_effect_t *e, gs_texrender_t *tr, uint32_t w, uint32_t h,
			      const char *tech, gs_texture_t *img, uint32_t cx, uint32_t cy)
{
	if (!target_begin(tr, w, h, false))
		return NULL;

	apply_params(f, e, cx, cy);
	set_tex(e, "image", img);
	while (gs_effect_loop(e, tech))
		gs_draw_sprite(img, 0, w, h);

	target_end(tr);
	return gs_texrender_get_texture(tr);
}

/* Assign every scalar effect parameter. libobs clears all parameter values at
 * the end of each technique (gs_technique_end -> da_resize(cur_val, 0)), so
 * this has to run again before every pass or the D3D11 backend silently skips
 * the draw ("Not all shader parameters were set"). run_pass() is the only
 * caller precisely so that nobody has to remember this. */
static void apply_params(struct composite_tv *f, gs_effect_t *e, uint32_t cx, uint32_t cy)
{
	/* Fields alternate 0,PI,0,PI over the four-field sequence. */
	const float field_base_phase = (f->field_counter & 1u) ? (float)NS_PI : 0.0f;

	set_v2(e, "field_size", (float)FIELD_W, (float)FIELD_H);
	set_v2(e, "output_size", (float)cx, (float)cy);
	set_f(e, "burst_phase", (float)BURST_PHASE_RAD);
	set_f(e, "field_base_phase", field_base_phase);
	/* Encode */
	set_f(e, "luma_cutoff", f->luma_cutoff);
	set_f(e, "cutoff_i", f->cutoff_i);
	set_f(e, "cutoff_q", f->cutoff_q);
	set_f(e, "enc_chroma_gain", f->enc_chroma_gain);
	set_f(e, "audio_gain", f->audio_gain);
	set_f(e, "peaking", f->peaking);
	set_f(e, "scope_on", f->scope_on ? 1.0f : 0.0f);
	/* The setting is a scan line of the 486-line frame, which is what the
	 * viewer sees; the signal itself only has 243 rows per field, so two
	 * neighbouring lines share one trace. */
	/* The slider is a scan line of the 486-line frame; the shader wants the
	 * signal row, which is the field row plus the VBI block above it. */
	set_f(e, "scope_line", floorf(f->scope_line * ((float)FIELD_H / (float)ACTIVE_LINES)) + (float)SIG_VBI_ROWS);
	/* Trace a line in the lower half and the panel moves out of its way. */
	set_f(e, "scope_top", f->scope_line >= 241.0f ? 1.0f : 0.0f);

	/* Signal output header metadata. The format the header reports is the
	 * one actually packed: with the display mode on "none" the published
	 * raster is still 16-bit, so the header must say 1, not 0. */
	set_f(e, "pack_format", (float)(f->signal_format == 0 ? 1 : f->signal_format));
	set_f(e, "pack_tap", (float)f->signal_tap);
	set_f(e, "pack_field", (float)(f->field_counter & 3u));
	set_f(e, "pack_aud_count", (float)f->aud_field_count);
	set_f(e, "pack_aud_present", f->audio_gain > 0.0f ? 1.0f : 0.0f);
	set_f(e, "aspect_mode", (float)f->aspect_mode);
	set_f(e, "input_aspect", (float)cx / (float)cy);
	float scr_aspect = (f->screen_aspect == 1)   ? (4.0f / 3.0f)
			   : (f->screen_aspect == 2) ? (16.0f / 9.0f)
						     : ((float)cx / (float)cy);
	set_f(e, "screen_aspect", scr_aspect);
	/* Detect */
	set_i(e, "field_seed", (int)(f->field_counter & 0xFFFFFu));
	set_f(e, "if_cutoff", f->if_cutoff);
	/* Fading, composed the way the propagation handbooks split it: a slow
	 * deep swell (duct, scatter, polarization), a seconds-scale beat from
	 * two-ray interference, and the fast small flutter of scintillation.
	 * Everything downstream reacts on its own: the snow breathes, colour
	 * drops in and out at the killer threshold, and the horizontal lock
	 * swims whenever a dip crosses the margin. */
	float fs_eff = f->field_strength;
	float fade_dip = 0.0f;
	if (f->fading > 0.0f) {
		float fw = 0.50f * (float)sin(f->fade_phase1) + 0.35f * (float)sin(f->fade_phase2) +
			   0.15f * (0.6f * (float)sin(f->fade_phase3) + 0.4f * (float)sin(f->fade_phase4));
		fade_dip = 0.5f + 0.5f * fw; /* 0 = crest, 1 = deepest */
		fs_eff *= clampf(1.0f - f->fading * fade_dip, 0.0f, 1.0f);
	}
	set_f(e, "field_strength", fs_eff);
	set_f(e, "noise_floor", f->noise_floor);
	set_f(e, "agc_jitter", f->agc_jitter);
	set_f(e, "afc_slop", f->h_hold);
	set_f(e, "snow_level", f->agc_level);
	set_f(e, "noise_norm_inv", f->noise_norm_inv);
	set_f(e, "det_sigma", 1.0f + (f->noise_floor - 1.0f) * fs_eff);
	set_f(e, "det_blend", smoothstepf(0.0f, 0.35f, fs_eff));
	/* Decode. The free-running oscillator only rotates the hue in the
	 * no-signal limit - with a picture the receiver locks to the burst. */
	set_f(e, "chroma_cutoff", (float)(CHROMA_DEMOD_LPF_HZ / SAMPLE_RATE_HZ));
	set_f(e, "chroma_gain", f->chroma_gain);
	/* The free-run drift is no longer gated by the field strength here: the
	 * decoder blends it in by the measured burst quality, which is the same
	 * decision a real oscillator makes - and for the same reason. */
	set_f(e, "cyc_phase", (float)f->chroma_phase_acc);
	set_f(e, "killer_auto", f->color_killer_mode == 0 ? 1.0f : 0.0f);
	/* Free-run frequency error of the line oscillator, in samples per line:
	 * only matters when the separator stops trusting the sync. */
	set_f(e, "afc_drift",
	      0.35f * (0.6f * (float)sin(f->afc_phase1) + 0.4f * (float)sin(f->afc_phase2)));
	set_f(e, "contrast", f->contrast);
	set_f(e, "brightness", f->brightness);
	set_f(e, "yc_mode", (float)f->yc_mode);
	/* FIR rotors shared by Encode / Detect / Decode */
	set_v2(e, "fir_rot_y", f->rot_y[0], f->rot_y[1]);
	set_v2(e, "fir_rot_i", f->rot_i[0], f->rot_i[1]);
	set_v2(e, "fir_rot_q", f->rot_q[0], f->rot_q[1]);
	set_v2(e, "fir_rot_c", f->rot_c[0], f->rot_c[1]);
	set_v2(e, "fir_rot_if", f->rot_if[0], f->rot_if[1]);
	/* Display */
	const float lines = (float)f->scan_lines;
	set_f(e, "active_lines", lines);
	set_f(e, "field_row_step", (float)FIELD_H / lines);
	set_f(e, "line_skip", f->line_skip ? 1.0f : 0.0f);
	set_f(e, "gap_level", f->gap_level);
	/* 240p and below are progressive by definition, so the field weave only
	 * applies to a full 486-line frame. */
	set_f(e, "interlace_on", (f->interlace && f->scan_lines == ACTIVE_LINES) ? 1.0f : 0.0f);
	set_f(e, "frame_parity", (float)(f->field_counter & 1u));
	set_f(e, "persistence", f->persistence);
	set_f(e, "spot_v", f->spot_v);
	set_f(e, "spot_dx", f->spot_h * 0.5f / (float)FIELD_W);
	set_f(e, "scanline", f->scanline);
	set_f(e, "curvature", f->curvature);
	set_f(e, "vignette", f->vignette);
	set_f(e, "overscan", f->overscan);
	set_f(e, "inv_active_lines", 1.0f / lines);
	/* Zooming in means each scan line covers proportionally more output
	 * pixels, so the scan-line visibility ramp has to know about it -
	 * otherwise magnifying would not reveal the lines it exists to show. */
	const float zoom = f->preview_zoom > 1.0f ? f->preview_zoom : 1.0f;
	set_f(e, "line_visible", smoothstepf(2.2f, 3.0f, (float)cy / lines * zoom));
	set_f(e, "preview_zoom", zoom);
	set_f(e, "zoom_x", f->zoom_x);
	set_f(e, "zoom_y", f->zoom_y);

	/* How the tube fits in the canvas: one axis stretches, the other is 1. */
	const float out_aspect = (float)cx / (float)cy;
	if (scr_aspect < out_aspect)
		set_v2(e, "screen_fit", out_aspect / scr_aspect, 1.0f);
	else
		set_v2(e, "screen_fit", 1.0f, scr_aspect / out_aspect);

	/* Shadow mask. The triad density is defined across the tube, which is
	 * narrower than the canvas when it is pillarboxed, so measure the tube
	 * width in output pixels for the visibility ramp. */
	const float tube_px = (scr_aspect < out_aspect) ? (float)cy * scr_aspect : (float)cx;
	set_f(e, "mask_type", (float)f->mask_type);
	set_f(e, "mask_strength", f->mask_strength);
	set_f(e, "mask_width", f->mask_width);
	set_f(e, "mask_triads", f->mask_pitch);
	set_f(e, "pixels_per_triad", tube_px * zoom / f->mask_pitch);
	set_f(e, "wire_width", f->wire_width);

	/* Glitches. The steady-state values were already gated by their group in
	 * ntsc_update(); the momentary bursts add on top, so the dock buttons and
	 * the hotkeys still work from a clean picture. Each fault answers to the
	 * burst of the half it belongs to - a reception burst leaves the tape
	 * alone and a playback burst leaves the aerial alone. */
	const float brx = f->burst_rx;
	const float bpb = f->burst_pb;
	/* Two-ray physics of interference fading: a deep dip means a second path
	 * of nearly equal strength arriving in antiphase - and a path like that
	 * IS a ghost. So as the fade approaches its trough, an echo stands up,
	 * on the user's own ghost path if one is set, else on a wandering path
	 * of its own. The wander is the reflection point moving. */
	float gg = f->ghost_gain;
	float gd = f->ghost_delay;
	if (f->fading > 0.0f) {
		float two_ray = f->fading * smoothstepf(0.55f, 0.95f, fade_dip);
		gg += 0.35f * two_ray;
		if (f->ghost_delay == 0.0f)
			gd = 16.0f + 7.0f * (float)sin(f->fade_phase5);
	}
	set_f(e, "ghost_gain", gg);
	set_f(e, "ghost_delay", gd);
	set_f(e, "beat_gain", f->beat_gain);
	set_f(e, "beat_norm", f->beat_norm);
	set_f(e, "beat_phase", (float)f->beat_phase);
	set_f(e, "v_roll", (float)f->v_roll_pos);
	set_f(e, "vblank_lines", bar_lines(f));
	set_f(e, "h_jitter", clampf(f->h_jitter + brx * 0.8f, 0.0f, 2.0f));
	/* Flagging can bend either way - the sign of the tension error decides -
	 * and a wandering tension makes the bend wave. The wobble is two slow
	 * incommensurate sines, so the flag never settles into a loop, and the
	 * burst pushes away from zero in whichever direction is already live. */
	float flag_eff =
		f->flagging + f->flag_wave * (0.6f * (float)sin(f->flag_phase1) + 0.4f * (float)sin(f->flag_phase2));
	flag_eff += bpb * 0.6f * (flag_eff < 0.0f ? -1.0f : 1.0f);
	set_f(e, "flagging", clampf(flag_eff, -2.0f, 2.0f));
	set_f(e, "head_switch", clampf(f->head_switch + bpb * 0.5f, 0.0f, 2.0f));
	/* Dropout. It lives in the signal now, so its lengths are line periods
	 * and its rate is per field row - nothing here depends on how many lines
	 * the tube happens to be drawing. */
	const float dop = clampf(f->dropout + bpb * 0.6f, 0.0f, 1.6f);
	float prob = 0.0f;
	if (dop > 0.0f) {
		/* The bottom of the slider sits on the measured ten dropouts a
		 * minute of a good tape and a few hundred for a ruined one; the
		 * top runs well past both, because a faithful maximum would be
		 * far too quiet to work as a glitch control. */
		prob = fminf(0.0018f * expf(9.3f * dop) / (float)FIELD_H, 0.25f);
	}
	set_f(e, "dropout_prob", prob);
	set_f(e, "dropout_min", (float)(DROPOUT_MIN_US / LINE_PERIOD_US));
	set_f(e, "dropout_span", (float)log(DROPOUT_MAX_US / DROPOUT_MIN_US));
	set_f(e, "dropout_mode", (float)f->dropout_mode);
	/* The crease is placed rather than rolled for: the slider is how many
	 * line periods of signal it takes out, so 0.5 is a whole line and
	 * anything past that wraps onto the rows below. */
	set_f(e, "damage_len", f->tape_damage * 2.0f);
	double dpos = (double)f->tape_damage_pos + f->damage_pos;
	set_f(e, "damage_line", floorf((float)(dpos - floor(dpos)) * (float)(FIELD_H - 1)) + (float)SIG_VBI_ROWS);

	/* Degauss. Squaring the envelope makes the tail die away smoothly. */
	const float dg = f->degauss * f->degauss * f->degauss_strength;
	set_f(e, "degauss_wob", dg);
	set_f(e, "degauss_conv", dg * 0.010f);
	set_f(e, "degauss_tint", dg);
	set_f(e, "degauss_phase", (float)f->degauss_phase);

	/* Power on/off envelope */
	struct power_gfx g = power_uniforms(f);
	set_f(e, "warmup", g.warmup);
	set_f(e, "collapse_v", g.collapse_v);
	set_f(e, "collapse_h", g.collapse_h);
	set_f(e, "flash", g.flash);
}

/* In signal-output mode the filter's own output IS the 910x264 packed raster,
 * so the reported size must say so - that is what keeps a Spout Filter (or
 * anything else grabbing the filter output) pixel-for-pixel. In normal mode,
 * report the target's size: once these callbacks exist, libobs consults them
 * for an enabled filter instead of recursing to the target by itself. */
static uint32_t ntsc_width(void *data)
{
	struct composite_tv *f = data;
	if (f->signal_format != 0)
		return SIG_W;
	obs_source_t *target = obs_filter_get_target(f->source);
	return target ? obs_source_get_base_width(target) : 0;
}

static uint32_t ntsc_height(void *data)
{
	struct composite_tv *f = data;
	if (f->signal_format != 0)
		return PACK_H;
	obs_source_t *target = obs_filter_get_target(f->source);
	return target ? obs_source_get_base_height(target) : 0;
}

/* Synthesize one field of the FM sound carrier into audio_tex. FM has memory,
 * so this is the one thing the shaders cannot do: the phase is the running
 * integral of the audio. One sincos per audio sample sets a rotor, and the
 * ~300 video samples that audio sample spans are pure rotations - the same
 * trick the FIR weights use. Runs on the graphics thread. */
static void ntsc_synth_carrier(struct composite_tv *f)
{
	if (!f->audio_tex) {
		f->audio_tex = gs_texture_create(SIG_W, SIG_H, GS_R32F, 1, NULL, GS_DYNAMIC);
		f->audio_buf = bzalloc(sizeof(float) * SIG_W * SIG_H);
		if (!f->audio_tex)
			return;
	}

	struct obs_audio_info oai;
	double rate = 48000.0;
	if (obs_get_audio_info(&oai) && oai.samples_per_sec > 0)
		rate = (double)oai.samples_per_sec;

	/* One field's worth of audio, with the fractional remainder carried
	 * over so 48000 / 59.94 = 800.8 does not drift. */
	double need_f = rate / 59.94 + f->aud_need;
	uint32_t need = (uint32_t)need_f;
	f->aud_need = need_f - (double)need;
	if (need > AUD_PULL_MAX)
		need = AUD_PULL_MAX;
	if (need == 0)
		need = 1;

	float pull[AUD_PULL_MAX];
	pthread_mutex_lock(&f->aud_mutex);
	uint64_t avail64 = f->aud_write - f->aud_read;
	uint32_t take = (avail64 < (uint64_t)need) ? (uint32_t)avail64 : need;
	for (uint32_t i = 0; i < take; i++)
		pull[i] = f->aud_ring[(f->aud_read + i) & (AUD_RING_LEN - 1)];
	f->aud_read += take;
	pthread_mutex_unlock(&f->aud_mutex);
	/* Underrun (or no source at all): the transmitter idles at rest
	 * frequency, it does not stop transmitting. */
	for (uint32_t i = take; i < need; i++)
		pull[i] = 0.0f;

	/* 75 us pre-emphasis: the standard high-frequency lift, y = x + tau*x'. */
	const float pre = (float)(AUDIO_PREEMPH_TAU_S * rate);
	for (uint32_t i = 0; i < need; i++) {
		float x = pull[i];
		float y = x + pre * (x - f->pre_x1);
		f->pre_x1 = x;
		pull[i] = y < -2.0f ? -2.0f : (y > 2.0f ? 2.0f : y);
	}

	const uint32_t total = SIG_W * SIG_H;
	float *dst = f->audio_buf;
	uint32_t idx = 0;
	for (uint32_t j = 0; j < need; j++) {
		double freq = AUDIO_CARRIER_HZ + AUDIO_DEVIATION_HZ * (double)pull[j];
		double dphi = 2.0 * NS_PI * freq / SAMPLE_RATE_HZ;
		uint32_t end = (uint32_t)((uint64_t)total * (uint64_t)(j + 1) / (uint64_t)need);
		float c = (float)cos(f->fm_phase);
		float s = (float)sin(f->fm_phase);
		float rc = (float)cos(dphi);
		float rs = (float)sin(dphi);
		uint32_t count = end - idx;
		for (; idx < end; idx++) {
			dst[idx] = s;
			float t = c * rc - s * rs;
			s = s * rc + c * rs;
			c = t;
		}
		f->fm_phase = fmod(f->fm_phase + dphi * (double)count, 2.0 * NS_PI);
	}

	gs_texture_set_image(f->audio_tex, (const uint8_t *)dst, SIG_W * sizeof(float), false);
	f->aud_field_count = need;
}

static void ntsc_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct composite_tv *f = data;
	obs_source_t *target = obs_filter_get_target(f->source);

	if (!f->effect || !target) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	uint32_t cx = obs_source_get_base_width(target);
	uint32_t cy = obs_source_get_base_height(target);
	if (cx == 0 || cy == 0) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	/* advance time / field state */
	uint64_t now = os_gettime_ns();
	float dt = f->last_time ? (float)((double)(now - f->last_time) / 1.0e9) : 0.016f;
	if (dt < 0.0f || dt > 0.5f)
		dt = 0.016f;
	f->last_time = now;
	f->field_counter++;
	f->chroma_phase_acc = fmod(f->chroma_phase_acc + (double)f->chroma_drift * dt, 2.0 * NS_PI);
	power_advance(f, dt);

	/* glitch animation: decay the two bursts, advance the roll and the beat.
	 * A playback burst runs for exactly as long as the crease takes to cross
	 * the picture once, because that sweep is what the burst is. */
	const float sweep = f->sweep_sec > 0.05f ? f->sweep_sec : 2.0f;
	if (f->burst_rx > 0.0f) {
		float len = f->burst_len > 0.05f ? f->burst_len : 0.4f;
		f->burst_rx -= dt / len;
		if (f->burst_rx < 0.0f)
			f->burst_rx = 0.0f;
	}
	if (f->burst_pb > 0.0f) {
		f->burst_pb -= dt / sweep;
		if (f->burst_pb < 0.0f)
			f->burst_pb = 0.0f;
	}
	/* The burst kicks the vertical hold hard so the picture visibly tumbles
	 * (several screens over the burst) before it re-locks. */
	/* One roll cycle is the active picture plus the blanking interval, which
	 * is what the shader wraps over - the two have to agree or the wrap would
	 * jump the picture. */
	const double frame_lines = (double)f->scan_lines;
	const double cycle = frame_lines + (double)bar_lines(f);
	float roll_speed = f->v_roll_speed + f->burst_rx * 6.0f;
	if (roll_speed != 0.0f) {
		f->v_roll_pos = fmod(f->v_roll_pos + (double)roll_speed * cycle * dt, cycle);
	} else if (f->v_roll_pos != 0.0) {
		/* Vertical hold re-locks: slide back to the framed position by the
		 * shortest way round, then snap once we are within half a line. */
		const double half = cycle * 0.5;
		double p = fmod(f->v_roll_pos, cycle);
		if (p > half)
			p -= cycle;
		else if (p < -half)
			p += cycle;
		p *= exp(-(double)dt / 0.30); /* ~300 ms settle, visible glide back */
		f->v_roll_pos = (fabs(p) < 0.5) ? 0.0 : p;
	}
	/* A crease is at a fixed place on the tape, not on the screen. Whether it
	 * lands on the same lines twice depends on the tape keeping step with the
	 * drum, and it does not quite, so the damaged band crawls through the
	 * picture - which is what a creased tape actually looks like. A playback
	 * burst sends it across at exactly one screen per sweep, which is what
	 * that setting means, and zeroing everything hands the band back to its
	 * own slider. */
	float damage_speed = f->damage_speed + (f->burst_pb > 0.0f ? 1.0f / sweep : 0.0f);
	if (damage_speed != 0.0f) {
		double p = f->damage_pos + (double)damage_speed * dt;
		f->damage_pos = p - floor(p);
	} else {
		f->damage_pos = 0.0;
	}

	f->beat_phase = fmod(f->beat_phase + 2.0 * NS_PI * 0.7 * dt, 2.0 * NS_PI);
	/* Tension wobble: slow and incommensurate (0.37 Hz and 0.11 Hz). */
	f->flag_phase1 = fmod(f->flag_phase1 + 2.0 * NS_PI * 0.37 * dt, 2.0 * NS_PI);
	f->flag_phase2 = fmod(f->flag_phase2 + 2.0 * NS_PI * 0.11 * dt, 2.0 * NS_PI);
	/* The line oscillator's free-run error wanders even more slowly - a
	 * warm resistor here, a cold capacitor there. */
	f->afc_phase1 = fmod(f->afc_phase1 + 2.0 * NS_PI * 0.043 * dt, 2.0 * NS_PI);
	f->afc_phase2 = fmod(f->afc_phase2 + 2.0 * NS_PI * 0.013 * dt, 2.0 * NS_PI);
	/* Propagation fading, one oscillator per mechanism: the duct breathes
	 * over tens of seconds, two-ray interference beats over seconds, and
	 * scintillation flutters around a second. */
	f->fade_phase1 = fmod(f->fade_phase1 + 2.0 * NS_PI * 0.027 * dt, 2.0 * NS_PI);
	f->fade_phase2 = fmod(f->fade_phase2 + 2.0 * NS_PI * 0.17 * dt, 2.0 * NS_PI);
	f->fade_phase3 = fmod(f->fade_phase3 + 2.0 * NS_PI * 1.3 * dt, 2.0 * NS_PI);
	f->fade_phase4 = fmod(f->fade_phase4 + 2.0 * NS_PI * 0.47 * dt, 2.0 * NS_PI);
	f->fade_phase5 = fmod(f->fade_phase5 + 2.0 * NS_PI * 0.031 * dt, 2.0 * NS_PI);

	/* degauss: decaying AC through the coil */
	if (f->degauss > 0.0f) {
		float dlen = f->degauss_len > 0.05f ? f->degauss_len : 1.2f;
		f->degauss -= dt / dlen;
		if (f->degauss < 0.0f)
			f->degauss = 0.0f;
		f->degauss_phase = fmod(f->degauss_phase + 2.0 * NS_PI * 9.0 * dt, 2.0 * NS_PI);
	}

	gs_effect_t *e = f->effect;

	/* 1) capture the filter input into a texture */
	ensure_tr(&f->input, GS_RGBA);
	if (!target_begin(f->input, cx, cy, true)) {
		obs_source_skip_video_filter(f->source);
		return;
	}
	obs_source_video_render(target);
	target_end(f->input);
	gs_texture_t *input_tex = gs_texrender_get_texture(f->input);

	/* The two signal rasters run at full float: half precision only carries
	 * ~11 significant bits, which the 16-bit signal export would expose. */
	ensure_tr(&f->composite, GS_RGBA32F);
	ensure_tr(&f->detector, GS_RGBA32F);
	ensure_tr(&f->field[0], GS_RGBA16F);
	ensure_tr(&f->field[1], GS_RGBA16F);
	ensure_tr(&f->display[0], GS_RGBA);
	ensure_tr(&f->display[1], GS_RGBA);

	/* 2) Encode -> 3) Detect -> 4) Decode. run_pass() applies the parameters
	 * itself, which is what keeps the libobs clear-on-technique-end quirk
	 * from being a rule anyone has to remember. */
	/* FM sound carrier, when switched on. Bound whether or not it is: the
	 * sampler is in the compiled shader either way, and an unset texture
	 * makes D3D11 silently skip the draw. */
	if (f->audio_gain > 0.0f)
		ntsc_synth_carrier(f);
	set_tex(e, "audio_tex", f->audio_tex ? f->audio_tex : input_tex);

	gs_texture_t *comp = run_pass(f, e, f->composite, SIG_W, SIG_H, "Encode", input_tex, cx, cy);
	gs_texture_t *det = run_pass(f, e, f->detector, SIG_W, SIG_H, "Detect", comp, cx, cy);

	/* The packed raster is produced every frame, whatever the display mode,
	 * and published for the companion signal source: that is what lets one
	 * filter instance show the CRT picture and feed an outside decoder at
	 * the same time, with a single set of settings. The pass is 910x264 -
	 * a rounding error next to the display pass. */
	ensure_tr(&f->pack, GS_RGBA);
	gs_texture_t *tap = (f->signal_tap == 1 && det) ? det : comp;
	gs_texture_t *packed = tap ? run_pass(f, e, f->pack, SIG_W, PACK_H, "Pack", tap, cx, cy) : NULL;
	if (packed)
		ctv_signal_publish(f, f->source, packed);

	/* Signal output mode: the filter's own output becomes the raster and
	 * the TV side - Decode, Display, power envelope - is skipped entirely:
	 * a deck does not care whether the set is even on. */
	if (f->signal_format != 0) {
		if (packed) {
			gs_effect_t *blit = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			set_tex(blit, "image", packed);
			while (gs_effect_loop(blit, "Draw"))
				gs_draw_sprite(packed, 0, SIG_W, PACK_H);
		}
		return;
	}

	/* Receiver tracking: measure sync and burst per row, then run the AFC
	 * over the measurements. Two 263x1 passes - the cost is a rounding
	 * error, and everything the decoder and the tube know about timing and
	 * colour lock now comes from here rather than from the encoder. */
	ensure_tr(&f->track, GS_RGBA32F);
	ensure_tr(&f->flywheel[0], GS_RGBA32F);
	ensure_tr(&f->flywheel[1], GS_RGBA32F);
	gs_texture_t *trk = det ? run_pass(f, e, f->track, SIG_H, 1, "Track", det, cx, cy) : NULL;
	/* The line oscillator's state continues across fields: last field's
	 * output seeds this field's recurrence, ping-pong style. On the very
	 * first frame there is no history and the seed is whatever gets read
	 * from the fallback texture - the loop pulls itself in within a field,
	 * exactly like a set warming up. */
	gs_texture_t *fly_prev = gs_texrender_get_texture(f->flywheel[1 - f->fly_idx]);
	set_tex(e, "flywheel_prev", fly_prev ? fly_prev : input_tex);
	gs_texture_t *fly = trk ? run_pass(f, e, f->flywheel[f->fly_idx], SIG_H, 1, "Flywheel", trk, cx, cy) : NULL;
	/* Only alternate when something was actually written, so a skipped
	 * frame can never point the seed at a buffer that was left stale. */
	if (fly)
		f->fly_idx ^= 1;
	set_tex(e, "track_tex", trk ? trk : input_tex);
	set_tex(e, "flywheel_tex", fly ? fly : input_tex);

	/* TEMPORARY diagnosis: once a second, read three rows of both tracking
	 * textures back and log them. Remove once the shift is understood. */
	uint64_t dbg_now = os_gettime_ns();
	if (trk && fly && dbg_now - f->dbg_last > 1000000000ULL) {
		f->dbg_last = dbg_now;
		if (!f->dbg_stage)
			f->dbg_stage = gs_stagesurface_create(SIG_H, 1, GS_RGBA32F);
		if (f->dbg_stage) {
			uint8_t *data;
			uint32_t linesize;
			gs_stage_texture(f->dbg_stage, trk);
			if (gs_stagesurface_map(f->dbg_stage, &data, &linesize)) {
				const float *px = (const float *)data;
				obs_log(LOG_INFO,
					"[dbg trk] r30=(%.3f %.3f %.3f %.3f) r100=(%.3f %.3f %.3f %.3f) r200=(%.3f %.3f %.3f %.3f)",
					px[30 * 4 + 0], px[30 * 4 + 1], px[30 * 4 + 2], px[30 * 4 + 3],
					px[100 * 4 + 0], px[100 * 4 + 1], px[100 * 4 + 2], px[100 * 4 + 3],
					px[200 * 4 + 0], px[200 * 4 + 1], px[200 * 4 + 2], px[200 * 4 + 3]);
				gs_stagesurface_unmap(f->dbg_stage);
			}
			gs_stage_texture(f->dbg_stage, fly);
			if (gs_stagesurface_map(f->dbg_stage, &data, &linesize)) {
				const float *px = (const float *)data;
				obs_log(LOG_INFO,
					"[dbg fly] r30=(%.3f %.3f %.3f %.3f) r100=(%.3f %.3f %.3f %.3f) r262=(%.3f %.3f %.3f %.3f)",
					px[30 * 4 + 0], px[30 * 4 + 1], px[30 * 4 + 2], px[30 * 4 + 3],
					px[100 * 4 + 0], px[100 * 4 + 1], px[100 * 4 + 2], px[100 * 4 + 3],
					px[262 * 4 + 0], px[262 * 4 + 1], px[262 * 4 + 2], px[262 * 4 + 3]);
				gs_stagesurface_unmap(f->dbg_stage);
			}
		}
	}

	gs_texture_t *field_cur =
		run_pass(f, e, f->field[f->field_idx], FIELD_W, FIELD_H, "Decode", det, cx, cy);

	/* 5) Display: interlaced CRT presentation into the display buffer */
	gs_texture_t *field_prev = gs_texrender_get_texture(f->field[1 - f->field_idx]);
	if (!field_prev)
		field_prev = field_cur;
	gs_texture_t *disp_prev = gs_texrender_get_texture(f->display[1 - f->display_idx]);
	if (!disp_prev)
		disp_prev = field_cur;

	set_tex(e, "field_prev", field_prev);
	set_tex(e, "display_prev", disp_prev);
	/* The debug scope traces the detector output, which the display pass has
	 * no other reason to see. Bound whether or not it is switched on: the
	 * sampler is in the compiled shader either way. */
	set_tex(e, "scope_src", det ? det : field_cur);
	gs_texture_t *display_cur =
		run_pass(f, e, f->display[f->display_idx], cx, cy, "Display", field_cur, cx, cy);

	/* 6) blit the final frame to the actual filter output */
	if (display_cur) {
		gs_effect_t *blit = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		set_tex(blit, "image", display_cur);
		while (gs_effect_loop(blit, "Draw"))
			gs_draw_sprite(display_cur, 0, cx, cy);
	}

	/* swap ping-pong buffers for the next frame */
	f->field_idx ^= 1;
	f->display_idx ^= 1;
}

/* ---- properties ------------------------------------------------------ */

/* Called when the properties object is destroyed - that is, when the filter
 * dialog closes. The inspection zoom is a tool, not a look, so wind it back
 * rather than leave a magnified picture going out live. A weak reference keeps
 * this safe if the filter itself was removed while the dialog was open. */
/* List every source that produces audio, for the FM carrier picker. */
static bool add_audio_source(void *param, obs_source_t *src)
{
	obs_property_t *prop = param;
	if (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) {
		const char *name = obs_source_get_name(src);
		if (name && *name)
			obs_property_list_add_string(prop, name, name);
	}
	return true;
}

static void ntsc_props_destroyed(void *param)
{
	obs_weak_source_t *weak = param;
	obs_source_t *src = obs_weak_source_get_source(weak);

	if (src) {
		obs_data_t *s = obs_source_get_settings(src);
		if (obs_data_get_double(s, "preview_zoom") != 1.0) {
			obs_data_set_double(s, "preview_zoom", 1.0);
			obs_data_set_double(s, "zoom_x", 0.5);
			obs_data_set_double(s, "zoom_y", 0.5);
			obs_source_update(src, s);
		}
		obs_data_release(s);
		obs_source_release(src);
	}
	obs_weak_source_release(weak);
}

/* Every default lives here exactly once: ntsc_defaults() sets them and
 * ntsc_get_properties() prints them into the "Default: x" tooltips, so the two
 * cannot disagree. */
#define DEF_POWER true
#define DEF_FIELD_STRENGTH 1.00
#define DEF_NOISE_FLOOR 0.04
#define DEF_YC_MODE 1
#define DEF_CHROMA_BAND_I 1.3
#define DEF_CHROMA_BAND_Q 0.4
#define DEF_ENC_CHROMA_GAIN 1.0
#define DEF_SCREEN_ASPECT 0
#define DEF_ASPECT_MODE 0
#define DEF_AGC_LEVEL 0.62
#define DEF_AGC_JITTER 0.06
#define DEF_H_HOLD 0.0
#define DEF_IF_BANDWIDTH 5.0
#define DEF_LUMA_BANDWIDTH 4.2
#define DEF_PEAKING 0.0
#define DEF_CHROMA_GAIN 0.70
#define DEF_COLOR_KILLER_MODE 0
#define DEF_CHROMA_DRIFT 0.6
#define DEF_CONTRAST 1.15
#define DEF_BRIGHTNESS -0.02
#define DEF_SCAN_LINES ACTIVE_LINES
#define DEF_LINE_SKIP false
#define DEF_GAP_LEVEL 0.15
#define DEF_INTERLACE true
#define DEF_PERSISTENCE 0.22
#define DEF_SPOT_V 0.45
#define DEF_SPOT_H 0.85
#define DEF_SCANLINE 0.35
#define DEF_PREVIEW_ZOOM 1.0
#define DEF_SCOPE_ON false
#define DEF_SCOPE_LINE 240.0
#define DEF_SIGNAL_FORMAT 0
#define DEF_SIGNAL_TAP 0
/* Off by default: the carrier is real signal content - with no 4.5 MHz trap
 * in the decoder it shows as fine dots - so it must never appear unasked. */
#define DEF_AUDIO_CARRIER_GAIN 0.0
#define DEF_ZOOM_X 0.5
#define DEF_ZOOM_Y 0.5
#define DEF_MASK_TYPE 0
#define DEF_MASK_STRENGTH 0.6
#define DEF_MASK_WIDTH 0.62
#define DEF_MASK_PITCH 450.0
#define DEF_WIRE_WIDTH 0.01
#define DEF_CURVATURE 0.025
#define DEF_VIGNETTE 0.30
#define DEF_OVERSCAN 0.0
#define DEF_DEGAUSS_LEN 1.2
#define DEF_DEGAUSS_STRENGTH 0.7
#define DEF_GLITCH_ENABLE false
#define DEF_PLAYBACK_ENABLE false
#define DEF_SWEEP_SEC 2.0
#define DEF_GHOST_GAIN 0.0
#define DEF_GHOST_DELAY 24.0
#define DEF_V_ROLL_SPEED 0.0
#define DEF_H_JITTER 0.0
#define DEF_FADING 0.0
#define DEF_FLAGGING 0.0
#define DEF_FLAG_WAVE 0.0
#define DEF_HEAD_SWITCH 0.0
#define DEF_DROPOUT 0.0
#define DEF_DROPOUT_MODE 0
#define DEF_TAPE_DAMAGE 0.0
#define DEF_TAPE_DAMAGE_POS 0.5
#define DEF_TAPE_DAMAGE_DRIFT 0.0
#define DEF_BEAT_GAIN 0.0
#define DEF_BEAT_FREQ 2.5
#define DEF_BURST_LEN 0.4

static obs_properties_t *ntsc_get_properties(void *data)
{
	struct composite_tv *f = data;
	obs_properties_t *p = obs_properties_create();

	if (f && f->source)
		obs_properties_set_param(p, obs_source_get_weak_source(f->source), ntsc_props_destroyed);

	obs_properties_add_text(p, "hint", obs_module_text("CompositeTV.Hint"), OBS_TEXT_INFO);
	ctv_add_bool(p, "power", obs_module_text("CompositeTV.Power"), DEF_POWER);
	ctv_add_float_slider(p, "field_strength", obs_module_text("CompositeTV.FieldStrength"), 0.0, 1.0, 0.01,
			     DEF_FIELD_STRENGTH);
	ctv_add_float_slider(p, "noise_floor", obs_module_text("CompositeTV.NoiseFloor"), 0.0, 0.30, 0.01,
			     DEF_NOISE_FLOOR);

	obs_property_t *yc = obs_properties_add_list(p, "yc_mode", obs_module_text("CompositeTV.YCMode"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(yc, obs_module_text("CompositeTV.YCMode.Bandpass"), 0);
	obs_property_list_add_int(yc, obs_module_text("CompositeTV.YCMode.Comb2"), 1);
	obs_property_list_add_int(yc, obs_module_text("CompositeTV.YCMode.Comb3"), 2);
	ctv_set_list_default_tip(yc, DEF_YC_MODE);

	ctv_add_float_slider(p, "chroma_band_i", obs_module_text("CompositeTV.ChromaBandI"), 0.4, 2.0, 0.1,
			     DEF_CHROMA_BAND_I);
	ctv_add_float_slider(p, "chroma_band_q", obs_module_text("CompositeTV.ChromaBandQ"), 0.2, 1.3, 0.1,
			     DEF_CHROMA_BAND_Q);
	ctv_add_float_slider(p, "enc_chroma_gain", obs_module_text("CompositeTV.EncChromaGain"), 0.0, 1.5, 0.05,
			     DEF_ENC_CHROMA_GAIN);

	obs_property_t *scr = obs_properties_add_list(p, "screen_aspect", obs_module_text("CompositeTV.ScreenAspect"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(scr, obs_module_text("CompositeTV.ScreenAspect.Source"), 0);
	obs_property_list_add_int(scr, obs_module_text("CompositeTV.ScreenAspect.43"), 1);
	obs_property_list_add_int(scr, obs_module_text("CompositeTV.ScreenAspect.169"), 2);
	ctv_set_list_default_tip(scr, DEF_SCREEN_ASPECT);

	obs_property_t *asp = obs_properties_add_list(p, "aspect_mode", obs_module_text("CompositeTV.AspectMode"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(asp, obs_module_text("CompositeTV.AspectMode.Letterbox"), 0);
	obs_property_list_add_int(asp, obs_module_text("CompositeTV.AspectMode.Stretch"), 1);
	ctv_set_list_default_tip(asp, DEF_ASPECT_MODE);

	ctv_add_float_slider(p, "agc_level", obs_module_text("CompositeTV.AgcLevel"), 0.20, 0.95, 0.01, DEF_AGC_LEVEL);
	ctv_add_float_slider(p, "agc_jitter", obs_module_text("CompositeTV.AgcJitter"), 0.0, 0.40, 0.01,
			     DEF_AGC_JITTER);
	obs_property_t *hh =
		ctv_add_float_slider(p, "h_hold", obs_module_text("CompositeTV.HHold"), 0.0, 1.0, 0.01, DEF_H_HOLD);
	ctv_add_tip_note(hh, obs_module_text("CompositeTV.HHold.Tip"));
	ctv_add_float_slider(p, "if_bandwidth", obs_module_text("CompositeTV.IfBandwidth"), 2.0, 7.0, 0.1,
			     DEF_IF_BANDWIDTH);

	ctv_add_float_slider(p, "luma_bandwidth", obs_module_text("CompositeTV.LumaBandwidth"), 1.0, 4.2, 0.1,
			     DEF_LUMA_BANDWIDTH);
	ctv_add_float_slider(p, "chroma_gain", obs_module_text("CompositeTV.ChromaGain"), 0.0, 2.0, 0.01,
			     DEF_CHROMA_GAIN);
	obs_property_t *ck = obs_properties_add_list(p, "color_killer_mode", obs_module_text("CompositeTV.ColorKiller"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(ck, obs_module_text("CompositeTV.ColorKiller.Auto"), 0);
	obs_property_list_add_int(ck, obs_module_text("CompositeTV.ColorKiller.Off"), 1);
	obs_property_list_add_int(ck, obs_module_text("CompositeTV.ColorKiller.Force"), 2);
	ctv_set_list_default_tip(ck, DEF_COLOR_KILLER_MODE);
	ctv_add_float_slider(p, "chroma_drift", obs_module_text("CompositeTV.ChromaDrift"), 0.0, 3.0, 0.05,
			     DEF_CHROMA_DRIFT);
	ctv_add_float_slider(p, "contrast", obs_module_text("CompositeTV.Contrast"), 0.3, 2.0, 0.01, DEF_CONTRAST);
	ctv_add_float_slider(p, "brightness", obs_module_text("CompositeTV.Brightness"), -0.3, 0.3, 0.01,
			     DEF_BRIGHTNESS);

	obs_property_t *sl = obs_properties_add_list(p, "scan_lines", obs_module_text("CompositeTV.ScanLines"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(sl, obs_module_text("CompositeTV.ScanLines.486"), 486);
	obs_property_list_add_int(sl, obs_module_text("CompositeTV.ScanLines.243"), 243);
	obs_property_list_add_int(sl, obs_module_text("CompositeTV.ScanLines.162"), 162);
	ctv_set_list_default_tip(sl, DEF_SCAN_LINES);

	ctv_add_bool(p, "line_skip", obs_module_text("CompositeTV.LineSkip"), DEF_LINE_SKIP);
	ctv_add_float_slider(p, "gap_level", obs_module_text("CompositeTV.GapLevel"), 0.0, 1.0, 0.05, DEF_GAP_LEVEL);

	ctv_add_bool(p, "interlace", obs_module_text("CompositeTV.Interlace"), DEF_INTERLACE);
	ctv_add_float_slider(p, "persistence", obs_module_text("CompositeTV.Persistence"), 0.0, 0.75, 0.01,
			     DEF_PERSISTENCE);
	ctv_add_float_slider(p, "spot_v", obs_module_text("CompositeTV.SpotV"), 0.30, 1.60, 0.01, DEF_SPOT_V);
	ctv_add_float_slider(p, "spot_h", obs_module_text("CompositeTV.SpotH"), 0.30, 2.50, 0.01, DEF_SPOT_H);
	ctv_add_float_slider(p, "scanline", obs_module_text("CompositeTV.Scanline"), 0.0, 0.60, 0.01, DEF_SCANLINE);

	/* Inspection aid, kept next to the controls it exists to make judgeable.
	 * It magnifies the output, so it winds back to 1.0 when the dialog is
	 * closed - the hint below says so. */
	ctv_add_float_slider(p, "preview_zoom", obs_module_text("CompositeTV.PreviewZoom"), 1.0, 8.0, 0.1,
			     DEF_PREVIEW_ZOOM);
	obs_properties_add_text(p, "zoom_hint", obs_module_text("CompositeTV.ZoomHint"), OBS_TEXT_INFO);
	ctv_add_float_slider(p, "zoom_x", obs_module_text("CompositeTV.ZoomX"), 0.0, 1.0, 0.01, DEF_ZOOM_X);
	ctv_add_float_slider(p, "zoom_y", obs_module_text("CompositeTV.ZoomY"), 0.0, 1.0, 0.01, DEF_ZOOM_Y);

	/* tube-face colour structure */
	obs_property_t *mt = obs_properties_add_list(p, "mask_type", obs_module_text("CompositeTV.MaskType"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mt, obs_module_text("CompositeTV.MaskType.Off"), 0);
	obs_property_list_add_int(mt, obs_module_text("CompositeTV.MaskType.Slot"), 1);
	obs_property_list_add_int(mt, obs_module_text("CompositeTV.MaskType.Dot"), 2);
	obs_property_list_add_int(mt, obs_module_text("CompositeTV.MaskType.Grille"), 3);
	ctv_set_list_default_tip(mt, DEF_MASK_TYPE);
	ctv_add_float_slider(p, "mask_strength", obs_module_text("CompositeTV.MaskStrength"), 0.0, 1.0, 0.01,
			     DEF_MASK_STRENGTH);
	ctv_add_float_slider(p, "mask_width", obs_module_text("CompositeTV.MaskWidth"), 0.20, 1.00, 0.01,
			     DEF_MASK_WIDTH);
	ctv_add_float_slider(p, "mask_pitch", obs_module_text("CompositeTV.MaskPitch"), 150.0, 900.0, 10.0,
			     DEF_MASK_PITCH);
	ctv_add_float_slider(p, "wire_width", obs_module_text("CompositeTV.WireWidth"), 0.01, 4.0, 0.01,
			     DEF_WIRE_WIDTH);

	ctv_add_float_slider(p, "curvature", obs_module_text("CompositeTV.Curvature"), 0.0, 0.12, 0.005, DEF_CURVATURE);
	ctv_add_float_slider(p, "vignette", obs_module_text("CompositeTV.Vignette"), 0.0, 0.80, 0.01, DEF_VIGNETTE);
	ctv_add_float_slider(p, "overscan", obs_module_text("CompositeTV.Overscan"), 0.0, 0.08, 0.005, DEF_OVERSCAN);
	ctv_add_float_slider(p, "degauss_len", obs_module_text("CompositeTV.DegaussLen"), 0.3, 3.0, 0.1,
			     DEF_DEGAUSS_LEN);
	ctv_add_float_slider(p, "degauss_strength", obs_module_text("CompositeTV.DegaussStrength"), 0.0, 1.0, 0.01,
			     DEF_DEGAUSS_STRENGTH);

	/* --- faults, in two collapsible halves, both off by default. They are
	 * split by where they come from, because that is what decides whether
	 * they belong together: the aerial and the tape fail at their own times
	 * and each half is fired on its own. --- */
	obs_properties_t *g = obs_properties_create();
	ctv_add_float_slider(g, "ghost_gain", obs_module_text("CompositeTV.GhostGain"), 0.0, 0.8, 0.01, DEF_GHOST_GAIN);
	ctv_add_float_slider(g, "ghost_delay", obs_module_text("CompositeTV.GhostDelay"), -60.0, 120.0, 1.0,
			     DEF_GHOST_DELAY);
	ctv_add_float_slider(g, "v_roll_speed", obs_module_text("CompositeTV.VRollSpeed"), -2.0, 2.0, 0.01,
			     DEF_V_ROLL_SPEED);
	ctv_add_float_slider(g, "h_jitter", obs_module_text("CompositeTV.HJitter"), 0.0, 1.0, 0.01, DEF_H_JITTER);
	obs_property_t *fd =
		ctv_add_float_slider(g, "fading", obs_module_text("CompositeTV.Fading"), 0.0, 1.0, 0.01, DEF_FADING);
	ctv_add_tip_note(fd, obs_module_text("CompositeTV.Fading.Tip"));
	ctv_add_float_slider(g, "beat_gain", obs_module_text("CompositeTV.BeatGain"), 0.0, 0.5, 0.01, DEF_BEAT_GAIN);
	ctv_add_float_slider(g, "beat_freq", obs_module_text("CompositeTV.BeatFreq"), 0.1, 5.0, 0.01, DEF_BEAT_FREQ);
	ctv_add_float_slider(g, "burst_len", obs_module_text("CompositeTV.BurstLen"), 0.1, 2.0, 0.05, DEF_BURST_LEN);
	obs_properties_add_group(p, "glitch_enable", obs_module_text("CompositeTV.Glitch"), OBS_GROUP_CHECKABLE, g);

	obs_properties_t *tp = obs_properties_create();
	obs_property_t *pk = ctv_add_float_slider(tp, "peaking", obs_module_text("CompositeTV.Peaking"), 0.0, 1.0, 0.01,
						  DEF_PEAKING);
	ctv_add_tip_note(pk, obs_module_text("CompositeTV.Peaking.Tip"));
	ctv_add_float_slider(tp, "flagging", obs_module_text("CompositeTV.Flagging"), -1.0, 1.0, 0.01, DEF_FLAGGING);
	ctv_add_float_slider(tp, "flag_wave", obs_module_text("CompositeTV.FlagWave"), 0.0, 1.0, 0.01, DEF_FLAG_WAVE);
	ctv_add_float_slider(tp, "head_switch", obs_module_text("CompositeTV.HeadSwitch"), 0.0, 1.0, 0.01,
			     DEF_HEAD_SWITCH);
	ctv_add_float_slider(tp, "dropout", obs_module_text("CompositeTV.Dropout"), 0.0, 1.0, 0.01, DEF_DROPOUT);
	obs_property_t *dm = obs_properties_add_list(tp, "dropout_mode", obs_module_text("CompositeTV.DropoutMode"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(dm, obs_module_text("CompositeTV.DropoutMode.None"), 0);
	obs_property_list_add_int(dm, obs_module_text("CompositeTV.DropoutMode.Grey"), 1);
	obs_property_list_add_int(dm, obs_module_text("CompositeTV.DropoutMode.Delay"), 2);
	ctv_set_list_default_tip(dm, DEF_DROPOUT_MODE);
	ctv_add_float_slider(tp, "tape_damage", obs_module_text("CompositeTV.TapeDamage"), 0.0, 1.0, 0.01,
			     DEF_TAPE_DAMAGE);
	ctv_add_float_slider(tp, "tape_damage_pos", obs_module_text("CompositeTV.TapeDamagePos"), 0.0, 1.0, 0.01,
			     DEF_TAPE_DAMAGE_POS);
	ctv_add_float_slider(tp, "tape_damage_drift", obs_module_text("CompositeTV.TapeDamageDrift"), -2.0, 2.0, 0.01,
			     DEF_TAPE_DAMAGE_DRIFT);
	obs_property_t *sw = ctv_add_float_slider(tp, "sweep_sec", obs_module_text("CompositeTV.SweepSec"), 0.2, 10.0,
						  0.1, DEF_SWEEP_SEC);
	ctv_add_tip_note(sw, obs_module_text("CompositeTV.SweepSec.Tip"));
	obs_properties_add_group(p, "playback_enable", obs_module_text("CompositeTV.Playback"), OBS_GROUP_CHECKABLE, tp);

	/* --- signal output, for feeding an outside decoder --- */
	obs_properties_t *so = obs_properties_create();
	obs_property_t *sfm = obs_properties_add_list(so, "signal_format", obs_module_text("CompositeTV.SignalFormat"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(sfm, obs_module_text("CompositeTV.SignalFormat.Off"), 0);
	obs_property_list_add_int(sfm, obs_module_text("CompositeTV.SignalFormat.Pack16"), 1);
	obs_property_list_add_int(sfm, obs_module_text("CompositeTV.SignalFormat.Gray8"), 2);
	ctv_set_list_default_tip(sfm, DEF_SIGNAL_FORMAT);
	obs_property_t *stp = obs_properties_add_list(so, "signal_tap", obs_module_text("CompositeTV.SignalTap"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(stp, obs_module_text("CompositeTV.SignalTap.Deck"), 0);
	obs_property_list_add_int(stp, obs_module_text("CompositeTV.SignalTap.Det"), 1);
	ctv_set_list_default_tip(stp, DEF_SIGNAL_TAP);
	obs_property_t *cas = obs_properties_add_list(so, "carrier_audio_source",
						      obs_module_text("CompositeTV.CarrierAudioSource"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(cas, obs_module_text("CompositeTV.CarrierAudio.None"), "");
	obs_enum_sources(add_audio_source, cas);
	obs_property_t *acg = ctv_add_float_slider(so, "audio_carrier_gain",
						   obs_module_text("CompositeTV.AudioCarrierGain"), 0.0, 0.3, 0.01,
						   DEF_AUDIO_CARRIER_GAIN);
	ctv_add_tip_note(acg, obs_module_text("CompositeTV.AudioCarrierGain.Tip"));
	obs_properties_add_text(so, "signal_hint", obs_module_text("CompositeTV.Signal.Hint"), OBS_TEXT_INFO);
	obs_properties_add_group(p, "signal_group", obs_module_text("CompositeTV.Signal"), OBS_GROUP_NORMAL, so);

	/* --- debug, last so it stays out of the way --- */
	obs_property_t *sc = ctv_add_bool(p, "scope_on", obs_module_text("CompositeTV.Scope"), DEF_SCOPE_ON);
	ctv_add_tip_note(sc, obs_module_text("CompositeTV.Scope.Tip"));
	ctv_add_float_slider(p, "scope_line", obs_module_text("CompositeTV.ScopeLine"), 0.0, (double)(ACTIVE_LINES - 1),
			     1.0, DEF_SCOPE_LINE);

	return p;
}

static void ntsc_defaults(obs_data_t *s)
{
	obs_data_set_default_bool(s, "power", DEF_POWER);
	obs_data_set_default_double(s, "field_strength", DEF_FIELD_STRENGTH);
	obs_data_set_default_double(s, "noise_floor", DEF_NOISE_FLOOR);
	obs_data_set_default_int(s, "yc_mode", DEF_YC_MODE);
	obs_data_set_default_double(s, "chroma_band_i", DEF_CHROMA_BAND_I);
	obs_data_set_default_double(s, "chroma_band_q", DEF_CHROMA_BAND_Q);
	obs_data_set_default_double(s, "enc_chroma_gain", DEF_ENC_CHROMA_GAIN);
	obs_data_set_default_int(s, "aspect_mode", DEF_ASPECT_MODE);
	obs_data_set_default_int(s, "screen_aspect", DEF_SCREEN_ASPECT);
	obs_data_set_default_double(s, "agc_level", DEF_AGC_LEVEL);
	obs_data_set_default_double(s, "agc_jitter", DEF_AGC_JITTER);
	obs_data_set_default_double(s, "h_hold", DEF_H_HOLD);
	obs_data_set_default_double(s, "if_bandwidth", DEF_IF_BANDWIDTH);
	obs_data_set_default_double(s, "luma_bandwidth", DEF_LUMA_BANDWIDTH);
	obs_data_set_default_double(s, "chroma_gain", DEF_CHROMA_GAIN);
	obs_data_set_default_int(s, "color_killer_mode", DEF_COLOR_KILLER_MODE);
	obs_data_set_default_double(s, "chroma_drift", DEF_CHROMA_DRIFT);
	obs_data_set_default_double(s, "contrast", DEF_CONTRAST);
	obs_data_set_default_double(s, "brightness", DEF_BRIGHTNESS);
	obs_data_set_default_int(s, "scan_lines", DEF_SCAN_LINES);
	obs_data_set_default_bool(s, "line_skip", DEF_LINE_SKIP);
	obs_data_set_default_double(s, "gap_level", DEF_GAP_LEVEL);
	obs_data_set_default_bool(s, "interlace", DEF_INTERLACE);
	obs_data_set_default_double(s, "persistence", DEF_PERSISTENCE);
	obs_data_set_default_double(s, "spot_v", DEF_SPOT_V);
	obs_data_set_default_double(s, "spot_h", DEF_SPOT_H);
	obs_data_set_default_double(s, "scanline", DEF_SCANLINE);
	obs_data_set_default_double(s, "curvature", DEF_CURVATURE);
	obs_data_set_default_double(s, "vignette", DEF_VIGNETTE);
	obs_data_set_default_double(s, "overscan", DEF_OVERSCAN);
	obs_data_set_default_double(s, "degauss_len", DEF_DEGAUSS_LEN);
	obs_data_set_default_double(s, "degauss_strength", DEF_DEGAUSS_STRENGTH);
	obs_data_set_default_bool(s, "scope_on", DEF_SCOPE_ON);
	obs_data_set_default_double(s, "scope_line", DEF_SCOPE_LINE);
	obs_data_set_default_int(s, "signal_format", DEF_SIGNAL_FORMAT);
	obs_data_set_default_int(s, "signal_tap", DEF_SIGNAL_TAP);
	obs_data_set_default_string(s, "carrier_audio_source", "");
	obs_data_set_default_double(s, "audio_carrier_gain", DEF_AUDIO_CARRIER_GAIN);
	obs_data_set_default_double(s, "preview_zoom", DEF_PREVIEW_ZOOM);
	obs_data_set_default_double(s, "zoom_x", DEF_ZOOM_X);
	obs_data_set_default_double(s, "zoom_y", DEF_ZOOM_Y);
	obs_data_set_default_int(s, "mask_type", DEF_MASK_TYPE);
	obs_data_set_default_double(s, "mask_strength", DEF_MASK_STRENGTH);
	obs_data_set_default_double(s, "mask_width", DEF_MASK_WIDTH);
	obs_data_set_default_double(s, "mask_pitch", DEF_MASK_PITCH);
	obs_data_set_default_double(s, "wire_width", DEF_WIRE_WIDTH);

	obs_data_set_default_bool(s, "glitch_enable", DEF_GLITCH_ENABLE);
	obs_data_set_default_bool(s, "playback_enable", DEF_PLAYBACK_ENABLE);
	obs_data_set_default_double(s, "sweep_sec", DEF_SWEEP_SEC);
	obs_data_set_default_double(s, "peaking", DEF_PEAKING);
	obs_data_set_default_double(s, "ghost_gain", DEF_GHOST_GAIN);
	obs_data_set_default_double(s, "ghost_delay", DEF_GHOST_DELAY);
	obs_data_set_default_double(s, "v_roll_speed", DEF_V_ROLL_SPEED);
	obs_data_set_default_double(s, "h_jitter", DEF_H_JITTER);
	obs_data_set_default_double(s, "fading", DEF_FADING);
	obs_data_set_default_double(s, "flagging", DEF_FLAGGING);
	obs_data_set_default_double(s, "flag_wave", DEF_FLAG_WAVE);
	obs_data_set_default_double(s, "head_switch", DEF_HEAD_SWITCH);
	obs_data_set_default_double(s, "dropout", DEF_DROPOUT);
	obs_data_set_default_int(s, "dropout_mode", DEF_DROPOUT_MODE);
	obs_data_set_default_double(s, "tape_damage", DEF_TAPE_DAMAGE);
	obs_data_set_default_double(s, "tape_damage_pos", DEF_TAPE_DAMAGE_POS);
	obs_data_set_default_double(s, "tape_damage_drift", DEF_TAPE_DAMAGE_DRIFT);
	obs_data_set_default_double(s, "beat_gain", DEF_BEAT_GAIN);
	obs_data_set_default_double(s, "beat_freq", DEF_BEAT_FREQ);
	obs_data_set_default_double(s, "burst_len", DEF_BURST_LEN);
}

struct obs_source_info composite_tv_filter_info = {
	.id = "composite_tv_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = ntsc_get_name,
	.create = ntsc_create,
	.destroy = ntsc_destroy,
	.update = ntsc_update,
	.video_render = ntsc_render,
	.get_width = ntsc_width,
	.get_height = ntsc_height,
	.get_properties = ntsc_get_properties,
	.get_defaults = ntsc_defaults,
	.filter_add = ntsc_filter_add,
	.filter_remove = ntsc_filter_remove,
};
