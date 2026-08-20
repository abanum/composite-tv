/*
Plugin Name
Copyright (C) <Year> <Developer> <Email Address>

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

#include <obs-module.h>
#include <plugin-support.h>

#include "composite-tv-filter.h"
#include "composite-tv-audio.h"
#include "composite-tv-signal.h"
#include "composite-tv-dock.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

bool obs_module_load(void)
{
	ctv_signal_registry_init();
	obs_register_source(&composite_tv_filter_info);
	obs_register_source(&composite_tv_audio_filter_info);
	obs_register_source(&composite_tv_signal_info);
	composite_tv_dock_register();
	obs_log(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	composite_tv_dock_unregister();
	ctv_signal_registry_free();
	obs_log(LOG_INFO, "plugin unloaded");
}
