/*
AC Motion Mask - "AC Motion" dock
Copyright (C) 2026 Alejandro Grajales (alejoxcode)

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

#include "motion-dock.hpp"
#include "updater.hpp"

extern "C" {
#include "motion-engine.h"
}

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <cstring>

#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#define POSITION_HOTKEYS 9

void acm_open_support_dialog_qt(QWidget *parent);

static inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static MotionDock *dock = nullptr;
static obs_hotkey_id hotkey_next = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id hotkey_prev = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id hotkey_pos[POSITION_HOTKEYS];

/* ------------------------------------------------------------------ */
/* scene helpers                                                       */

/* Scene being arranged: the preview in Studio Mode, otherwise the current
 * scene. Returns a new reference. */
static obs_source_t *editing_scene()
{
	if (obs_frontend_preview_program_mode_active())
		return obs_frontend_get_current_preview_scene();
	return obs_frontend_get_current_scene();
}

struct selected_search {
	obs_sceneitem_t *found;
};

static bool find_selected(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	auto *search = static_cast<selected_search *>(param);

	if (obs_sceneitem_selected(item)) {
		search->found = item;
		return false;
	}
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, find_selected, param);
		if (search->found)
			return false;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* AC Motion Mask filter: its mask travels with each saved position    */

#define MOTION_FILTER_ID "ac_motion_mask_filter"

static const char *const mask_keys[] = {
	"mask_type",     "mask_width", "mask_height", "mask_x",     "mask_y",  "corner_radius", "corner_individual",
	"corner_tl",     "corner_tr",  "corner_br",   "corner_bl",  "feather", "invert",        "image_path",
	"image_channel", "grad_angle", "grad_pos",    "grad_width",
};

struct filter_search {
	obs_source_t *found;
};

static void find_motion_filter_cb(obs_source_t *, obs_source_t *filter, void *param)
{
	auto *search = static_cast<filter_search *>(param);
	if (!search->found && strcmp(obs_source_get_unversioned_id(filter), MOTION_FILTER_ID) == 0)
		search->found = obs_source_get_ref(filter);
}

/* First AC Motion Mask filter on the source (new reference), or nullptr. */
static obs_source_t *find_motion_filter(obs_source_t *source)
{
	filter_search search = {nullptr};
	if (source)
		obs_source_enum_filters(source, find_motion_filter_cb, &search);
	return search.found;
}

/* Copies the filter's current mask into the saved position. */
static void save_mask(obs_sceneitem_t *item, obs_data_t *pos)
{
	obs_source_t *filter = find_motion_filter(obs_sceneitem_get_source(item));
	if (!filter) {
		obs_data_erase(pos, "mask");
		return;
	}
	obs_data_t *settings = obs_source_get_settings(filter);
	obs_data_t *mask = obs_data_create();
	for (const char *key : mask_keys) {
		obs_data_item_t *value = obs_data_item_byname(settings, key);
		if (value) {
			obs_data_item_release(&value);
			if (strcmp(key, "image_path") == 0)
				obs_data_set_string(mask, key, obs_data_get_string(settings, key));
			else if (strcmp(key, "corner_individual") == 0 || strcmp(key, "invert") == 0)
				obs_data_set_bool(mask, key, obs_data_get_bool(settings, key));
			else if (strcmp(key, "mask_type") == 0 || strcmp(key, "image_channel") == 0)
				obs_data_set_int(mask, key, obs_data_get_int(settings, key));
			else
				obs_data_set_double(mask, key, obs_data_get_double(settings, key));
		} else {
			/* never changed from its default: store the default so it is restored too */
			if (strcmp(key, "image_path") == 0)
				obs_data_set_string(mask, key, obs_data_get_default_string(settings, key));
			else if (strcmp(key, "corner_individual") == 0 || strcmp(key, "invert") == 0)
				obs_data_set_bool(mask, key, obs_data_get_default_bool(settings, key));
			else if (strcmp(key, "mask_type") == 0 || strcmp(key, "image_channel") == 0)
				obs_data_set_int(mask, key, obs_data_get_default_int(settings, key));
			else
				obs_data_set_double(mask, key, obs_data_get_default_double(settings, key));
		}
	}
	obs_data_set_obj(pos, "mask", mask);
	obs_data_release(mask);
	obs_data_release(settings);
	obs_source_release(filter);
}

