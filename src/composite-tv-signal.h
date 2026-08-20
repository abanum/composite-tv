/*
Composite TV — companion signal source
Copyright (C) 2026

The filter publishes its packed composite raster here every frame; the
"Composite TV Signal" source displays whichever publisher the user picked.
One filter instance therefore carries one set of settings while showing the
CRT picture AND handing the raw signal to a Spout Filter at the same time.

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

#pragma once

#include <obs-module.h>

extern struct obs_source_info composite_tv_signal_info;

void ctv_signal_registry_init(void);
void ctv_signal_registry_free(void);

/* Called by the filter once per rendered frame with its packed raster. The
 * entry is keyed by `owner` (the filter instance) and labelled from the
 * filter's parent, so publishing before the filter is attached is a no-op. */
void ctv_signal_publish(void *owner, obs_source_t *filter_source, gs_texture_t *tex);

/* Called from the filter's destroy, before its textures are freed. */
void ctv_signal_unpublish(void *owner);
