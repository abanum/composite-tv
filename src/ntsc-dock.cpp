/*
NTSC Snow — control dock
Copyright (C) 2026

A dockable panel that drives one source's NTSC Snow filter: a power on/off
button (triggers the CRT warm-up / collapse animation) and a field-strength
slider (clean picture .. snow). The target source is chosen from a drop-down
and can be locked to a single source.

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
#include <obs-frontend-api.h>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include <cstring>

#include "ntsc-dock.h"

static const char *FILTER_ID = "ntsc_snow_filter";

/* Return the first NTSC Snow filter on a source (borrowed reference, valid
 * only while the parent source reference is held). */
static obs_source_t *find_ntsc_filter(obs_source_t *src)
{
	if (!src)
		return nullptr;

	struct find_ctx {
		obs_source_t *found;
	} ctx{nullptr};

	obs_source_enum_filters(
		src,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			auto *c = static_cast<find_ctx *>(param);
			if (!c->found && strcmp(obs_source_get_id(filter), FILTER_ID) == 0)
				c->found = filter;
		},
		&ctx);

	return ctx.found;
}

/* Add a source to the combo if it has the filter and is not already listed. */
static void add_if_filtered(QComboBox *combo, obs_source_t *src)
{
	if (!find_ntsc_filter(src))
		return;
	QString name = QString::fromUtf8(obs_source_get_name(src));
	if (combo->findText(name) < 0)
		combo->addItem(name);
}

/* Fill the combo box with every source or scene that currently has the filter.
 * Inputs come from obs_enum_all_sources; scenes are added via obs_enum_scenes
 * so a filter applied to a whole scene (the full composited picture) is listed
 * regardless of how scenes are stored internally. */
static void populate_sources(QComboBox *combo)
{
	QString current = combo->currentText();
	combo->blockSignals(true);
	combo->clear();

	obs_enum_all_sources(
		[](void *param, obs_source_t *src) -> bool {
			add_if_filtered(static_cast<QComboBox *>(param), src);
			return true;
		},
		combo);
	obs_enum_scenes(
		[](void *param, obs_source_t *src) -> bool {
			add_if_filtered(static_cast<QComboBox *>(param), src);
			return true;
		},
		combo);

	int idx = combo->findText(current);
	if (idx >= 0)
		combo->setCurrentIndex(idx);
	combo->blockSignals(false);
}

/* Invoke fn(filter) for the filter of the named source, managing references. */
template<typename F> static void with_filter(const QString &name, F fn)
{
	obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
	if (!src)
		return;
	obs_source_t *filter = find_ntsc_filter(src);
	if (filter)
		fn(filter);
	obs_source_release(src);
}

void ntsc_dock_register(void)
{
	QWidget *root = new QWidget();
	QVBoxLayout *layout = new QVBoxLayout(root);

	/* target source selector */
	QHBoxLayout *src_row = new QHBoxLayout();
	QLabel *src_label = new QLabel(QString::fromUtf8(obs_module_text("NTSCSnow.Dock.Source")));
	QComboBox *combo = new QComboBox();
	QPushButton *refresh = new QPushButton(QString::fromUtf8(obs_module_text("NTSCSnow.Dock.Refresh")));
	src_row->addWidget(src_label);
	src_row->addWidget(combo, 1);
	src_row->addWidget(refresh);
	layout->addLayout(src_row);

	/* power button */
	QPushButton *power = new QPushButton(QString::fromUtf8(obs_module_text("NTSCSnow.Dock.PowerOff")));
	layout->addWidget(power);

	/* field-strength slider */
	QLabel *fs_label = new QLabel(QString::fromUtf8(obs_module_text("NTSCSnow.FieldStrength")));
	QSlider *slider = new QSlider(Qt::Horizontal);
	slider->setRange(0, 100);
	slider->setValue(100);
	layout->addWidget(fs_label);
	layout->addWidget(slider);
	layout->addStretch(1);

	populate_sources(combo);

	auto sync_from_filter = [combo, slider, power]() {
		with_filter(combo->currentText(), [&](obs_source_t *filter) {
			obs_data_t *s = obs_source_get_settings(filter);
			slider->blockSignals(true);
			slider->setValue((int)(obs_data_get_double(s, "field_strength") * 100.0 + 0.5));
			slider->blockSignals(false);
			bool on = obs_data_get_bool(s, "power");
			power->setText(QString::fromUtf8(
				obs_module_text(on ? "NTSCSnow.Dock.PowerOff" : "NTSCSnow.Dock.PowerOn")));
			obs_data_release(s);
		});
	};

	QObject::connect(refresh, &QPushButton::clicked, [combo, sync_from_filter]() {
		populate_sources(combo);
		sync_from_filter();
	});

	QObject::connect(combo, &QComboBox::currentTextChanged,
			 [sync_from_filter](const QString &) { sync_from_filter(); });

	QObject::connect(slider, &QSlider::valueChanged, [combo](int val) {
		with_filter(combo->currentText(), [&](obs_source_t *filter) {
			obs_data_t *s = obs_source_get_settings(filter);
			obs_data_set_double(s, "field_strength", val / 100.0);
			obs_source_update(filter, s);
			obs_data_release(s);
		});
	});

	QObject::connect(power, &QPushButton::clicked, [combo, power]() {
		with_filter(combo->currentText(), [&](obs_source_t *filter) {
			obs_data_t *s = obs_source_get_settings(filter);
			bool on = !obs_data_get_bool(s, "power");
			obs_data_set_bool(s, "power", on);
			obs_source_update(filter, s);
			obs_data_release(s);
			power->setText(QString::fromUtf8(
				obs_module_text(on ? "NTSCSnow.Dock.PowerOff" : "NTSCSnow.Dock.PowerOn")));
		});
	});

	sync_from_filter();

	obs_frontend_add_dock_by_id("ntsc_snow_dock", obs_module_text("NTSCSnow.Dock"), root);
}
