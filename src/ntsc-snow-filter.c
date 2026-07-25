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

	/* animation state */
	uint64_t last_time;
	double chroma_phase_acc;
	uint32_t field_counter;
	int field_index4;

	/* parameters */
	float field_strength;
	float noise_floor;
	int yc_mode;
	float chroma_band_i;
	float chroma_band_q;
	float enc_chroma_gain;
	int aspect_mode;
	float agc_level;
	float agc_jitter;
	float if_bandwidth;
	float luma_bandwidth;
	float chroma_gain;
	bool color_killer;
	float chroma_drift;
	float contrast;
	float brightness;
	bool interlace;
	float persistence;
	float spot_v;
	float spot_h;
	float scanline;
	float curvature;
	float vignette;
	float overscan;
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

static inline void set_tex(gs_effect_t *e, const char *n, gs_texture_t *t)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, n);
	if (p)
		gs_effect_set_texture(p, t);
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
	f->chroma_band_i = (float)obs_data_get_double(s, "chroma_band_i");
	f->chroma_band_q = (float)obs_data_get_double(s, "chroma_band_q");
	f->enc_chroma_gain = (float)obs_data_get_double(s, "enc_chroma_gain");
	f->aspect_mode = (int)obs_data_get_int(s, "aspect_mode");
	f->agc_level = (float)obs_data_get_double(s, "agc_level");
	f->agc_jitter = (float)obs_data_get_double(s, "agc_jitter");
	f->if_bandwidth = (float)obs_data_get_double(s, "if_bandwidth");
	f->luma_bandwidth = (float)obs_data_get_double(s, "luma_bandwidth");
	f->chroma_gain = (float)obs_data_get_double(s, "chroma_gain");
	f->color_killer = obs_data_get_bool(s, "color_killer");
	f->chroma_drift = (float)obs_data_get_double(s, "chroma_drift");
	f->contrast = (float)obs_data_get_double(s, "contrast");
	f->brightness = (float)obs_data_get_double(s, "brightness");
	f->interlace = obs_data_get_bool(s, "interlace");
	f->persistence = (float)obs_data_get_double(s, "persistence");
	f->spot_v = (float)obs_data_get_double(s, "spot_v");
	f->spot_h = (float)obs_data_get_double(s, "spot_h");
	f->scanline = (float)obs_data_get_double(s, "scanline");
	f->curvature = (float)obs_data_get_double(s, "curvature");
	f->vignette = (float)obs_data_get_double(s, "vignette");
	f->overscan = (float)obs_data_get_double(s, "overscan");
}

static void *ntsc_create(obs_data_t *settings, obs_source_t *source)
{
	struct ntsc_snow *f = bzalloc(sizeof(struct ntsc_snow));
	f->source = source;

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
static gs_texrender_t *ensure_tr(gs_texrender_t **tr, enum gs_color_format fmt)
{
	if (!*tr)
		*tr = gs_texrender_create(fmt, GS_ZS_NONE);
	return *tr;
}

/* Render one full-screen technique into a texrender; returns its texture. */
static gs_texture_t *run_pass(gs_effect_t *e, gs_texrender_t *tr, uint32_t w, uint32_t h,
			      const char *tech, gs_texture_t *img)
{
	if (!tr)
		return NULL;
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, w, h))
		return NULL;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	set_tex(e, "image", img);
	while (gs_effect_loop(e, tech))
		gs_draw_sprite(img, 0, w, h);

	gs_blend_state_pop();
	gs_texrender_end(tr);
	return gs_texrender_get_texture(tr);
}

/* Give EVERY effect parameter a value so no draw is ever skipped with
 * "Not all shader parameters were set". Non-texture params default to zero,
 * textures to a placeholder; the real values are assigned afterwards. */
