/*
Composite TV — companion signal source
Copyright (C) 2026

See composite-tv-signal.h for the idea. The registry is a small mutex-guarded
list; entries are owned by filter instances and looked up by label. Texture
pointers are only ever dereferenced inside a video render, and a filter can
only free its textures inside obs_enter_graphics(), which cannot interleave
with a render in progress - that is what makes the handoff safe.

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

#include "composite-tv-signal.h"

#include <util/darray.h>
#include <util/dstr.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <string.h>

/* ---- publisher registry ----------------------------------------------- */

struct sig_entry {
	void *owner;       /* the filter instance */
	char *label;       /* "parent / filter", what the picker shows */
	gs_texture_t *tex; /* the packed raster, owned by the filter */
	uint32_t width;
	uint32_t height;
};

static DARRAY(struct sig_entry) g_entries;
static pthread_mutex_t g_mutex;

void ctv_signal_registry_init(void)
{
	pthread_mutex_init(&g_mutex, NULL);
	da_init(g_entries);
}

void ctv_signal_registry_free(void)
{
	for (size_t i = 0; i < g_entries.num; i++)
		bfree(g_entries.array[i].label);
	da_free(g_entries);
	pthread_mutex_destroy(&g_mutex);
}

static struct sig_entry *find_by_owner(void *owner)
{
	for (size_t i = 0; i < g_entries.num; i++) {
		if (g_entries.array[i].owner == owner)
			return &g_entries.array[i];
	}
	return NULL;
}

void ctv_signal_publish(void *owner, obs_source_t *filter_source, gs_texture_t *tex)
{
	/* No parent yet means no name to publish under; try again next frame. */
	obs_source_t *parent = obs_filter_get_parent(filter_source);
	if (!parent || !tex)
		return;
	const char *pname = obs_source_get_name(parent);
	const char *fname = obs_source_get_name(filter_source);
	if (!pname || !fname)
		return;

	struct dstr label = {0};
	dstr_printf(&label, "%s / %s", pname, fname);

	pthread_mutex_lock(&g_mutex);
	struct sig_entry *e = find_by_owner(owner);
	if (!e) {
		struct sig_entry ne = {0};
		ne.owner = owner;
		e = da_push_back_new(g_entries);
		*e = ne;
	}
	/* Renames just follow along: the label is refreshed on every publish. */
	if (!e->label || strcmp(e->label, label.array) != 0) {
		bfree(e->label);
		e->label = bstrdup(label.array);
	}
	e->tex = tex;
	e->width = gs_texture_get_width(tex);
	e->height = gs_texture_get_height(tex);
	pthread_mutex_unlock(&g_mutex);

	dstr_free(&label);
}

void ctv_signal_unpublish(void *owner)
{
	pthread_mutex_lock(&g_mutex);
	for (size_t i = 0; i < g_entries.num; i++) {
		if (g_entries.array[i].owner == owner) {
			bfree(g_entries.array[i].label);
			da_erase(g_entries, i);
			break;
		}
	}
	pthread_mutex_unlock(&g_mutex);
}

/* ---- the source ------------------------------------------------------- */

struct sig_source {
	obs_source_t *source;
	char *target; /* label of the picked publisher */
};

static const char *sig_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("CompositeTV.Signal.SourceName");
}

static void sig_update(void *data, obs_data_t *s)
{
	struct sig_source *ss = data;
	bfree(ss->target);
	ss->target = bstrdup(obs_data_get_string(s, "target"));
}

static void *sig_create(obs_data_t *settings, obs_source_t *source)
{
	struct sig_source *ss = bzalloc(sizeof(struct sig_source));
	ss->source = source;
	sig_update(ss, settings);
	return ss;
}

static void sig_destroy(void *data)
{
	struct sig_source *ss = data;
	bfree(ss->target);
	bfree(ss);
}

/* Look up the picked publisher and copy out what a caller may use outside
 * the lock. The texture pointer is only valid to hand to the GPU inside the
 * current video render - see the header comment. */
static bool sig_lookup(struct sig_source *ss, gs_texture_t **tex, uint32_t *w, uint32_t *h)
{
	bool found = false;
	if (!ss->target || !*ss->target)
		return false;
	pthread_mutex_lock(&g_mutex);
	for (size_t i = 0; i < g_entries.num; i++) {
		struct sig_entry *e = &g_entries.array[i];
		if (e->label && strcmp(e->label, ss->target) == 0 && e->tex) {
			if (tex)
				*tex = e->tex;
			if (w)
				*w = e->width;
			if (h)
				*h = e->height;
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_mutex);
	return found;
}

static uint32_t sig_get_width(void *data)
{
	uint32_t w = 0;
	sig_lookup(data, NULL, &w, NULL);
	return w;
}

static uint32_t sig_get_height(void *data)
{
	uint32_t h = 0;
	sig_lookup(data, NULL, NULL, &h);
	return h;
}

static void sig_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	gs_texture_t *tex = NULL;
	uint32_t w = 0, h = 0;
	if (!sig_lookup(data, &tex, &w, &h))
		return;

	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *img = gs_effect_get_param_by_name(def, "image");
	if (img)
		gs_effect_set_texture(img, tex);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(tex, 0, w, h);
}

static obs_properties_t *sig_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();

	obs_property_t *tp = obs_properties_add_list(p, "target", obs_module_text("CompositeTV.Signal.Target"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(tp, obs_module_text("CompositeTV.Signal.Target.None"), "");
	pthread_mutex_lock(&g_mutex);
	for (size_t i = 0; i < g_entries.num; i++) {
		const char *label = g_entries.array[i].label;
		if (label)
			obs_property_list_add_string(tp, label, label);
	}
	pthread_mutex_unlock(&g_mutex);

	obs_properties_add_text(p, "hint", obs_module_text("CompositeTV.Signal.SourceHint"), OBS_TEXT_INFO);
	return p;
}

struct obs_source_info composite_tv_signal_info = {
	.id = "composite_tv_signal",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = sig_get_name,
	.create = sig_create,
	.destroy = sig_destroy,
	.update = sig_update,
	.video_render = sig_render,
	.get_width = sig_get_width,
	.get_height = sig_get_height,
	.get_properties = sig_get_properties,
};
