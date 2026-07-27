/*
NTSC Snow — OBS video filter
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

#include "ntsc-snow-filter.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>
#include <math.h>

#include <plugin-support.h>

#define NS_PI 3.14159265358979323846

/* Fixed NTSC-M internal geometry (4fsc sampling). */
#define FIELD_W 754    /* active samples per line   */
#define FIELD_H 243    /* active lines per field    */
#define ACTIVE_LINES 486
#define SAMPLE_RATE_HZ 14318180.0 /* 4 * 3.579545 MHz */
#define BURST_PHASE_RAD (57.0 * NS_PI / 180.0)
#define CHROMA_DEMOD_LPF_HZ 600000.0
/* Settings are in MHz; the shader wants cutoffs normalised to the sample rate. */
#define MHZ_TO_NORM (1.0e6 / SAMPLE_RATE_HZ)

/* Power on/off envelope (seconds), ported from the reference PowerEnvelope. */
#define WARMUP_SEC 1.5f
#define COLLAPSE_V_SEC 0.13f
#define COLLAPSE_H_SEC 0.28f
#define AFTERGLOW_SEC 0.55f

enum power_state { POWER_OFF = 0, POWER_WARMING, POWER_ON, POWER_SHUTTING };

struct ntsc_snow {
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
	int aspect_mode;
	int screen_aspect;
	float agc_level;
	float agc_jitter;
	float if_cutoff;
	float luma_cutoff;
	float chroma_gain;
	float chroma_drift;
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
	float v_bar;
	float h_jitter;
	float flagging;
	float head_switch;
	float dropout;
	float beat_gain;
	float beat_norm;
	float burst_len;

	/* glitch animation state */
	double v_roll_pos;
	double beat_phase;
	float burst;          /* 1 -> 0 momentary glitch envelope */
	long long glitch_pulse; /* bumped by the dock to fire a burst */
	obs_hotkey_id glitch_hotkey;

	/* degauss */
	float degauss_len;
	float degauss_strength;

	/* inspection zoom */
	float preview_zoom;
	float zoom_x;
	float zoom_y;
	float degauss;        /* 1 -> 0 envelope */
	double degauss_phase;
	long long degauss_pulse;
	obs_hotkey_id degauss_hotkey;
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

static void power_advance(struct ntsc_snow *f, float dt)
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

static struct power_gfx power_uniforms(const struct ntsc_snow *f)
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

/* ---- OBS source callbacks -------------------------------------------- */

static const char *ntsc_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("NTSCSnow.Name");
}

static void ntsc_update(void *data, obs_data_t *s)
{
	struct ntsc_snow *f = data;
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
	f->if_cutoff = (float)(obs_data_get_double(s, "if_bandwidth") * 0.5 * MHZ_TO_NORM);
	f->luma_cutoff = (float)(obs_data_get_double(s, "luma_bandwidth") * MHZ_TO_NORM);
	/* The colour killer is a plain gate, so fold it in here rather than
	 * re-testing it on every pass. */
	f->chroma_gain = obs_data_get_bool(s, "color_killer")
				 ? 0.0f
				 : (float)obs_data_get_double(s, "chroma_gain");
	f->chroma_drift = (float)obs_data_get_double(s, "chroma_drift");
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

	/* Every glitch is gated by the group toggle, so apply the gate once here
	 * and let the render path just read the values. */
	const bool glitch = obs_data_get_bool(s, "glitch_enable");
	f->ghost_gain = glitch ? (float)obs_data_get_double(s, "ghost_gain") : 0.0f;
	f->ghost_delay = glitch ? (float)obs_data_get_double(s, "ghost_delay") : 0.0f;
	f->v_roll_speed = glitch ? (float)obs_data_get_double(s, "v_roll_speed") : 0.0f;
	f->v_bar = glitch ? (float)obs_data_get_double(s, "v_bar") : 0.0f;
	f->h_jitter = glitch ? (float)obs_data_get_double(s, "h_jitter") : 0.0f;
	f->flagging = glitch ? (float)obs_data_get_double(s, "flagging") : 0.0f;
	f->head_switch = glitch ? (float)obs_data_get_double(s, "head_switch") : 0.0f;
	f->dropout = glitch ? (float)obs_data_get_double(s, "dropout") : 0.0f;
	f->beat_gain = glitch ? (float)obs_data_get_double(s, "beat_gain") : 0.0f;
	f->beat_norm = (float)(obs_data_get_double(s, "beat_freq") * MHZ_TO_NORM);
	f->burst_len = (float)obs_data_get_double(s, "burst_len");

	f->degauss_len = (float)obs_data_get_double(s, "degauss_len");
	f->degauss_strength = (float)obs_data_get_double(s, "degauss_strength");
	f->preview_zoom = (float)obs_data_get_double(s, "preview_zoom");
	f->zoom_x = (float)obs_data_get_double(s, "zoom_x");
	f->zoom_y = (float)obs_data_get_double(s, "zoom_y");

	/* The dock fires a burst by incrementing this counter. */
	long long pulse = obs_data_get_int(s, "glitch_pulse");
	if (pulse != f->glitch_pulse) {
		f->glitch_pulse = pulse;
		f->burst = 1.0f;
	}
	long long dpulse = obs_data_get_int(s, "degauss_pulse");
	if (dpulse != f->degauss_pulse) {
		f->degauss_pulse = dpulse;
		f->degauss = 1.0f;
	}
}

