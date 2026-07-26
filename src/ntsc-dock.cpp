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
#include <util/config-file.h>

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

/* Where the chosen targets are remembered between runs. */
static const char *CFG_SECTION = "NTSCSnow";
static const char *CFG_VIDEO = "VideoSource";
static const char *CFG_AUDIO = "AudioSource";

/* Localised UI string as a QString. */
static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static QString load_choice(const char *key)
{
	config_t *cfg = obs_frontend_get_user_config();
	if (!cfg)
		return QString();
	const char *v = config_get_string(cfg, CFG_SECTION, key);
	return v ? QString::fromUtf8(v) : QString();
}

static void save_choice(const char *key, const QString &name)
{
	config_t *cfg = obs_frontend_get_user_config();
	if (!cfg)
		return;
	config_set_string(cfg, CFG_SECTION, key, name.toUtf8().constData());
	config_save(cfg);
}

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
static void populate(QComboBox *combo, const char *id, const QString &remembered)
{
	QString current = combo->currentText();
	combo->blockSignals(true);
	combo->clear();

	struct add_ctx ctx{combo, id};
	auto collect = [](void *param, obs_source_t *src) -> bool {
		add_if_filtered(static_cast<add_ctx *>(param), src);
		return true;
	};
	obs_enum_all_sources(collect, &ctx);
	obs_enum_scenes(collect, &ctx);

	/* Keep what the user was on; failing that, fall back to the target
	 * remembered from a previous run. At start-up the combo is empty and no
	 * sources exist yet, so the remembered name is the only thing to go on. */
	int idx = combo->findText(current);
	if (idx < 0)
		idx = combo->findText(remembered);
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

/* Read the settings of the given-id filter of the named source. */
template<typename F> static void read_settings(const QString &name, const char *id, F fn)
{
	with_filter(name, id, [&](obs_source_t *filter) {
		obs_data_t *s = obs_source_get_settings(filter);
		fn(s);
		obs_data_release(s);
	});
}

/* Mutate the settings of the given-id filter and push them back to the source. */
template<typename F> static void edit_settings(const QString &name, const char *id, F fn)
{
	with_filter(name, id, [&](obs_source_t *filter) {
		obs_data_t *s = obs_source_get_settings(filter);
		fn(s);
		obs_source_update(filter, s);
		obs_data_release(s);
	});
}

static bool get_filter_bool(const QString &name, const char *id, const char *key, bool fallback)
{
	bool result = fallback;
	read_settings(name, id, [&](obs_data_t *s) { result = obs_data_get_bool(s, key); });
	return result;
}

static void set_filter_double(const QString &name, const char *id, const char *key, double value)
{
	edit_settings(name, id, [&](obs_data_t *s) { obs_data_set_double(s, key, value); });
}

static void set_filter_bool(const QString &name, const char *id, const char *key, bool value)
{
	edit_settings(name, id, [&](obs_data_t *s) { obs_data_set_bool(s, key, value); });
}

/* Bump an int setting - how the dock fires a momentary effect. */
static void bump_filter_int(const QString &name, const char *id, const char *key)
{
	edit_settings(name, id, [&](obs_data_t *s) { obs_data_set_int(s, key, obs_data_get_int(s, key) + 1); });
}

/* There is only ever one dock, and the frontend event callback below is a plain
 * C function that has to reach its widgets, so they live here. */
static QComboBox *g_vid_combo = nullptr;
static QComboBox *g_aud_combo = nullptr;
static QSlider *g_slider = nullptr;
static QPushButton *g_power = nullptr;

/* Reflect the video target's stored values in the widgets. */
static void sync_widgets()
{
	if (!g_vid_combo)
		return;
	read_settings(g_vid_combo->currentText(), FILTER_ID, [&](obs_data_t *s) {
		g_slider->blockSignals(true);
		g_slider->setValue((int)(obs_data_get_double(s, "field_strength") * 100.0 + 0.5));
		g_slider->blockSignals(false);
		g_power->setText(T(obs_data_get_bool(s, "power") ? "NTSCSnow.Dock.PowerOff"
								: "NTSCSnow.Dock.PowerOn"));
	});
}

/* Rebuild both target lists, restoring the sources remembered last run. */
static void refill_targets()
{
	if (!g_vid_combo)
		return;
	populate(g_vid_combo, FILTER_ID, load_choice(CFG_VIDEO));
	populate(g_aud_combo, AUDIO_FILTER_ID, load_choice(CFG_AUDIO));
	sync_widgets();
}

/* Sources do not exist yet while modules load, so the lists can only be filled
 * once OBS has finished loading the scene collection - and again whenever the
 * collection is swapped out. */
static void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING ||
	    event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED)
		refill_targets();
}