/* Morphs the source's mask to the one saved with the position. */
static void apply_mask(obs_source_t *source, obs_data_t *pos, float seconds, int easing)
{
	obs_data_t *mask = obs_data_get_obj(pos, "mask");
	obs_source_t *filter = mask ? find_motion_filter(source) : nullptr;
	if (filter) {
		calldata_t cd;
		calldata_init(&cd);
		calldata_set_float(&cd, "duration", seconds);
		calldata_set_int(&cd, "easing", easing);
		proc_handler_call(obs_source_get_proc_handler(filter), "acm_begin_morph", &cd);
		calldata_free(&cd);
		obs_source_update(filter, mask);
	}
	obs_source_release(filter);
	obs_data_release(mask);
}

/* ------------------------------------------------------------------ */
/* dock                                                                */

MotionDock::MotionDock(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);

	/* header: target source + lock */
	auto *header = new QHBoxLayout();
	sourceLabel = new QLabel(T("Dock.NoSource"));
	QFont bold = sourceLabel->font();
	bold.setBold(true);
	sourceLabel->setFont(bold);
	sourceLabel->setWordWrap(true);
	lockButton = new QToolButton();
	lockButton->setCheckable(true);
	lockButton->setText(T("Dock.Lock"));
	lockButton->setToolTip(T("Dock.Lock.Tip"));
	header->addWidget(sourceLabel, 1);
	header->addWidget(lockButton);
	layout->addLayout(header);

	/* update notice (hidden until a newer release is found) */
	updateBanner = new QWidget();
	auto *bannerLayout = new QHBoxLayout(updateBanner);
	bannerLayout->setContentsMargins(8, 6, 8, 6);
	updateText = new QLabel();
	updateText->setWordWrap(true);
	auto *updateNow = new QPushButton(T("Update.Now"));
	bannerLayout->addWidget(updateText, 1);
	bannerLayout->addWidget(updateNow);
	updateBanner->setStyleSheet(
		QStringLiteral("QWidget#acmUpdate { border: 1px solid palette(highlight); border-radius: 6px; }"));
	updateBanner->setObjectName(QStringLiteral("acmUpdate"));
	updateBanner->setVisible(false);
	layout->addWidget(updateBanner);
	connect(updateNow, &QPushButton::clicked, this, [] { acm::Updater::instance().update(); });
	connect(&acm::Updater::instance(), &acm::Updater::updateAvailable, this, [this](const acm::Release &release) {
		updateText->setText(T("Update.Available").arg(release.version));
		updateBanner->setVisible(true);
	});

	/* saved positions */
	list = new QListWidget();
	list->setMinimumHeight(110);
	list->setEditTriggers(QAbstractItemView::EditKeyPressed);
	list->setContextMenuPolicy(Qt::CustomContextMenu);
	list->setToolTip(T("Dock.List.Tip"));
	layout->addWidget(list, 1);

	emptyHint = new QLabel(T("Dock.Empty"));
	emptyHint->setWordWrap(true);
	emptyHint->setEnabled(false);
	layout->addWidget(emptyHint);

	auto *editRow = new QHBoxLayout();
	saveButton = new QPushButton(T("Dock.Save"));
	saveButton->setToolTip(T("Dock.Save.Tip"));
	updateButton = new QPushButton(T("Dock.Update"));
	updateButton->setToolTip(T("Dock.Update.Tip"));
	deleteButton = new QPushButton(T("Dock.Delete"));
	editRow->addWidget(saveButton, 2);
	editRow->addWidget(updateButton, 1);
	editRow->addWidget(deleteButton, 1);
	layout->addLayout(editRow);

	/* move */
	auto *moveRow = new QHBoxLayout();
	prevButton = new QPushButton(QStringLiteral("◀"));
	prevButton->setToolTip(T("Dock.Previous"));
	goButton = new QPushButton(T("Dock.Go"));
	goButton->setDefault(true);
	nextButton = new QPushButton(QStringLiteral("▶"));
	nextButton->setToolTip(T("Dock.Next"));
	moveRow->addWidget(prevButton);
	moveRow->addWidget(goButton, 1);
	moveRow->addWidget(nextButton);
	layout->addLayout(moveRow);

	/* motion settings */
	auto *form = new QFormLayout();
	duration = new QSpinBox();
	duration->setRange(0, 10000);
	duration->setSingleStep(50);
	duration->setValue(700);
	duration->setSuffix(QStringLiteral(" ms"));
	easing = new QComboBox();
	easing->addItem(T("Easing.Out"), ACM_EASE_OUT);
	easing->addItem(T("Easing.InOut"), ACM_EASE_IN_OUT);
	easing->addItem(T("Easing.Back"), ACM_EASE_BACK);
	easing->addItem(T("Easing.Elastic"), ACM_EASE_ELASTIC);
	easing->addItem(T("Easing.Bounce"), ACM_EASE_BOUNCE);
	easing->addItem(T("Easing.Linear"), ACM_EASE_LINEAR);
	form->addRow(T("Anim.Duration"), duration);
	form->addRow(T("Anim.Easing"), easing);
	layout->addLayout(form);

	status = new QLabel();
	status->setWordWrap(true);
	layout->addWidget(status);

	/* Ko-fi */
	auto *kofi = new QLabel(QStringLiteral("<a href=\"#\">☕ %1</a>").arg(T("Support.Link").toHtmlEscaped()));
	kofi->setAlignment(Qt::AlignCenter);
	QFont small = kofi->font();
	small.setPointSizeF(small.pointSizeF() * 0.9);
	kofi->setFont(small);
	layout->addWidget(kofi);

	connect(lockButton, &QToolButton::toggled, this, &MotionDock::LockToggled);
	connect(saveButton, &QPushButton::clicked, this, &MotionDock::SaveNew);
	connect(updateButton, &QPushButton::clicked, this, &MotionDock::UpdateSelected);
	connect(deleteButton, &QPushButton::clicked, this, &MotionDock::DeleteSelected);
	connect(goButton, &QPushButton::clicked, this, &MotionDock::GoSelected);
	connect(prevButton, &QPushButton::clicked, this, &MotionDock::GoPrevious);
	connect(nextButton, &QPushButton::clicked, this, &MotionDock::GoNext);
	connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { GoSelected(); });
	connect(list, &QListWidget::itemActivated, this, [this](QListWidgetItem *) { GoSelected(); });
	connect(list, &QListWidget::itemChanged, this, &MotionDock::ItemRenamed);
	connect(list, &QListWidget::itemSelectionChanged, this, &MotionDock::UpdateButtons);
	connect(list, &QListWidget::customContextMenuRequested, this, &MotionDock::ShowContextMenu);
	connect(kofi, &QLabel::linkActivated, this, &MotionDock::ShowKofi);

	pollTimer = new QTimer(this);
	pollTimer->setInterval(250);
	connect(pollTimer, &QTimer::timeout, this, &MotionDock::Poll);
	pollTimer->start();

	RefreshList(true);
}