/* Hotkey: fire a momentary glitch burst. */
static void ntsc_glitch_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct ntsc_snow *f = data;
	f->burst = 1.0f;
}

/* Hotkey: run the degauss coil. */
static void ntsc_degauss_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct ntsc_snow *f = data;
	f->degauss = 1.0f;
}

static void *ntsc_create(obs_data_t *settings, obs_source_t *source)
{
	struct ntsc_snow *f = bzalloc(sizeof(struct ntsc_snow));
	f->source = source;
	f->glitch_hotkey = obs_hotkey_register_source(source, "ntsc_snow.glitch",
						      obs_module_text("NTSCSnow.Hotkey.Glitch"),
						      ntsc_glitch_hotkey, f);
	f->degauss_hotkey = obs_hotkey_register_source(source, "ntsc_snow.degauss",
						       obs_module_text("NTSCSnow.Hotkey.Degauss"),
						       ntsc_degauss_hotkey, f);

	char *path = obs_module_file("effects/ntsc-snow.effect");
	obs_enter_graphics();
	char *errors = NULL;
	f->effect = gs_effect_create_from_file(path, &errors);
	obs_leave_graphics();
	if (!f->effect) {
		obs_log(LOG_ERROR, "Failed to load ntsc-snow.effect: %s", errors ? errors : "(unknown)");
	}
	bfree(errors);
	bfree(path);

	ntsc_update(f, settings);
	/* Start already settled in the target state, with nothing animating: the
	 * saved pulse counters are momentary events that were fired in an earlier
	 * session, so adopting their values must not replay them now. */
	f->power_state = f->powered ? POWER_ON : POWER_OFF;
	f->power_elapsed = 0.0f;
	f->burst = 0.0f;
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
	struct ntsc_snow *f = data;
	if (f->glitch_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(f->glitch_hotkey);
	if (f->degauss_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(f->degauss_hotkey);
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
	obs_leave_graphics();
	bfree(f);
}

/* Ensure a texrender exists with the requested format. */
static void ensure_tr(gs_texrender_t **tr, enum gs_color_format fmt)
{
	if (!*tr)
		*tr = gs_texrender_create(fmt, GS_ZS_NONE);
}

/* Start drawing into a texrender with the pipeline state every pass expects. */
static bool target_begin(gs_texrender_t *tr, uint32_t w, uint32_t h)
{
	if (!tr)
		return false;
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, w, h))
		return false;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
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

static void apply_params(struct ntsc_snow *f, gs_effect_t *e, uint32_t cx, uint32_t cy);

/* Render one full-screen technique into a texrender; returns its texture.
 * Applying the parameters in here - rather than at each call site - is what
 * makes the libobs quirk described on apply_params() impossible to trip over. */
static gs_texture_t *run_pass(struct ntsc_snow *f, gs_effect_t *e, gs_texrender_t *tr, uint32_t w, uint32_t h,
			      const char *tech, gs_texture_t *img, uint32_t cx, uint32_t cy)
{
	if (!target_begin(tr, w, h))
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
static void apply_params(struct ntsc_snow *f, gs_effect_t *e, uint32_t cx, uint32_t cy)
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
	set_f(e, "aspect_mode", (float)f->aspect_mode);
	set_f(e, "input_aspect", (float)cx / (float)cy);
	float scr_aspect = (f->screen_aspect == 1)   ? (4.0f / 3.0f)
			   : (f->screen_aspect == 2) ? (16.0f / 9.0f)
						     : ((float)cx / (float)cy);
	set_f(e, "screen_aspect", scr_aspect);
	/* Detect */
	set_i(e, "field_seed", (int)(f->field_counter & 0xFFFFFu));
	set_f(e, "if_cutoff", f->if_cutoff);
	set_f(e, "field_strength", f->field_strength);
	set_f(e, "noise_floor", f->noise_floor);
	set_f(e, "agc_jitter", f->agc_jitter);
	set_f(e, "snow_level", f->agc_level);
	/* Decode */
	set_f(e, "chroma_cutoff", (float)(CHROMA_DEMOD_LPF_HZ / SAMPLE_RATE_HZ));
	set_f(e, "chroma_gain", f->chroma_gain);
	set_f(e, "chroma_phase", (float)f->chroma_phase_acc);
	set_f(e, "contrast", f->contrast);
	set_f(e, "brightness", f->brightness);
	set_f(e, "yc_mode", (float)f->yc_mode);
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
	set_f(e, "spot_h", f->spot_h);
	set_f(e, "scanline", f->scanline);
	set_f(e, "curvature", f->curvature);
	set_f(e, "vignette", f->vignette);
	set_f(e, "overscan", f->overscan);
	/* Zooming in means each scan line covers proportionally more output
	 * pixels, so the scan-line visibility ramp has to know about it -
	 * otherwise magnifying would not reveal the lines it exists to show. */
	const float zoom = f->preview_zoom > 1.0f ? f->preview_zoom : 1.0f;
	set_f(e, "pixels_per_line", (float)cy / lines * zoom);
	set_f(e, "preview_zoom", zoom);
	set_f(e, "zoom_x", f->zoom_x);
	set_f(e, "zoom_y", f->zoom_y);

	/* Glitches. The steady-state values were already gated by glitch_enable in
	 * ntsc_update(); the momentary burst adds on top, so the dock button and
	 * the hotkey still work from a clean picture. */
	const float b = f->burst;
	set_f(e, "ghost_gain", f->ghost_gain);
	set_f(e, "ghost_delay", f->ghost_delay);
	set_f(e, "beat_gain", f->beat_gain);
	set_f(e, "beat_norm", f->beat_norm);
	set_f(e, "beat_phase", (float)f->beat_phase);
	set_f(e, "v_roll", (float)f->v_roll_pos);
	/* The bar is specified in NTSC lines (of the 486-line active frame), so
	 * scale it when the frame is drawn with fewer lines - otherwise 240p would
	 * show a blanking bar twice as deep as it should be. */
	set_f(e, "v_bar", (f->v_bar + b * 12.0f) * (lines / (float)ACTIVE_LINES));
	set_f(e, "h_jitter", clampf(f->h_jitter + b * 0.8f, 0.0f, 2.0f));
	set_f(e, "flagging", clampf(f->flagging + b * 0.6f, 0.0f, 2.0f));
	set_f(e, "head_switch", clampf(f->head_switch + b * 0.5f, 0.0f, 2.0f));
	set_f(e, "dropout", clampf(f->dropout + b * 0.6f, 0.0f, 2.0f));

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

static void ntsc_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct ntsc_snow *f = data;
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

	/* glitch animation: decay the burst, advance the roll and the beat */
	if (f->burst > 0.0f) {
		float len = f->burst_len > 0.05f ? f->burst_len : 0.4f;
		f->burst -= dt / len;
		if (f->burst < 0.0f)
			f->burst = 0.0f;
	}
	/* The burst kicks the vertical hold hard so the picture visibly tumbles
	 * (several screens over the burst) before it re-locks. */
	/* The roll is measured in scan lines, so one screen is however many lines
	 * the frame is currently drawn with. */
	const double frame_lines = (double)f->scan_lines;
	float roll_speed = f->v_roll_speed + f->burst * 6.0f;
	if (roll_speed != 0.0f) {
		f->v_roll_pos = fmod(f->v_roll_pos + (double)roll_speed * frame_lines * dt, frame_lines);
	} else if (f->v_roll_pos != 0.0) {
		/* Vertical hold re-locks: slide back to the framed position by the
		 * shortest way round, then snap once we are within half a line. */
		const double half = frame_lines * 0.5;
		double p = fmod(f->v_roll_pos, frame_lines);
		if (p > half)
			p -= frame_lines;
		else if (p < -half)
			p += frame_lines;
		p *= exp(-(double)dt / 0.30); /* ~300 ms settle, visible glide back */
		f->v_roll_pos = (fabs(p) < 0.5) ? 0.0 : p;
	}
	f->beat_phase = fmod(f->beat_phase + 2.0 * NS_PI * 0.7 * dt, 2.0 * NS_PI);

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
	if (!target_begin(f->input, cx, cy)) {
		obs_source_skip_video_filter(f->source);
		return;
	}
	obs_source_video_render(target);
	target_end(f->input);
	gs_texture_t *input_tex = gs_texrender_get_texture(f->input);

	ensure_tr(&f->composite, GS_RGBA16F);
	ensure_tr(&f->detector, GS_RGBA16F);
	ensure_tr(&f->field[0], GS_RGBA16F);
	ensure_tr(&f->field[1], GS_RGBA16F);
	ensure_tr(&f->display[0], GS_RGBA);
	ensure_tr(&f->display[1], GS_RGBA);

	/* 2) Encode -> 3) Detect -> 4) Decode. run_pass() applies the parameters
	 * itself, which is what keeps the libobs clear-on-technique-end quirk
	 * from being a rule anyone has to remember. */
	gs_texture_t *comp = run_pass(f, e, f->composite, FIELD_W, FIELD_H, "Encode", input_tex, cx, cy);
	gs_texture_t *det = run_pass(f, e, f->detector, FIELD_W, FIELD_H, "Detect", comp, cx, cy);
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

static obs_properties_t *ntsc_get_properties(void *data)
{
	struct ntsc_snow *f = data;
	obs_properties_t *p = obs_properties_create();

	if (f && f->source)
		obs_properties_set_param(p, obs_source_get_weak_source(f->source), ntsc_props_destroyed);

	obs_properties_add_text(p, "hint", obs_module_text("NTSCSnow.Hint"), OBS_TEXT_INFO);
	obs_properties_add_bool(p, "power", obs_module_text("NTSCSnow.Power"));
	obs_properties_add_float_slider(p, "field_strength", obs_module_text("NTSCSnow.FieldStrength"), 0.0, 1.0,
					0.01);
	obs_properties_add_float_slider(p, "noise_floor", obs_module_text("NTSCSnow.NoiseFloor"), 0.0, 0.30, 0.01);

	obs_property_t *yc = obs_properties_add_list(p, "yc_mode", obs_module_text("NTSCSnow.YCMode"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(yc, obs_module_text("NTSCSnow.YCMode.Bandpass"), 0);
	obs_property_list_add_int(yc, obs_module_text("NTSCSnow.YCMode.Comb2"), 1);
	obs_property_list_add_int(yc, obs_module_text("NTSCSnow.YCMode.Comb3"), 2);

	obs_properties_add_float_slider(p, "chroma_band_i", obs_module_text("NTSCSnow.ChromaBandI"), 0.4, 2.0, 0.1);
	obs_properties_add_float_slider(p, "chroma_band_q", obs_module_text("NTSCSnow.ChromaBandQ"), 0.2, 1.3, 0.1);
	obs_properties_add_float_slider(p, "enc_chroma_gain", obs_module_text("NTSCSnow.EncChromaGain"), 0.0, 1.5,
					0.05);

	obs_property_t *scr = obs_properties_add_list(p, "screen_aspect", obs_module_text("NTSCSnow.ScreenAspect"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(scr, obs_module_text("NTSCSnow.ScreenAspect.Source"), 0);
	obs_property_list_add_int(scr, obs_module_text("NTSCSnow.ScreenAspect.43"), 1);
	obs_property_list_add_int(scr, obs_module_text("NTSCSnow.ScreenAspect.169"), 2);

	obs_property_t *asp = obs_properties_add_list(p, "aspect_mode", obs_module_text("NTSCSnow.AspectMode"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(asp, obs_module_text("NTSCSnow.AspectMode.Letterbox"), 0);
	obs_property_list_add_int(asp, obs_module_text("NTSCSnow.AspectMode.Stretch"), 1);

	obs_properties_add_float_slider(p, "agc_level", obs_module_text("NTSCSnow.AgcLevel"), 0.20, 0.95, 0.01);
	obs_properties_add_float_slider(p, "agc_jitter", obs_module_text("NTSCSnow.AgcJitter"), 0.0, 0.40, 0.01);
	obs_properties_add_float_slider(p, "if_bandwidth", obs_module_text("NTSCSnow.IfBandwidth"), 2.0, 7.0, 0.1);

	obs_properties_add_float_slider(p, "luma_bandwidth", obs_module_text("NTSCSnow.LumaBandwidth"), 1.0, 4.2, 0.1);
	obs_properties_add_float_slider(p, "chroma_gain", obs_module_text("NTSCSnow.ChromaGain"), 0.0, 2.0, 0.01);
	obs_properties_add_bool(p, "color_killer", obs_module_text("NTSCSnow.ColorKiller"));
	obs_properties_add_float_slider(p, "chroma_drift", obs_module_text("NTSCSnow.ChromaDrift"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(p, "contrast", obs_module_text("NTSCSnow.Contrast"), 0.3, 2.0, 0.01);
	obs_properties_add_float_slider(p, "brightness", obs_module_text("NTSCSnow.Brightness"), -0.3, 0.3, 0.01);

	obs_property_t *sl = obs_properties_add_list(p, "scan_lines", obs_module_text("NTSCSnow.ScanLines"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(sl, obs_module_text("NTSCSnow.ScanLines.486"), 486);
	obs_property_list_add_int(sl, obs_module_text("NTSCSnow.ScanLines.243"), 243);
	obs_property_list_add_int(sl, obs_module_text("NTSCSnow.ScanLines.162"), 162);

	obs_properties_add_bool(p, "line_skip", obs_module_text("NTSCSnow.LineSkip"));
	obs_properties_add_float_slider(p, "gap_level", obs_module_text("NTSCSnow.GapLevel"), 0.0, 1.0, 0.05);

	obs_properties_add_bool(p, "interlace", obs_module_text("NTSCSnow.Interlace"));
	obs_properties_add_float_slider(p, "persistence", obs_module_text("NTSCSnow.Persistence"), 0.0, 0.75, 0.01);
	obs_properties_add_float_slider(p, "spot_v", obs_module_text("NTSCSnow.SpotV"), 0.30, 1.60, 0.01);
	obs_properties_add_float_slider(p, "spot_h", obs_module_text("NTSCSnow.SpotH"), 0.30, 2.50, 0.01);
	obs_properties_add_float_slider(p, "scanline", obs_module_text("NTSCSnow.Scanline"), 0.0, 0.60, 0.01);
	obs_properties_add_float_slider(p, "curvature", obs_module_text("NTSCSnow.Curvature"), 0.0, 0.12, 0.005);
	obs_properties_add_float_slider(p, "vignette", obs_module_text("NTSCSnow.Vignette"), 0.0, 0.80, 0.01);
	obs_properties_add_float_slider(p, "overscan", obs_module_text("NTSCSnow.Overscan"), 0.0, 0.08, 0.005);
	obs_properties_add_float_slider(p, "degauss_len", obs_module_text("NTSCSnow.DegaussLen"), 0.3, 3.0, 0.1);
	obs_properties_add_float_slider(p, "degauss_strength", obs_module_text("NTSCSnow.DegaussStrength"), 0.0, 1.0,
					0.01);

	/* Inspection aid: magnifies the output, so it must be wound back to 1.0
	 * before going live - the label says so. */
	obs_properties_add_float_slider(p, "preview_zoom", obs_module_text("NTSCSnow.PreviewZoom"), 1.0, 8.0, 0.1);
	obs_properties_add_text(p, "zoom_hint", obs_module_text("NTSCSnow.ZoomHint"), OBS_TEXT_INFO);
	obs_properties_add_float_slider(p, "zoom_x", obs_module_text("NTSCSnow.ZoomX"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "zoom_y", obs_module_text("NTSCSnow.ZoomY"), 0.0, 1.0, 0.01);

	/* --- glitches (collapsible, switched off by default) --- */
	obs_properties_t *g = obs_properties_create();
	obs_properties_add_float_slider(g, "ghost_gain", obs_module_text("NTSCSnow.GhostGain"), 0.0, 0.8, 0.01);
	obs_properties_add_float_slider(g, "ghost_delay", obs_module_text("NTSCSnow.GhostDelay"), -60.0, 120.0, 1.0);
	obs_properties_add_float_slider(g, "v_roll_speed", obs_module_text("NTSCSnow.VRollSpeed"), -2.0, 2.0, 0.01);
	obs_properties_add_float_slider(g, "v_bar", obs_module_text("NTSCSnow.VBar"), 0.0, 45.0, 1.0);
	obs_properties_add_float_slider(g, "h_jitter", obs_module_text("NTSCSnow.HJitter"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, "flagging", obs_module_text("NTSCSnow.Flagging"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, "head_switch", obs_module_text("NTSCSnow.HeadSwitch"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, "dropout", obs_module_text("NTSCSnow.Dropout"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, "beat_gain", obs_module_text("NTSCSnow.BeatGain"), 0.0, 0.5, 0.01);
	obs_properties_add_float_slider(g, "beat_freq", obs_module_text("NTSCSnow.BeatFreq"), 0.1, 5.0, 0.01);
	obs_properties_add_float_slider(g, "burst_len", obs_module_text("NTSCSnow.BurstLen"), 0.1, 2.0, 0.05);
	obs_properties_add_group(p, "glitch_enable", obs_module_text("NTSCSnow.Glitch"), OBS_GROUP_CHECKABLE, g);

	return p;
}

static void ntsc_defaults(obs_data_t *s)
{
	obs_data_set_default_bool(s, "power", true);
	obs_data_set_default_double(s, "field_strength", 1.00);
	obs_data_set_default_double(s, "noise_floor", 0.04);
	obs_data_set_default_int(s, "yc_mode", 1);
	obs_data_set_default_double(s, "chroma_band_i", 1.3);
	obs_data_set_default_double(s, "chroma_band_q", 0.4);
	obs_data_set_default_double(s, "enc_chroma_gain", 1.0);
	obs_data_set_default_int(s, "aspect_mode", 0);
	obs_data_set_default_int(s, "screen_aspect", 0);
	obs_data_set_default_double(s, "agc_level", 0.62);
	obs_data_set_default_double(s, "agc_jitter", 0.06);
	obs_data_set_default_double(s, "if_bandwidth", 5.0);
	obs_data_set_default_double(s, "luma_bandwidth", 4.2);
	obs_data_set_default_double(s, "chroma_gain", 0.70);
	obs_data_set_default_bool(s, "color_killer", false);
	obs_data_set_default_double(s, "chroma_drift", 0.6);
	obs_data_set_default_double(s, "contrast", 1.15);
	obs_data_set_default_double(s, "brightness", -0.02);
	obs_data_set_default_int(s, "scan_lines", ACTIVE_LINES);
	obs_data_set_default_bool(s, "line_skip", false);
	obs_data_set_default_double(s, "gap_level", 0.15);
	obs_data_set_default_bool(s, "interlace", true);
	obs_data_set_default_double(s, "persistence", 0.22);
	obs_data_set_default_double(s, "spot_v", 0.45);
	obs_data_set_default_double(s, "spot_h", 0.85);
	obs_data_set_default_double(s, "scanline", 0.35);
	obs_data_set_default_double(s, "curvature", 0.025);
	obs_data_set_default_double(s, "vignette", 0.30);
	obs_data_set_default_double(s, "overscan", 0.02);
	obs_data_set_default_double(s, "degauss_len", 1.2);
	obs_data_set_default_double(s, "degauss_strength", 0.7);
	obs_data_set_default_double(s, "preview_zoom", 1.0);
	obs_data_set_default_double(s, "zoom_x", 0.5);
	obs_data_set_default_double(s, "zoom_y", 0.5);

	obs_data_set_default_bool(s, "glitch_enable", false);
	obs_data_set_default_double(s, "ghost_gain", 0.0);
	obs_data_set_default_double(s, "ghost_delay", 24.0);
	obs_data_set_default_double(s, "v_roll_speed", 0.0);
	/* 525 total lines - 486 active = 39 lines of vertical blanking. */
	obs_data_set_default_double(s, "v_bar", 39.0);
	obs_data_set_default_double(s, "h_jitter", 0.0);
	obs_data_set_default_double(s, "flagging", 0.0);
	obs_data_set_default_double(s, "head_switch", 0.0);
	obs_data_set_default_double(s, "dropout", 0.0);
	obs_data_set_default_double(s, "beat_gain", 0.0);
	obs_data_set_default_double(s, "beat_freq", 2.5);
	obs_data_set_default_double(s, "burst_len", 0.4);
}

struct obs_source_info ntsc_snow_filter_info = {
	.id = "ntsc_snow_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = ntsc_get_name,
	.create = ntsc_create,
	.destroy = ntsc_destroy,
	.update = ntsc_update,
	.video_render = ntsc_render,
	.get_properties = ntsc_get_properties,
	.get_defaults = ntsc_defaults,
};
