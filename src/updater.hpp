/*
AC Motion Mask - updates from the GitHub releases (same flow as AC Multichat)
Copyright (C) 2026 Alejandro Grajales (alejoxcode)
SPDX-License-Identifier: GPL-2.0-or-later

Al abrir OBS, cada 12 horas y desde Herramientas → "AC Motion Mask: buscar actualizaciones"
revisa la última Release. Si trae una versión más nueva pregunta (Actualizar ahora / Más tarde /
Omitir esta versión), mostrando sus novedades, y el panel AC Motion muestra un aviso con el botón
"Actualizar". Al aceptar la descarga con el curl del sistema, comprueba su SHA-256 (el "digest" del
asset o el SHA256SUMS de la Release; sin hash no instala), la descomprime y reemplaza el plugin
donde está instalado. El cambio se aplica al reiniciar OBS.

Después de actualizar, la primera vez que OBS abre la versión nueva muestra sus novedades.
*/

#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class QProgressDialog;

namespace acm {

struct Release {
	QString version;   // "1.2.0", sin la v
	QString url;       // archivo de este sistema, o la página de la Release si no lo trae
	QString page;      // página de la Release
	QString assetName; // nombre del archivo de este sistema
	QString sha256;    // su SHA-256 según el "digest" de GitHub, vacío si no lo trae
	QString sumsUrl;   // el SHA256SUMS de la Release, vacío si no lo trae
	QString notes;     // la descripción de la Release (Markdown)
};

// Novedades como lista HTML, desde una descripción escrita como CHANGELOG.md: "### Español" y
// "### English" con líneas "- …" debajo. Vacío si no hay. Máximo 8.
QString notesHtml(const QString &notes, bool spanish);
bool isNewerVersion(const QString &latest, const QString &current);
bool obsInSpanish();

class Updater : public QObject {
	Q_OBJECT

public:
	static Updater &instance();

	void start();
	void stop();
	// manual: también avisa si ya está al día o si falla la revisión.
	void check(bool manual);
	// Instala la versión encontrada (la del aviso del panel).
	void update();
	Release available() const { return available_; }

signals:
	void updateAvailable(const acm::Release &release);

private:
	Updater();
	void handleRelease(const QByteArray &json, bool manual);
	void askToUpdate(const Release &release);
	void download(const Release &release);
	void verify(const Release &release, const QString &archive);
	void installIfMatches(const QString &version, const QString &archive, const QString &expected);
	void install(const QString &version, const QString &archive);
	void fail(const QString &message, const QString &detail = QString());
	void showWhatsNewIfUpdated();

	Release available_;
	QString asked_;
	QTimer timer_;
	bool busy_ = false;
	bool stopped_ = false;
	QProgressDialog *progress_ = nullptr;
};

// Instaladores por sistema (updater-install.cpp).
bool installPlugin(const QString &extracted, QString *problem);
QString moduleBinary();
QString moduleDataPath();
QString workDir();

} // namespace acm
