/*
Composite TV — audio filter
Copyright (C) 2026

Synthesises the no-signal TV audio of the reference NTSC Snow Simulator
(https://github.com/abanum/ZAA, MIT): FM triangular noise (75us de-emphasis =
first-order high-pass) band-limited to 15 kHz, plus a faint intercarrier tone.
The source audio is ducked as field strength falls, so sound fades from the
programme audio into full static in step with the picture.

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

#include "composite-tv-audio.h"

#include <obs-module.h>
#include <math.h>

#define NA_PI 3.14159265358979323846
#define NA_TWO_PI 6.283185307179586
#define NA_MAX_CH 8

#define DEEMPHASIS_TAU_S 75.0e-6
#define AUDIO_LPF_HZ 15000.0
#define INTERCARRIER_HZ 15734.264

struct ntsc_audio {
	obs_source_t *source;

	/* parameters */
	float field_strength;
	float volume;
	float intercarrier;
	bool powered;

	/* derived from the sample rate */
	double sample_rate;
	float a_deemph;   /* de-emphasis high-pass coefficient */
	float alpha_lp;   /* one-pole low-pass coefficient     */
	float g_ramp;     /* power-gain ramp coefficient       */
	double ic_dphase; /* intercarrier phase step           */
	double dg_dphase; /* degauss hum phase step            */
	float dg_decay;   /* degauss envelope step per sample  */

	/* per-channel filter state */
	float hp_x1[NA_MAX_CH];
	float hp_y1[NA_MAX_CH];
	float lp_y1[NA_MAX_CH];
	uint32_t prng[NA_MAX_CH];

	double ic_phase; /* intercarrier oscillator phase */
	float master;    /* smoothed power gain (0..1)    */

	/* degauss "boing" */
	float degauss;   /* 1 -> 0 envelope */
	double dg_phase; /* low hum phase   */
	long long degauss_pulse;
	obs_hotkey_id degauss_hotkey;
	obs_hotkey_id power_hotkey;
};

/* ---- helpers --------------------------------------------------------- */

static inline uint32_t xrng(uint32_t *s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

static inline float xrngf(uint32_t *s)
{
	return (float)((double)xrng(s) * 2.3283064365386963e-10);
}

static inline float gaussf(uint32_t *s)
{
	float u1 = xrngf(s);
	if (u1 < 1.0e-7f)
		u1 = 1.0e-7f;
	float u2 = xrngf(s);
	return sqrtf(-2.0f * logf(u1)) * cosf((float)NA_TWO_PI * u2);
}

static inline float smoothstep01(float e0, float e1, float x)
{
	float t = (x - e0) / (e1 - e0);
	t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
	return t * t * (3.0f - 2.0f * t);
}

static void na_recalc_coeffs(struct ntsc_audio *f)
{
	struct obs_audio_info oai;
	if (obs_get_audio_info(&oai) && oai.samples_per_sec > 0)
		f->sample_rate = (double)oai.samples_per_sec;
	if (f->sample_rate < 1.0)
		f->sample_rate = 48000.0;

	double dt = 1.0 / f->sample_rate;
	f->a_deemph = (float)(DEEMPHASIS_TAU_S / (DEEMPHASIS_TAU_S + dt));
	double rc = 1.0 / (NA_TWO_PI * AUDIO_LPF_HZ);
	f->alpha_lp = (float)(dt / (rc + dt));
	f->g_ramp = (float)(dt / (0.03 + dt));            /* ~30 ms power ramp   */
	f->ic_dphase = NA_TWO_PI * INTERCARRIER_HZ / f->sample_rate;
	f->dg_dphase = NA_TWO_PI * 55.0 / f->sample_rate; /* degauss hum, 55 Hz  */
	f->dg_decay = (float)(dt / 0.35);                 /* boing decays in .35s */
}

/* ---- OBS source callbacks -------------------------------------------- */

static const char *na_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("CompositeTV.Audio.Name");
}

static void na_update(void *data, obs_data_t *s)
{
	struct ntsc_audio *f = data;
	f->field_strength = (float)obs_data_get_double(s, "field_strength");
	f->volume = (float)obs_data_get_double(s, "volume");
	f->intercarrier = (float)obs_data_get_double(s, "intercarrier");
	f->powered = obs_data_get_bool(s, "power");
	na_recalc_coeffs(f);

	/* The dock fires the degauss sound by incrementing this counter. */
	long long dpulse = obs_data_get_int(s, "degauss_pulse");
	if (dpulse != f->degauss_pulse) {
		f->degauss_pulse = dpulse;
		f->degauss = 1.0f;
		f->dg_phase = 0.0;
	}
}

/* Hotkey: sound the degauss coil. */
static void na_degauss_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct ntsc_audio *f = data;
	f->degauss = 1.0f;
	f->dg_phase = 0.0;
}

/* Hotkey: toggle mains power. Routed through the settings so the properties
 * dialog and the dock stay in sync with the new state. */
static void na_power_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct ntsc_audio *f = data;
	obs_data_t *s = obs_data_create();
	obs_data_set_bool(s, "power", !f->powered);
	obs_source_update(f->source, s);
	obs_data_release(s);
}

/* The hotkey settings UI only lists sources it shows elsewhere (scenes and
 * regular sources), never the filter's own private source, so the hotkeys
 * must be registered on the parent the filter is attached to. */
static void na_filter_add(void *data, obs_source_t *parent)
{
	struct ntsc_audio *f = data;
	if (f->degauss_hotkey != OBS_INVALID_HOTKEY_ID)
		return;
	f->degauss_hotkey = obs_hotkey_register_source(parent, "composite_tv_audio.degauss",
						       obs_module_text("CompositeTV.Hotkey.Degauss"),
						       na_degauss_hotkey, f);
	f->power_hotkey = obs_hotkey_register_source(parent, "composite_tv_audio.power",
						     obs_module_text("CompositeTV.Hotkey.Power"),
						     na_power_hotkey, f);
}