MotionDock::~MotionDock()
{
	pollTimer->stop();
}

void MotionDock::SetStatus(const QString &text, bool error)
{
	status->setText(text);
	status->setStyleSheet(error ? QStringLiteral("color: #e5534b;") : QString());
}

std::string MotionDock::SelectedSourceUuid() const
{
	obs_source_t *sceneSource = editing_scene();
	if (!sceneSource)
		return {};

	std::string uuid;
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (scene) {
		selected_search search = {nullptr};
		obs_scene_enum_items(scene, find_selected, &search);
		if (search.found)
			uuid = obs_source_get_uuid(obs_sceneitem_get_source(search.found));
	}
	obs_source_release(sceneSource);
	return uuid;
}

obs_sceneitem_t *MotionDock::FindItem(obs_source_t *sceneSource) const
{
	if (!sceneSource || targetUuid.empty())
		return nullptr;

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_source_t *target = obs_get_source_by_uuid(targetUuid.c_str());
	if (!scene || !target) {
		obs_source_release(target);
		return nullptr;
	}

	obs_sceneitem_t *item = obs_scene_find_source_recursive(scene, obs_source_get_name(target));
	obs_sceneitem_addref(item);
	obs_source_release(target);
	return item;
}

obs_sceneitem_t *MotionDock::EditingItem() const
{
	obs_source_t *sceneSource = editing_scene();
	obs_sceneitem_t *item = FindItem(sceneSource);
	obs_source_release(sceneSource);
	return item;
}