static void prime_all_params(gs_effect_t *e, gs_texture_t *placeholder)
{
	size_t count = gs_effect_get_num_params(e);
	for (size_t i = 0; i < count; i++) {
		gs_eparam_t *p = gs_effect_get_param_by_idx(e, i);
		struct gs_effect_param_info info;
		gs_effect_get_param_info(p, &info);
		switch (info.type) {
		case GS_SHADER_PARAM_BOOL:
			gs_effect_set_bool(p, false);
			break;
		case GS_SHADER_PARAM_FLOAT:
			gs_effect_set_float(p, 0.0f);
			break;
		case GS_SHADER_PARAM_INT:
			gs_effect_set_int(p, 0);
			break;
		case GS_SHADER_PARAM_VEC2: {
			struct vec2 z;
			vec2_zero(&z);
			gs_effect_set_vec2(p, &z);
			break;
		}
		case GS_SHADER_PARAM_VEC3: {
			struct vec3 z;
			vec3_zero(&z);
			gs_effect_set_vec3(p, &z);
			break;
		}
		case GS_SHADER_PARAM_VEC4: {
			struct vec4 z;
			vec4_zero(&z);
			gs_effect_set_vec4(p, &z);
			break;
		}
		case GS_SHADER_PARAM_TEXTURE:
			gs_effect_set_texture(p, placeholder);
			break;
		default:
			/* matrices (ViewProj) are set automatically by the backend */
			break;
		}
	}
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
	f->field_index4 = (f->field_index4 + 1) & 3;
	f->chroma_phase_acc = fmod(f->chroma_phase_acc + (double)f->chroma_drift * dt, 2.0 * NS_PI);

	gs_effect_t *e = f->effect;

	/* 1) capture the filter input into a texture */
	ensure_tr(&f->input, GS_RGBA);
	gs_texrender_reset(f->input);
	if (!gs_texrender_begin(f->input, cx, cy)) {
		obs_source_skip_video_filter(f->source);
		return;
	}
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	obs_source_video_render(target);
	gs_blend_state_pop();
	gs_texrender_end(f->input);
	gs_texture_t *input_tex = gs_texrender_get_texture(f->input);

	const float luma_cut = (float)(f->luma_bandwidth * 1.0e6 / SAMPLE_RATE_HZ);
	const float field_base_phase = (float)fmod(NS_PI * (double)f->field_index4, 2.0 * NS_PI);

	/* Create every render target up front, then give every effect parameter
	 * a value. The D3D11 backend skips a draw (logging "Not all shader
	 * parameters were set") if ANY parameter is unset at draw time, which
	 * blanks the output. */
	ensure_tr(&f->composite, GS_RGBA16F);
	ensure_tr(&f->detector, GS_RGBA16F);
	ensure_tr(&f->field[0], GS_RGBA16F);
	ensure_tr(&f->field[1], GS_RGBA16F);
	ensure_tr(&f->display[0], GS_RGBA);
	ensure_tr(&f->display[1], GS_RGBA);
	prime_all_params(e, input_tex);

	set_v2(e, "field_size", (float)FIELD_W, (float)FIELD_H);
	set_v2(e, "output_size", (float)cx, (float)cy);
	set_f(e, "burst_phase", (float)BURST_PHASE_RAD);
	set_f(e, "field_base_phase", field_base_phase);
	/* Encode */
	set_f(e, "luma_cutoff", luma_cut);
	set_f(e, "cutoff_i", (float)(f->chroma_band_i * 1.0e6 / SAMPLE_RATE_HZ));
	set_f(e, "cutoff_q", (float)(f->chroma_band_q * 1.0e6 / SAMPLE_RATE_HZ));
	set_f(e, "enc_chroma_gain", f->enc_chroma_gain);
	set_f(e, "aspect_mode", (float)f->aspect_mode);
	set_f(e, "input_aspect", (float)cx / (float)cy);
	/* Detect */
	set_i(e, "field_seed", (int)(f->field_counter & 0xFFFFFu));
	set_f(e, "if_cutoff", (float)(f->if_bandwidth * 1.0e6 * 0.5 / SAMPLE_RATE_HZ));
	set_f(e, "field_strength", f->field_strength);
	set_f(e, "noise_floor", f->noise_floor);
	set_f(e, "agc_jitter", f->agc_jitter);
	set_f(e, "snow_level", f->agc_level);
	/* Decode */
	set_f(e, "chroma_cutoff", (float)(CHROMA_DEMOD_LPF_HZ / SAMPLE_RATE_HZ));
	set_f(e, "chroma_gain", f->color_killer ? 0.0f : f->chroma_gain);
	set_f(e, "chroma_phase", (float)f->chroma_phase_acc);
	set_f(e, "contrast", f->contrast);
	set_f(e, "brightness", f->brightness);
	set_f(e, "yc_mode", (float)f->yc_mode);
	/* Display */
	set_f(e, "active_lines", (float)ACTIVE_LINES);
	set_f(e, "interlace_on", f->interlace ? 1.0f : 0.0f);
	set_f(e, "frame_parity", (float)(f->field_index4 & 1));
	set_f(e, "persistence", f->persistence);
	set_f(e, "spot_v", f->spot_v);
	set_f(e, "spot_h", f->spot_h);
	set_f(e, "scanline", f->scanline);
	set_f(e, "curvature", f->curvature);
	set_f(e, "vignette", f->vignette);
	set_f(e, "overscan", f->overscan);
	set_f(e, "pixels_per_line", (float)cy / (float)ACTIVE_LINES);
	set_tex(e, "field_prev", input_tex);
	set_tex(e, "display_prev", input_tex);

	/* 2) Encode -> 3) Detect -> 4) Decode */
	ensure_tr(&f->composite, GS_RGBA16F);
	gs_texture_t *comp = run_pass(e, f->composite, FIELD_W, FIELD_H, "Encode", input_tex);

	ensure_tr(&f->detector, GS_RGBA16F);
	gs_texture_t *det = run_pass(e, f->detector, FIELD_W, FIELD_H, "Detect", comp);

	ensure_tr(&f->field[0], GS_RGBA16F);
	ensure_tr(&f->field[1], GS_RGBA16F);
	gs_texture_t *field_cur = run_pass(e, f->field[f->field_idx], FIELD_W, FIELD_H, "Decode", det);

	/* 5) Display: interlaced CRT presentation into the display buffer */
	gs_texture_t *field_prev = gs_texrender_get_texture(f->field[1 - f->field_idx]);
	if (!field_prev)
		field_prev = field_cur;
	set_tex(e, "field_prev", field_prev);

	gs_texture_t *disp_prev = gs_texrender_get_texture(f->display[1 - f->display_idx]);
	if (!disp_prev)
		disp_prev = field_cur;
	set_tex(e, "display_prev", disp_prev);

	ensure_tr(&f->display[0], GS_RGBA);
	ensure_tr(&f->display[1], GS_RGBA);
	gs_texture_t *display_cur = run_pass(e, f->display[f->display_idx], cx, cy, "Display", field_cur);

	/* 6) blit the final frame to the actual filter output */
	if (display_cur) {
		set_tex(e, "image", display_cur);
		while (gs_effect_loop(e, "Draw"))
			gs_draw_sprite(display_cur, 0, cx, cy);
	}

	/* swap ping-pong buffers for the next frame */
	f->field_idx ^= 1;
	f->display_idx ^= 1;
}