static void na_unregister_hotkeys(struct ntsc_audio *f)
{
	if (f->degauss_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(f->degauss_hotkey);
	if (f->power_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(f->power_hotkey);
	f->degauss_hotkey = OBS_INVALID_HOTKEY_ID;
	f->power_hotkey = OBS_INVALID_HOTKEY_ID;
}

static void na_filter_remove(void *data, obs_source_t *parent)
{
	UNUSED_PARAMETER(parent);
	na_unregister_hotkeys(data);
}

static void *na_create(obs_data_t *settings, obs_source_t *source)
{
	struct ntsc_audio *f = bzalloc(sizeof(struct ntsc_audio));
	f->source = source;
	/* Registered on the parent in na_filter_add; 0 is a valid hotkey id,
	 * so the zeroed struct must not be left as-is. */
	f->degauss_hotkey = OBS_INVALID_HOTKEY_ID;
	f->power_hotkey = OBS_INVALID_HOTKEY_ID;
	f->sample_rate = 48000.0;
	for (int c = 0; c < NA_MAX_CH; c++)
		f->prng[c] = 0x9e3779b9u ^ (uint32_t)(c * 2654435761u + 12345u);
	na_update(f, settings);
	/* Start already settled: no fade, and no replay of a degauss that was
	 * fired in an earlier session and only survives as a saved counter. */
	f->master = f->powered ? 1.0f : 0.0f;
	f->degauss = 0.0f;
	return f;
}

static void na_destroy(void *data)
{
	struct ntsc_audio *f = data;
	na_unregister_hotkeys(f);
	bfree(f);
}

static struct obs_audio_data *na_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct ntsc_audio *f = data;
	const uint32_t frames = audio->frames;
	if (frames == 0)
		return audio;

	int nch = 0;
	while (nch < NA_MAX_CH && audio->data[nch])
		nch++;
	if (nch == 0)
		return audio;

	const float duck = smoothstep01(0.0f, 0.35f, f->field_strength);
	const float noise_scale = 1.0f - 0.88f * duck; /* full hiss -> 12% with picture */
	const float vol = f->volume;
	const float noise_gain = noise_scale * vol * 0.6f;
	const float ic_gain = (f->intercarrier + duck * 0.5f) * 0.08f * vol;
	const float target = f->powered ? 1.0f : 0.0f;

	const float a = f->a_deemph;
	const float alpha = f->alpha_lp;
	const float gcoef = f->g_ramp;
	const double dg_dphase = f->dg_dphase;
	const float dg_decay = f->dg_decay;

	for (uint32_t i = 0; i < frames; i++) {
		float ic = (float)sin(f->ic_phase);
		f->ic_phase += f->ic_dphase;
		if (f->ic_phase > NA_TWO_PI)
			f->ic_phase -= NA_TWO_PI;

		float boing = 0.0f;
		if (f->degauss > 0.0f) {
			float env = f->degauss * f->degauss;
			boing = (float)(sin(f->dg_phase) * 0.6 + sin(f->dg_phase * 2.0) * 0.3) * env * vol;
			f->dg_phase += dg_dphase;
			if (f->dg_phase > NA_TWO_PI)
				f->dg_phase -= NA_TWO_PI;
			f->degauss -= dg_decay;
			if (f->degauss < 0.0f)
				f->degauss = 0.0f;
		}

		f->master += (target - f->master) * gcoef;
		float m = f->master;

		for (int c = 0; c < nch; c++) {
			float *buf = (float *)audio->data[c];
			float src = buf[i];

			/* FM triangular noise: white -> de-emphasis high-pass -> LPF */
			float g = gaussf(&f->prng[c]);
			float hp = a * g - a * f->hp_x1[c] + a * f->hp_y1[c];
			f->hp_x1[c] = g;
			f->hp_y1[c] = hp;
			f->lp_y1[c] += alpha * (hp - f->lp_y1[c]);
			float hiss = f->lp_y1[c] * 0.25f;

			float out = src * duck + hiss * noise_gain + ic * ic_gain + boing;
			buf[i] = out * m;
		}
	}

	return audio;
}

/* ---- properties ------------------------------------------------------ */

static obs_properties_t *na_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_text(p, "hint", obs_module_text("CompositeTV.Hint"), OBS_TEXT_INFO);
	obs_properties_add_bool(p, "power", obs_module_text("CompositeTV.Power"));
	obs_properties_add_float_slider(p, "field_strength", obs_module_text("CompositeTV.FieldStrength"), 0.0, 1.0,
					0.01);
	obs_properties_add_float_slider(p, "volume", obs_module_text("CompositeTV.Volume"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "intercarrier", obs_module_text("CompositeTV.Intercarrier"), 0.0, 0.25, 0.01);
	return p;
}

static void na_defaults(obs_data_t *s)
{
	obs_data_set_default_bool(s, "power", true);
	obs_data_set_default_double(s, "field_strength", 1.0);
	obs_data_set_default_double(s, "volume", 0.35);
	obs_data_set_default_double(s, "intercarrier", 0.0);
}

struct obs_source_info composite_tv_audio_filter_info = {
	.id = "composite_tv_audio",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = na_get_name,
	.create = na_create,
	.destroy = na_destroy,
	.update = na_update,
	.filter_audio = na_filter_audio,
	.get_properties = na_get_properties,
	.get_defaults = na_defaults,
	.filter_add = na_filter_add,
	.filter_remove = na_filter_remove,
};
