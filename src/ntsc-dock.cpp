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
static const char *AUDIO_FILTER_ID = "ntsc_snow_audio";

/* Return the first filter with the given id on a source (borrowed reference,
 * valid only while the parent source reference is held). */
static obs_source_t *find_filter(obs_source_t *src, const char *id)
{
	if (!src)
		return nullptr;

	struct find_ctx {
		const char *id;
		obs_source_t *found;
	} ctx{id, nullptr};

	obs_source_enum_filters(
		src,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			auto *c = static_cast<find_ctx *>(param);
			if (!c->found && strcmp(obs_source_get_id(filter), c->id) == 0)
				c->found = filter;
		},
		&ctx);

	return ctx.found;
}

struct add_ctx {
	QComboBox *combo;
	const char *id;
};

/* Add a source to the combo if it carries the given filter id. */
static void add_if_filtered(struct add_ctx *a, obs_source_t *src)
{
	if (!find_filter(src, a->id))
		return;
	QString name = QString::fromUtf8(obs_source_get_name(src));
	if (a->combo->findText(name) < 0)
		a->combo->addItem(name);
}

/* Fill the combo box with every source or scene that has the given filter id.
 * Inputs come from obs_enum_all_sources; scenes are added via obs_enum_scenes
 * so a filter applied to a whole scene (the full composited picture) is listed
 * regardless of how scenes are stored internally. */
static void populate(QComboBox *combo, const char *id)
{
	QString current = combo->currentText();
	combo->blockSignals(true);
	combo->clear();

	struct add_ctx ctx{combo, id};
	obs_enum_all_sources(
		[](void *param, obs_source_t *src) -> bool {
			add_if_filtered(static_cast<add_ctx *>(param), src);
			return true;
		},
		&ctx);
	obs_enum_scenes(
		[](void *param, obs_source_t *src) -> bool {
			add_if_filtered(static_cast<add_ctx *>(param), src);
			return true;
		},
		&ctx);

	int idx = combo->findText(current);
	if (idx >= 0)
		combo->setCurrentIndex(idx);
	combo->blockSignals(false);
}

/* Invoke fn(filter) for the given-id filter of the named source. */
template<typename F> static void with_filter(const QString &name, const char *id, F fn)
{
	obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
	if (!src)
		return;
	obs_source_t *filter = find_filter(src, id);
	if (filter)
		fn(filter);
	obs_source_release(src);
}

/* Read a bool setting from the given-id filter of the named source. */
static bool get_filter_bool(const QString &name, const char *id, const char *key, bool fallback)
{
	bool result = fallback;
	with_filter(name, id, [&](obs_source_t *filter) {
		obs_data_t *s = obs_source_get_settings(filter);
		result = obs_data_get_bool(s, key);
		obs_data_release(s);
	});
	return result;
}

/* Set a double setting on the given-id filter of the named source. */
static void set_filter_double(const QString &name, const char *id, const char *key, double value)
{
	with_filter(name, id, [&](obs_source_t *filter) {
		obs_data_t *s = obs_source_get_settings(filter);
		obs_data_set_double(s, key, value);
		obs_source_update(filter, s);
		obs_data_release(s);
	});
}

/* Set a bool setting on the given-id filter of the named source. */
static void set_filter_bool(const QString &name, const char *id, const char *key, bool value)
{
	with_filter(name, id, [&](obs_source_t *filter) {
		obs_data_t *s = obs_source_get_settings(filter);
		obs_data_set_bool(s, key, value);
		obs_source_update(filter, s);
		obs_data_release(s);
	});
}

void ntsc_dock_register(void)
{
	QWidget *root = new QWidget();
	QVBoxLayout *layout = new QVBoxLayout(root);

	/* video target selector */
	QHBoxLayout *vid_row = new QHBoxLayout();
	vid_row->addWidget(new QLabel(QString::fromUtf8(obs_module_text("NTSCSnow.Dock.Source"))));
	QComboBox *vid_combo = new QComboBox();
	vid_row->addWidget(vid_combo, 1);
	layout->addLayout(vid_row);

	/* audio target selector */
	QHBoxLayout *aud_row = new QHBoxLayout();
	aud_row->addWidget(new QLabel(QString::fromUtf8(obs_module_text("NTSCSnow.Dock.AudioSource"))));
	QComboBox *aud_combo = new QComboBox();
	QPushButton *refresh = new QPushButton(QString::fromUtf8(obs_module_text("NTSCSnow.Dock.Refresh")));
	aud_row->addWidget(aud_combo, 1);
	aud_row->addWidget(refresh);
	layout->addLayout(aud_row);

	/* power button (drives both picture and sound) */
	QPushButton *power = new QPushButton(QString::fromUtf8(obs_module_text("NTSCSnow.Dock.PowerOff")));
	layout->addWidget(power);

	/* field-strength slider (drives both picture and sound) */
	layout->addWidget(new QLabel(QString::fromUtf8(obs_module_text("NTSCSnow.FieldStrength"))));
	QSlider *slider = new QSlider(Qt::Horizontal);
	slider->setRange(0, 100);
	slider->setValue(100);
	layout->addWidget(slider);
	layout->addStretch(1);

	populate(vid_combo, FILTER_ID);
	populate(aud_combo, AUDIO_FILTER_ID);

	/* reflect the video target's current values in the widgets */
	auto sync = [vid_combo, slider, power]() {
		with_filter(vid_combo->currentText(), FILTER_ID, [&](obs_source_t *filter) {
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

	QObject::connect(refresh, &QPushButton::clicked, [vid_combo, aud_combo, sync]() {
		populate(vid_combo, FILTER_ID);
		populate(aud_combo, AUDIO_FILTER_ID);
		sync();
	});
	QObject::connect(vid_combo, &QComboBox::currentTextChanged, [sync](const QString &) { sync(); });

	QObject::connect(slider, &QSlider::valueChanged, [vid_combo, aud_combo](int val) {
		double fs = val / 100.0;
		set_filter_double(vid_combo->currentText(), FILTER_ID, "field_strength", fs);
		set_filter_double(aud_combo->currentText(), AUDIO_FILTER_ID, "field_strength", fs);
	});

	QObject::connect(power, &QPushButton::clicked, [vid_combo, aud_combo, power]() {
		bool next = !get_filter_bool(vid_combo->currentText(), FILTER_ID, "power", true);
		set_filter_bool(vid_combo->currentText(), FILTER_ID, "power", next);
		set_filter_bool(aud_combo->currentText(), AUDIO_FILTER_ID, "power", next);
		power->setText(QString::fromUtf8(
			obs_module_text(next ? "NTSCSnow.Dock.PowerOff" : "NTSCSnow.Dock.PowerOn")));
	});

	sync();

	obs_frontend_add_dock_by_id("ntsc_snow_dock", obs_module_text("NTSCSnow.Dock"), root);
}