void MotionDock::SetTarget(const std::string &uuid)
{
	if (uuid == targetUuid)
		return;
	targetUuid = uuid;
	lastIndex = -1;
	SetStatus(QString());
	RefreshList(true);
}

void MotionDock::Poll()
{
	if (!lockButton->isChecked()) {
		std::string uuid = SelectedSourceUuid();
		/* clicking empty canvas keeps the last source */
		if (!uuid.empty())
			SetTarget(uuid);
	}
	RefreshList(false);
}

void MotionDock::RefreshList(bool force)
{
	/* do not rebuild while a name is being edited (the editor lives in the viewport) */
	QWidget *focus = QApplication::focusWidget();
	if (!force && focus && focus != list && list->isAncestorOf(focus))
		return;

	obs_source_t *target = targetUuid.empty() ? nullptr : obs_get_source_by_uuid(targetUuid.c_str());
	obs_sceneitem_t *item = EditingItem();

	/* header */
	QString header;
	if (!target)
		header = T("Dock.NoSource");
	else
		header = T("Dock.Source").arg(QString::fromUtf8(obs_source_get_name(target)));

	/* build a signature so the list only rebuilds when something changed */
	std::string signature = targetUuid + (item ? "|in" : "|out");
	obs_data_array_t *positions = item ? acm_positions_get(item) : nullptr;
	size_t count = positions ? obs_data_array_count(positions) : 0;
	for (size_t i = 0; i < count; i++) {
		obs_data_t *pos = obs_data_array_item(positions, i);
		signature += "|";
		signature += obs_data_get_string(pos, "name");
		obs_data_release(pos);
	}

	if (force || signature != listSignature) {
		listSignature = signature;
		int selected = list->currentRow();

		QSignalBlocker block(list);
		list->clear();
		for (size_t i = 0; i < count; i++) {
			obs_data_t *pos = obs_data_array_item(positions, i);
			auto *row = new QListWidgetItem(QString::fromUtf8(obs_data_get_string(pos, "name")));
			row->setFlags(row->flags() | Qt::ItemIsEditable);
			list->addItem(row);
			obs_data_release(pos);
		}
		if (count)
			list->setCurrentRow(selected >= 0 && selected < (int)count ? selected : 0);

		sourceLabel->setText(header);
		if (!target)
			emptyHint->setText(T("Dock.SelectHint"));
		else if (!item)
			emptyHint->setText(T("Dock.NotInScene"));
		else
			emptyHint->setText(T("Dock.Empty"));
		emptyHint->setVisible(count == 0);
		list->setVisible(count > 0);
		UpdateButtons();
	}

	obs_data_array_release(positions);
	obs_sceneitem_release(item);
	obs_source_release(target);
}

void MotionDock::UpdateButtons()
{
	bool hasSelection = list->currentRow() >= 0 && list->count() > 0;
	updateButton->setEnabled(hasSelection);
	deleteButton->setEnabled(hasSelection);
	goButton->setEnabled(hasSelection);
	prevButton->setEnabled(list->count() > 1);
	nextButton->setEnabled(list->count() > 1);
}

void MotionDock::SaveNew()
{
	obs_sceneitem_t *item = EditingItem();
	if (!item) {
		SetStatus(targetUuid.empty() ? T("Dock.SelectHint") : T("Dock.NotInScene"), true);
		return;
	}

	obs_data_array_t *positions = acm_positions_get(item);
	size_t n = obs_data_array_count(positions) + 1;
	QString name;
	do {
		name = T("Dock.DefaultName").arg(n++);
	} while (acm_positions_find(positions, name.toUtf8().constData()) >= 0);

	struct acm_transform t;
	acm_transform_from_item(item, &t);

	obs_data_t *pos = obs_data_create();
	obs_data_set_string(pos, "name", name.toUtf8().constData());
	acm_transform_to_data(&t, pos);
	save_mask(item, pos);
	obs_data_array_push_back(positions, pos);
	obs_data_release(pos);
	acm_positions_set(item, positions);

	int newRow = (int)obs_data_array_count(positions) - 1;
	obs_data_array_release(positions);
	obs_sceneitem_release(item);

	RefreshList(true);
	list->setCurrentRow(newRow);
	lastIndex = newRow;
	SetStatus(T("Dock.Saved").arg(name));
}