void ntsc_dock_register(void)
{
	QWidget *root = new QWidget();
	QVBoxLayout *layout = new QVBoxLayout(root);

	/* video target selector */
	QHBoxLayout *vid_row = new QHBoxLayout();
	vid_row->addWidget(new QLabel(T("NTSCSnow.Dock.Source")));
	QComboBox *vid_combo = new QComboBox();
	vid_row->addWidget(vid_combo, 1);
	layout->addLayout(vid_row);

	/* audio target selector */
	QHBoxLayout *aud_row = new QHBoxLayout();
	aud_row->addWidget(new QLabel(T("NTSCSnow.Dock.AudioSource")));
	QComboBox *aud_combo = new QComboBox();
	QPushButton *refresh = new QPushButton(T("NTSCSnow.Dock.Refresh"));
	aud_row->addWidget(aud_combo, 1);
	aud_row->addWidget(refresh);
	layout->addLayout(aud_row);

	/* power button (drives both picture and sound) */
	QPushButton *power = new QPushButton(T("NTSCSnow.Dock.PowerOff"));
	layout->addWidget(power);

	/* field-strength slider (drives both picture and sound) */
	layout->addWidget(new QLabel(T("NTSCSnow.FieldStrength")));
	QSlider *slider = new QSlider(Qt::Horizontal);
	slider->setRange(0, 100);
	slider->setValue(100);
	layout->addWidget(slider);

	/* momentary glitch burst */
	QPushButton *glitch = new QPushButton(T("NTSCSnow.Dock.Glitch"));
	layout->addWidget(glitch);

	/* degauss coil (picture ripple + boing) */
	QPushButton *degauss = new QPushButton(T("NTSCSnow.Dock.Degauss"));
	layout->addWidget(degauss);
	layout->addStretch(1);

	g_vid_combo = vid_combo;
	g_aud_combo = aud_combo;
	g_slider = slider;
	g_power = power;

	QObject::connect(refresh, &QPushButton::clicked, refill_targets);

	/* Remember the picks so the dock comes back usable after a restart. */
	QObject::connect(vid_combo, &QComboBox::currentTextChanged, [](const QString &name) {
		save_choice(CFG_VIDEO, name);
		sync_widgets();
	});
	QObject::connect(aud_combo, &QComboBox::currentTextChanged,
			 [](const QString &name) { save_choice(CFG_AUDIO, name); });

	QObject::connect(slider, &QSlider::valueChanged, [vid_combo, aud_combo](int val) {
		double fs = val / 100.0;
		set_filter_double(vid_combo->currentText(), FILTER_ID, "field_strength", fs);
		set_filter_double(aud_combo->currentText(), AUDIO_FILTER_ID, "field_strength", fs);
	});

	QObject::connect(power, &QPushButton::clicked, [vid_combo, aud_combo]() {
		bool next = !get_filter_bool(vid_combo->currentText(), FILTER_ID, "power", true);
		set_filter_bool(vid_combo->currentText(), FILTER_ID, "power", next);
		set_filter_bool(aud_combo->currentText(), AUDIO_FILTER_ID, "power", next);
		sync_widgets(); /* re-reads what we just wrote, so the label cannot drift */
	});

	QObject::connect(glitch, &QPushButton::clicked,
			 [vid_combo]() { bump_filter_int(vid_combo->currentText(), FILTER_ID, "glitch_pulse"); });

	/* Degauss drives picture and sound together. */
	QObject::connect(degauss, &QPushButton::clicked, [vid_combo, aud_combo]() {
		bump_filter_int(vid_combo->currentText(), FILTER_ID, "degauss_pulse");
		bump_filter_int(aud_combo->currentText(), AUDIO_FILTER_ID, "degauss_pulse");
	});

	refill_targets();
	obs_frontend_add_event_callback(frontend_event, nullptr);

	obs_frontend_add_dock_by_id("ntsc_snow_dock", obs_module_text("NTSCSnow.Dock"), root);
}

void ntsc_dock_unregister(void)
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	g_vid_combo = nullptr;
	g_aud_combo = nullptr;
	g_slider = nullptr;
	g_power = nullptr;
}
