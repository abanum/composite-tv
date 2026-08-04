/*
Composite TV — property helpers
Copyright (C) 2026

Adds a control and hangs a "Default: x" tooltip on it in one call. The default
is handed in from the DEF_* macro the filter's get_defaults() uses as well, so
the value shown on hover cannot drift away from the value the Defaults button
restores. OBS renders a long description as a "?" icon beside the label (or
beside the checkbox for bools) with the text as its tooltip.

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
#include <stdio.h>

static inline void ctv_set_default_tip(obs_property_t *prop, const char *value)
{
	if (!prop || !value)
		return;
	char tip[256];
	snprintf(tip, sizeof(tip), "%s: %s", obs_module_text("CompositeTV.DefaultTip"), value);
	obs_property_set_long_description(prop, tip);
}

/* Print the default with the precision the slider can actually reach, so a
 * 0.005-step control does not advertise a default of "0.03". */
static inline int ctv_step_decimals(double step)
{
	if (step >= 1.0)
		return 0;
	if (step >= 0.1)
		return 1;
	if (step >= 0.01)
		return 2;
	return 3;
}

static inline obs_property_t *ctv_add_float_slider(obs_properties_t *p, const char *name, const char *text, double min,
						   double max, double step, double def)
{
	obs_property_t *prop = obs_properties_add_float_slider(p, name, text, min, max, step);
	char value[32];
	snprintf(value, sizeof(value), "%.*f", ctv_step_decimals(step), def);
	ctv_set_default_tip(prop, value);
	return prop;
}

static inline obs_property_t *ctv_add_bool(obs_properties_t *p, const char *name, const char *text, bool def)
{
	obs_property_t *prop = obs_properties_add_bool(p, name, text);
	ctv_set_default_tip(prop, obs_module_text(def ? "CompositeTV.On" : "CompositeTV.Off"));
	return prop;
}

/* Call once the list is populated: the tooltip quotes the entry's own label
 * rather than the raw number stored in the settings. */
static inline void ctv_set_list_default_tip(obs_property_t *prop, long long def)
{
	if (!prop)
		return;
	size_t count = obs_property_list_item_count(prop);
	for (size_t i = 0; i < count; i++) {
		if (obs_property_list_item_int(prop, i) == def) {
			ctv_set_default_tip(prop, obs_property_list_item_name(prop, i));
			return;
		}
	}
}