void MotionDock::UpdateSelected()
{
	int row = list->currentRow();
	obs_sceneitem_t *item = EditingItem();
	if (!item || row < 0) {
		obs_sceneitem_release(item);
		return;
	}

	obs_data_array_t *positions = acm_positions_get(item);
	obs_data_t *pos = obs_data_array_item(positions, (size_t)row);
	if (pos) {
		struct acm_transform t;
		acm_transform_from_item(item, &t);
		acm_transform_to_data(&t, pos);
		save_mask(item, pos);
		SetStatus(T("Dock.Updated").arg(QString::fromUtf8(obs_data_get_string(pos, "name"))));
		obs_data_release(pos);
	}
	obs_data_array_release(positions);
	obs_sceneitem_release(item);
}

void MotionDock::DeleteSelected()
{
	int row = list->currentRow();
	if (row < 0)
		return;

	QString name = list->item(row)->text();
	auto answer = QMessageBox::question(this, T("Dock.Title"), T("Dock.DeleteConfirm").arg(name),
					    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	if (answer != QMessageBox::Yes)
		return;

	obs_sceneitem_t *item = EditingItem();
	if (!item)
		return;

	obs_data_array_t *positions = acm_positions_get(item);
	obs_data_array_erase(positions, (size_t)row);
	acm_positions_set(item, positions);
	obs_data_array_release(positions);
	obs_sceneitem_release(item);

	lastIndex = -1;
	RefreshList(true);
	SetStatus(T("Dock.Deleted").arg(name));
}

void MotionDock::RenameSelected()
{
	QListWidgetItem *row = list->currentItem();
	if (row)
		list->editItem(row);
}

void MotionDock::ItemRenamed(QListWidgetItem *row)
{
	int index = list->row(row);
	obs_sceneitem_t *item = EditingItem();
	if (!item || index < 0) {
		obs_sceneitem_release(item);
		return;
	}

	obs_data_array_t *positions = acm_positions_get(item);
	obs_data_t *pos = obs_data_array_item(positions, (size_t)index);
	QString oldName = QString::fromUtf8(obs_data_get_string(pos, "name"));
	QString newName = row->text().trimmed();

	if (newName.isEmpty() || newName == oldName) {
		newName = oldName;
	} else {
		/* keep names unique: they are how scenes share a position */
		QString base = newName;
		int n = 2;
		while (acm_positions_find(positions, newName.toUtf8().constData()) >= 0)
			newName = QStringLiteral("%1 %2").arg(base).arg(n++);
		obs_data_set_string(pos, "name", newName.toUtf8().constData());
	}

	obs_data_release(pos);
	obs_data_array_release(positions);
	obs_sceneitem_release(item);

	RefreshList(true);
}

void MotionDock::ShowContextMenu(const QPoint &pos)
{
	if (!list->itemAt(pos))
		return;

	QMenu menu(this);
	menu.addAction(T("Dock.Go"), this, &MotionDock::GoSelected);
	menu.addSeparator();
	menu.addAction(T("Dock.Rename"), this, &MotionDock::RenameSelected);
	menu.addAction(T("Dock.Update"), this, &MotionDock::UpdateSelected);
	menu.addSeparator();
	menu.addAction(T("Dock.Delete"), this, &MotionDock::DeleteSelected);
	menu.exec(list->viewport()->mapToGlobal(pos));
}

void MotionDock::LockToggled(bool locked)
{
	lockButton->setText(locked ? T("Dock.Locked") : T("Dock.Lock"));
	if (!locked)
		Poll();
}

bool MotionDock::GoToName(const QString &name)
{
	QByteArray utf8 = name.toUtf8();
	float seconds = (float)duration->value() / 1000.0f;
	int ease = easing->currentData().toInt();

	/* move what the audience sees (program) and what is being arranged
	 * (preview in Studio Mode); each scene keeps its own positions */
	obs_source_t *scenes[2] = {obs_frontend_get_current_scene(), editing_scene()};
	if (scenes[1] == scenes[0]) {
		obs_source_release(scenes[1]);
		scenes[1] = nullptr;
	}

	int moved = 0;
	obs_source_t *masked = nullptr; /* both scenes may show the same source: morph its mask once */
	for (obs_source_t *sceneSource : scenes) {
		obs_sceneitem_t *item = FindItem(sceneSource);
		if (item) {
			obs_data_array_t *positions = acm_positions_get(item);
			int index = acm_positions_find(positions, utf8.constData());
			if (index >= 0) {
				obs_data_t *pos = obs_data_array_item(positions, (size_t)index);
				struct acm_transform t;
				acm_transform_from_data(pos, &t);
				acm_animate(item, &t, seconds, ease);
				obs_source_t *source = obs_sceneitem_get_source(item);
				if (source != masked) {
					apply_mask(source, pos, seconds, ease);
					masked = source;
				}
				obs_data_release(pos);
				moved++;
			}
			obs_data_array_release(positions);
			obs_sceneitem_release(item);
		}
		obs_source_release(sceneSource);
	}

	if (moved)
		SetStatus(T("Dock.Moving").arg(name));
	else
		SetStatus(T("Dock.NotInProgram").arg(name), true);
	return moved > 0;
}

void MotionDock::GoSelected()
{
	int row = list->currentRow();
	if (row < 0)
		return;
	GoToIndex(row);
}

void MotionDock::GoToIndex(int index)
{
	RefreshList(false);
	if (index < 0 || index >= list->count()) {
		SetStatus(T("Dock.NoPosition").arg(index + 1), true);
		return;
	}
	lastIndex = index;
	{
		QSignalBlocker block(list);
		list->setCurrentRow(index);
	}
	UpdateButtons();
	GoToName(list->item(index)->text());
}

void MotionDock::GoNext()
{
	RefreshList(false);
	int count = list->count();
	if (!count)
		return;
	int from = lastIndex >= 0 ? lastIndex : list->currentRow();
	GoToIndex((from + 1 + count) % count);
}

void MotionDock::GoPrevious()
{
	RefreshList(false);
	int count = list->count();
	if (!count)
		return;
	int from = lastIndex >= 0 ? lastIndex : list->currentRow();
	if (from < 0)
		from = 0;
	GoToIndex((from - 1 + count) % count);
}

void MotionDock::SaveForSource(obs_source_t *source)
{
	ShowDock();
	SetTarget(obs_source_get_uuid(source));
	SaveNew();
}

void MotionDock::ShowDock()
{
	QWidget *frame = parentWidget();
	if (frame) {
		frame->setVisible(true);
		frame->raise();
	}
	setVisible(true);
}

void MotionDock::ShowKofi()
{
	acm_open_support_dialog_qt(this);
}

void MotionDock::SaveState(obs_data_t *data) const
{
	obs_data_set_int(data, "duration", duration->value());
	obs_data_set_int(data, "easing", easing->currentData().toInt());
	obs_data_set_bool(data, "locked", lockButton->isChecked());
	obs_data_set_string(data, "target", targetUuid.c_str());
}

void MotionDock::LoadState(obs_data_t *data)
{
	obs_data_set_default_int(data, "duration", 700);
	obs_data_set_default_int(data, "easing", ACM_EASE_OUT);

	duration->setValue((int)obs_data_get_int(data, "duration"));
	int ease = easing->findData((int)obs_data_get_int(data, "easing"));
	easing->setCurrentIndex(ease >= 0 ? ease : 0);

	targetUuid = obs_data_get_string(data, "target");
	lockButton->setChecked(obs_data_get_bool(data, "locked") && !targetUuid.empty());
	lastIndex = -1;
	RefreshList(true);
}

/* ------------------------------------------------------------------ */
/* hotkeys, persistence and registration                              */

static void hotkey_next_cb(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed && dock)
		QMetaObject::invokeMethod(dock, "GoNext", Qt::QueuedConnection);
}

