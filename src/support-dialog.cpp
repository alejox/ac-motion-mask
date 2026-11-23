/*
AC Motion Mask - "Invítame un café" (same as AC Multichat)
Copyright (C) 2026 Alejandro Grajales (alejoxcode)
SPDX-License-Identifier: GPL-2.0-or-later

Un mensaje breve con el enlace a Ko-fi. Solo se abre cuando el usuario lo pide (el enlace al pie
del panel AC Motion o el botón al final de las propiedades del filtro); nunca aparece solo.
*/

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QDesktopServices>
#include <QDialog>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

const char *const kKofiUrl = "https://ko-fi.com/alejoxcode";

inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

} // namespace

void acm_open_support_dialog_qt(QWidget *parent)
{
	QDialog dialog(parent);
	dialog.setWindowTitle(T("Support.Link"));
	dialog.setMinimumWidth(380);
	auto *layout = new QVBoxLayout(&dialog);
	layout->setContentsMargins(22, 20, 22, 18);
	layout->setSpacing(12);

	auto *title = new QLabel(QStringLiteral("☕  ") + T("Support.Title"), &dialog);
	QFont titleFont = title->font();
	titleFont.setBold(true);
	titleFont.setPointSizeF(titleFont.pointSizeF() * 1.3);
	title->setFont(titleFont);
	title->setWordWrap(true);
	layout->addWidget(title);

	for (const char *key : {"Support.Free", "Support.Why", "Support.NoPressure"}) {
		auto *text = new QLabel(T(key), &dialog);
		text->setWordWrap(true);
		layout->addWidget(text);
	}
	layout->addSpacing(4);

	auto *kofi = new QPushButton(T("Support.Kofi"), &dialog);
	kofi->setDefault(true);
	kofi->setCursor(Qt::PointingHandCursor);
	kofi->setMinimumHeight(34);
	kofi->setStyleSheet("QPushButton { background-color: palette(highlight); color: palette(highlighted-text); "
			    "font-weight: bold; border: none; border-radius: 6px; padding: 6px 12px; }");
	auto *later = new QPushButton(T("Support.Continue"), &dialog);
	later->setMinimumHeight(34);
	layout->addWidget(kofi);
	layout->addWidget(later);

	QObject::connect(kofi, &QPushButton::clicked, &dialog, [&dialog] {
		QDesktopServices::openUrl(QUrl(QString::fromLatin1(kKofiUrl)));
		dialog.accept();
	});
	QObject::connect(later, &QPushButton::clicked, &dialog, &QDialog::reject);
	dialog.exec();
}

/* For the filter (C): opens over the main window. */
extern "C" void acm_open_support_dialog(void)
{
	acm_open_support_dialog_qt(static_cast<QMainWindow *>(obs_frontend_get_main_window()));
}

/* Yes/No question over the main window, for the filter (C). */
extern "C" bool acm_confirm(const char *text)
{
	auto *parent = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	return QMessageBox::question(parent, QStringLiteral("AC Motion Mask"), QString::fromUtf8(text),
				     QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
}