/* ---- properties ------------------------------------------------------ */

static obs_properties_t *ntsc_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();

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

	obs_properties_add_bool(p, "interlace", obs_module_text("NTSCSnow.Interlace"));
	obs_properties_add_float_slider(p, "persistence", obs_module_text("NTSCSnow.Persistence"), 0.0, 0.75, 0.01);
	obs_properties_add_float_slider(p, "spot_v", obs_module_text("NTSCSnow.SpotV"), 0.30, 1.60, 0.01);
	obs_properties_add_float_slider(p, "spot_h", obs_module_text("NTSCSnow.SpotH"), 0.30, 2.50, 0.01);
	obs_properties_add_float_slider(p, "scanline", obs_module_text("NTSCSnow.Scanline"), 0.0, 0.60, 0.01);
	obs_properties_add_float_slider(p, "curvature", obs_module_text("NTSCSnow.Curvature"), 0.0, 0.12, 0.005);
	obs_properties_add_float_slider(p, "vignette", obs_module_text("NTSCSnow.Vignette"), 0.0, 0.80, 0.01);
	obs_properties_add_float_slider(p, "overscan", obs_module_text("NTSCSnow.Overscan"), 0.0, 0.08, 0.005);

	return p;
}

static void ntsc_defaults(obs_data_t *s)
{
	obs_data_set_default_double(s, "field_strength", 1.00);
	obs_data_set_default_double(s, "noise_floor", 0.04);
	obs_data_set_default_int(s, "yc_mode", 1);
	obs_data_set_default_double(s, "chroma_band_i", 1.3);
	obs_data_set_default_double(s, "chroma_band_q", 0.4);
	obs_data_set_default_double(s, "enc_chroma_gain", 1.0);
	obs_data_set_default_int(s, "aspect_mode", 0);
	obs_data_set_default_double(s, "agc_level", 0.62);
	obs_data_set_default_double(s, "agc_jitter", 0.06);
	obs_data_set_default_double(s, "if_bandwidth", 5.0);
	obs_data_set_default_double(s, "luma_bandwidth", 4.2);
	obs_data_set_default_double(s, "chroma_gain", 0.70);
	obs_data_set_default_bool(s, "color_killer", false);
	obs_data_set_default_double(s, "chroma_drift", 0.6);
	obs_data_set_default_double(s, "contrast", 1.15);
	obs_data_set_default_double(s, "brightness", -0.02);
	obs_data_set_default_bool(s, "interlace", true);
	obs_data_set_default_double(s, "persistence", 0.22);
	obs_data_set_default_double(s, "spot_v", 0.45);
	obs_data_set_default_double(s, "spot_h", 0.85);
	obs_data_set_default_double(s, "scanline", 0.35);
	obs_data_set_default_double(s, "curvature", 0.025);
	obs_data_set_default_double(s, "vignette", 0.30);
	obs_data_set_default_double(s, "overscan", 0.02);
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