static void hotkey_prev_cb(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed && dock)
		QMetaObject::invokeMethod(dock, "GoPrevious", Qt::QueuedConnection);
}

static void hotkey_pos_cb(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	int index = (int)(intptr_t)data;
	if (pressed && dock)
		QMetaObject::invokeMethod(dock, "GoToIndex", Qt::QueuedConnection, Q_ARG(int, index));
}

static void save_hotkey(obs_data_t *data, const char *name, obs_hotkey_id id)
{
	obs_data_array_t *arr = obs_hotkey_save(id);
	obs_data_set_array(data, name, arr);
	obs_data_array_release(arr);
}

static void load_hotkey(obs_data_t *data, const char *name, obs_hotkey_id id)
{
	obs_data_array_t *arr = obs_data_get_array(data, name);
	obs_hotkey_load(id, arr);
	obs_data_array_release(arr);
}

static void save_callback(obs_data_t *save_data, bool saving, void *)
{
	if (!dock)
		return;

	if (saving) {
		obs_data_t *data = obs_data_create();
		dock->SaveState(data);
		save_hotkey(data, "hotkey_next", hotkey_next);
		save_hotkey(data, "hotkey_prev", hotkey_prev);
		for (int i = 0; i < POSITION_HOTKEYS; i++)
			save_hotkey(data, QStringLiteral("hotkey_pos_%1").arg(i + 1).toUtf8().constData(),
				    hotkey_pos[i]);
		obs_data_set_obj(save_data, "ac_motion", data);
		obs_data_release(data);
	} else {
		obs_data_t *data = obs_data_get_obj(save_data, "ac_motion");
		if (!data)
			data = obs_data_create();
		dock->LoadState(data);
		load_hotkey(data, "hotkey_next", hotkey_next);
		load_hotkey(data, "hotkey_prev", hotkey_prev);
		for (int i = 0; i < POSITION_HOTKEYS; i++)
			load_hotkey(data, QStringLiteral("hotkey_pos_%1").arg(i + 1).toUtf8().constData(),
				    hotkey_pos[i]);
		obs_data_release(data);
	}
}

