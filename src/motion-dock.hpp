/*
AC Motion Mask - "AC Motion" dock
Copyright (C) 2026 Alejandro Grajales (alejoxcode)
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QWidget>
#include <string>

#include <obs.h>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QToolButton;
class QSpinBox;
class QComboBox;
class QTimer;

class MotionDock : public QWidget {
	Q_OBJECT

public:
	explicit MotionDock(QWidget *parent = nullptr);
	~MotionDock() override;

	void SaveState(obs_data_t *data) const;
	void LoadState(obs_data_t *data);

public slots:
	void GoToIndex(int index);
	void GoNext();
	void GoPrevious();

private slots:
	void Poll();
	void SaveNew();
	void UpdateSelected();
	void DeleteSelected();
	void RenameSelected();
	void GoSelected();
	void ItemRenamed(QListWidgetItem *item);
	void ShowContextMenu(const QPoint &pos);
	void LockToggled(bool locked);
	void ShowKofi();

private:
	/* target item in the scene the user is arranging (preview in Studio
	 * Mode); returns a new reference or nullptr */
	obs_sceneitem_t *EditingItem() const;
	/* target item in the given scene; returns a new reference or nullptr */
	obs_sceneitem_t *FindItem(obs_source_t *sceneSource) const;
	std::string SelectedSourceUuid() const;
	void SetTarget(const std::string &uuid);
	void RefreshList(bool force);
	void UpdateButtons();
	void SetStatus(const QString &text, bool error = false);
	bool GoToName(const QString &name);

	std::string targetUuid;
	std::string listSignature;
	int lastIndex = -1;

	QLabel *sourceLabel;
	QToolButton *lockButton;
	QListWidget *list;
	QLabel *emptyHint;
	QPushButton *saveButton;
	QPushButton *updateButton;
	QPushButton *deleteButton;
	QPushButton *goButton;
	QPushButton *prevButton;
	QPushButton *nextButton;
	QSpinBox *duration;
	QComboBox *easing;
	QLabel *status;
	QTimer *pollTimer;
};

extern "C" void acm_dock_load(void);
extern "C" void acm_dock_unload(void);