extern "C" void acm_dock_save_for_source(obs_source_t *source)
{
	if (dock && source)
		dock->SaveForSource(source);
}

extern "C" void acm_dock_show(void)
{
	if (dock)
		dock->ShowDock();
}

extern "C" void acm_dock_load(void)
{
	acm_engine_init();

	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	dock = new MotionDock(main);
	obs_frontend_add_dock_by_id("ac_motion_dock", obs_module_text("Dock.Title"), dock);

	hotkey_next = obs_hotkey_register_frontend("ac_motion.next", obs_module_text("Hotkey.NextPosition"),
						   hotkey_next_cb, nullptr);
	hotkey_prev = obs_hotkey_register_frontend("ac_motion.prev", obs_module_text("Hotkey.PrevPosition"),
						   hotkey_prev_cb, nullptr);
	for (int i = 0; i < POSITION_HOTKEYS; i++) {
		QByteArray name = QStringLiteral("ac_motion.position_%1").arg(i + 1).toUtf8();
		QByteArray desc = T("Hotkey.GoToPosition").arg(i + 1).toUtf8();
		hotkey_pos[i] = obs_hotkey_register_frontend(name.constData(), desc.constData(), hotkey_pos_cb,
							     (void *)(intptr_t)i);
	}

	obs_frontend_add_save_callback(save_callback, nullptr);

	obs_frontend_add_tools_menu_item(
		obs_module_text("Update.CheckMenu"), [](void *) { acm::Updater::instance().check(true); }, nullptr);
	acm::Updater::instance().start();
}

extern "C" void acm_dock_unload(void)
{
	acm::Updater::instance().stop();
	obs_frontend_remove_save_callback(save_callback, nullptr);

	obs_hotkey_unregister(hotkey_next);
	obs_hotkey_unregister(hotkey_prev);
	for (int i = 0; i < POSITION_HOTKEYS; i++)
		obs_hotkey_unregister(hotkey_pos[i]);

	/* the dock widget itself is owned and destroyed by the OBS frontend */
	dock = nullptr;
	acm_engine_free();
}
