/*
AC Motion Mask - updates from the GitHub releases (same flow as AC Multichat)
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

#include "updater.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

namespace acm {

namespace {

const char *const kRepo = "alejox/ac-motion-mask";
const char *const kReleasesPage = "https://github.com/alejox/ac-motion-mask/releases/latest";

#if defined(_WIN32)
const char *const kAssetSuffix = "-windows-x64.zip";
#elif defined(__APPLE__)
const char *const kAssetSuffix = "-macos.zip";
#else
const char *const kAssetSuffix = "-linux-x86_64.tar.gz";
#endif

inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QWidget *mainWindow()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

QString apiUrl(const QString &path)
{
	return QStringLiteral("https://api.github.com/repos/%1/%2").arg(QString::fromLatin1(kRepo), path);
}

QString systemTool(const char *name)
{
#if defined(_WIN32)
	// curl.exe y tar.exe vienen con Windows 10 (1803) y 11.
	return qEnvironmentVariable("SystemRoot", "C:\\Windows") + "\\System32\\" + name + ".exe";
#elif defined(__APPLE__)
	return QString("/usr/bin/") + name;
#else
	const QString found = QStandardPaths::findExecutable(QString::fromLatin1(name));
	return found.isEmpty() ? QString("/usr/bin/") + name : found;
#endif
}

// curl (y en Linux, si no está, wget). file vacío: lo descargado sale por la salida estándar.
void downloadCommand(const QString &url, const QString &file, int seconds, bool api, QString *program,
		     QStringList *args)
{
#if !defined(_WIN32) && !defined(__APPLE__)
	if (QStandardPaths::findExecutable("curl").isEmpty() && !QStandardPaths::findExecutable("wget").isEmpty()) {
		*program = systemTool("wget");
		*args = {"-q", "--timeout", QString::number(seconds), "-O", file.isEmpty() ? QString("-") : file};
		if (api)
			*args << "--header" << "Accept: application/vnd.github+json";
		*args << url;
		return;
	}
#endif
	*program = systemTool("curl");
	*args = {"-fsSL", "--max-time", QString::number(seconds)};
	if (api)
		*args << "-H" << "Accept: application/vnd.github+json";
	if (!file.isEmpty())
		*args << "-o" << file;
	*args << url;
}

QString tool(const QProcess *process)
{
	return QFileInfo(process->program()).baseName();
}

QList<int> versionParts(QString v)
{
	v = v.trimmed();
	if (v.startsWith('v') || v.startsWith('V'))
		v.remove(0, 1);
	QList<int> parts;
	for (const QString &p : v.section('-', 0, 0).split('.'))
		parts << p.toInt();
	return parts;
}

bool isSha256Hex(const QString &hex)
{
	static const QRegularExpression re(QStringLiteral("^[0-9a-fA-F]{64}$"));
	return re.match(hex).hasMatch();
}

QString sha256FromDigest(const QString &digest)
{
	const QString prefix = QStringLiteral("sha256:");
	if (!digest.startsWith(prefix, Qt::CaseInsensitive))
		return {};
	const QString hex = digest.mid(prefix.size()).trimmed();
	return isSha256Hex(hex) ? hex.toLower() : QString();
}

QString sha256FromSums(const QByteArray &sums, const QString &fileName)
{
	for (const QString &raw : QString::fromUtf8(sums).split(QLatin1Char('\n'))) {
		const QString line = raw.trimmed();
		const qsizetype space = line.indexOf(QLatin1Char(' '));
		if (space != 64 || !isSha256Hex(line.left(64)))
			continue;
		QString name = line.mid(space).trimmed();
		if (name.startsWith(QLatin1Char('*')))
			name.remove(0, 1);
		if (name == fileName)
			return line.left(64).toLower();
	}
	return {};
}

QString sha256OfFile(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return {};
	QCryptographicHash hash(QCryptographicHash::Sha256);
	if (!hash.addData(&file))
		return {};
	return QString::fromLatin1(hash.result().toHex());
}

Release parseRelease(const QJsonObject &json)
{
	Release r;
	if (json.value("draft").toBool() || json.value("prerelease").toBool())
		return r;
	r.version = json.value("tag_name").toString();
	if (r.version.startsWith('v'))
		r.version.remove(0, 1);
	r.page = json.value("html_url").toString();
	r.notes = json.value("body").toString();
	r.url = r.page;
	const QString suffix = QString::fromLatin1(kAssetSuffix);
	for (const QJsonValue a : json.value("assets").toArray()) {
		const QJsonObject asset = a.toObject();
		const QString name = asset.value("name").toString();
		if (name == QLatin1String("SHA256SUMS")) {
			r.sumsUrl = asset.value("browser_download_url").toString();
		} else if (r.assetName.isEmpty() && name.endsWith(suffix)) {
			r.url = asset.value("browser_download_url").toString();
			r.assetName = name;
			r.sha256 = sha256FromDigest(asset.value("digest").toString());
		}
	}
	return r;
}

QString configFile(const char *name)
{
	char *path = obs_module_config_path(name);
	QString result = QString::fromUtf8(path);
	bfree(path);
	return result;
}

QString readConfig(const char *name)
{
	QFile file(configFile(name));
	return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
}

void writeConfig(const char *name, const QString &value)
{
	const QString path = configFile(name);
	QDir().mkpath(QFileInfo(path).absolutePath());
	QSaveFile file(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(value.toUtf8());
		file.commit();
	}
}

} // namespace

bool obsInSpanish()
{
	return QString::fromUtf8(obs_get_locale()).startsWith(QStringLiteral("es"));
}

bool isNewerVersion(const QString &latest, const QString &current)
{
	const QList<int> a = versionParts(latest), b = versionParts(current);
	for (qsizetype i = 0; i < std::max(a.size(), b.size()); ++i) {
		const int x = a.value(i), y = b.value(i);
		if (x != y)
			return x > y;
	}
	return false;
}

QString notesHtml(const QString &notes, bool spanish)
{
	QStringList es, en, plain;
	QStringList *current = &plain;
	for (QString line : notes.split('\n')) {
		line = line.trimmed();
		if (line.startsWith('#')) {
			const QString title = line.remove(QRegularExpression(QStringLiteral("^#+\\s*"))).toLower();
			current = title.startsWith(QStringLiteral("espa"))   ? &es
				  : title.startsWith(QStringLiteral("engl")) ? &en
									     : &plain;
		} else if (line.startsWith(QStringLiteral("- ")) || line.startsWith(QStringLiteral("* "))) {
			current->append(line.mid(2).trimmed());
		}
	}
	QStringList items = spanish ? es : en;
	if (items.isEmpty())
		items = spanish ? en : es;
	if (items.isEmpty())
		items = plain;
	if (items.isEmpty())
		return {};
	QString html = QStringLiteral("<ul style=\"margin-left:14px; -qt-list-indent:0;\">");
	for (const QString &item : items.mid(0, 8)) {
		QString text = item.toHtmlEscaped();
		text.replace(QRegularExpression(QStringLiteral("\\*\\*(.+?)\\*\\*")), QStringLiteral("<b>\\1</b>"));
		html += QStringLiteral("<li>") + text + QStringLiteral("</li>");
	}
	return html + QStringLiteral("</ul>");
}

QString workDir()
{
	return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath("ac-motion-mask-update");
}

QString moduleBinary()
{
	return QDir::cleanPath(
		QFileInfo(QString::fromUtf8(obs_get_module_binary_path(obs_current_module()))).absoluteFilePath());
}

QString moduleDataPath()
{
	return QDir::cleanPath(
		QFileInfo(QString::fromUtf8(obs_get_module_data_path(obs_current_module()))).absoluteFilePath());
}

Updater &Updater::instance()
{
	static Updater *updater = new Updater(); // vive hasta que OBS cierra
	return *updater;
}

Updater::Updater()
{
	timer_.setInterval(12 * 60 * 60 * 1000);
	connect(&timer_, &QTimer::timeout, this, [this] { check(false); });
}

void Updater::start()
{
	QDir(workDir()).removeRecursively();
	// Unos segundos después de abrir OBS, para no competir con la carga de las escenas.
	QTimer::singleShot(8000, this, [this] { showWhatsNewIfUpdated(); });
	QTimer::singleShot(15000, this, [this] { check(false); });
	timer_.start();
}

void Updater::stop()
{
	stopped_ = true;
	timer_.stop();
}

// La primera vez que se abre una versión nueva, cuenta qué trae (desde su Release).
void Updater::showWhatsNewIfUpdated()
{
	const QString current = QString::fromLatin1(PLUGIN_VERSION);
	if (stopped_ || readConfig("last-version.txt") == current)
		return;
	writeConfig("last-version.txt", current);

	auto *curl = new QProcess(this);
	connect(curl, &QProcess::finished, this, [this, curl, current](int code, QProcess::ExitStatus) {
		const QByteArray json = curl->readAllStandardOutput();
		curl->deleteLater();
		if (code != 0 || stopped_)
			return;
		const QString notes = QJsonDocument::fromJson(json).object().value("body").toString();
		const QString html = notesHtml(notes, obsInSpanish());
		if (html.isEmpty())
			return;
		QMessageBox box(mainWindow());
		box.setWindowTitle(T("Update.Title"));
		box.setIcon(QMessageBox::Information);
		box.setTextFormat(Qt::RichText);
		box.setText(QStringLiteral("<b>%1</b>").arg(T("Update.WhatsNewTitle").arg(current).toHtmlEscaped()) +
			    html);
		box.addButton(T("Update.GotIt"), QMessageBox::AcceptRole);
		box.exec();
	});
	connect(curl, &QProcess::errorOccurred, curl, [curl](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart)
			curl->deleteLater();
	});
	QString program;
	QStringList args;
	downloadCommand(apiUrl(QStringLiteral("releases/tags/%1").arg(current)), QString(), 20, true, &program, &args);
	curl->start(program, args);
}

void Updater::check(bool manual)
{
	if (busy_ || stopped_)
		return;
	busy_ = true;
	auto *curl = new QProcess(this);
	connect(curl, &QProcess::finished, this, [this, curl, manual](int code, QProcess::ExitStatus) {
		const QByteArray json = curl->readAllStandardOutput();
		const QString error = QString::fromUtf8(curl->readAllStandardError()).trimmed();
		curl->deleteLater();
		busy_ = false;
		if (code != 0) {
			obs_log(LOG_INFO, "could not check for updates (%s %d)", tool(curl).toUtf8().constData(), code);
			if (manual)
				fail(T("Update.CheckFailed"),
				     QString("%1 %2: %3").arg(tool(curl), QString::number(code), error));
			return;
		}
		handleRelease(json, manual);
	});
	connect(curl, &QProcess::errorOccurred, this, [this, curl, manual](QProcess::ProcessError error) {
		if (error != QProcess::FailedToStart)
			return;
		curl->deleteLater();
		busy_ = false;
		if (manual)
			fail(T("Update.CheckFailed"), curl->errorString());
	});
	QString program;
	QStringList args;
	downloadCommand(apiUrl(QStringLiteral("releases/latest")), QString(), 20, true, &program, &args);
	curl->start(program, args);
}

void Updater::handleRelease(const QByteArray &json, bool manual)
{
	const Release release = parseRelease(QJsonDocument::fromJson(json).object());
	const QString current = QString::fromLatin1(PLUGIN_VERSION);
	if (release.version.isEmpty() || release.assetName.isEmpty()) {
		if (manual)
			fail(T("Update.CheckFailed"),
			     T("Update.Problem.NoAsset").arg(QString::fromLatin1(kAssetSuffix)));
		return;
	}
	if (!isNewerVersion(release.version, current)) {
		if (manual)
			QMessageBox::information(mainWindow(), T("Update.Title"), T("Update.UpToDate").arg(current));
		return;
	}
	obs_log(LOG_INFO, "a new version is available: %s", release.version.toUtf8().constData());
	available_ = release;
	emit updateAvailable(release);
	// Se pregunta una vez por sesión de OBS (el aviso del panel queda), salvo que se omitiera.
	if (manual || (asked_ != release.version && readConfig("skip-version.txt") != release.version))
		askToUpdate(release);
}

void Updater::askToUpdate(const Release &release)
{
	asked_ = release.version;
	QMessageBox box(mainWindow());
	box.setWindowTitle(T("Update.Title"));
	box.setIcon(QMessageBox::Information);
	box.setTextFormat(Qt::RichText);
	box.setTextInteractionFlags(Qt::TextBrowserInteraction);
	QString text =
		T("Update.Ask").arg(release.version, QString::fromLatin1(PLUGIN_VERSION), release.page.toHtmlEscaped());
	const QString notes = notesHtml(release.notes, obsInSpanish());
	if (!notes.isEmpty())
		text += QStringLiteral("<br><br><b>%1</b>").arg(T("Update.WhatsNew")) + notes;
	box.setText(text);
	QPushButton *now = box.addButton(T("Update.Now"), QMessageBox::AcceptRole);
	box.addButton(T("Update.Later"), QMessageBox::RejectRole);
	QPushButton *skip = box.addButton(T("Update.Skip"), QMessageBox::DestructiveRole);
	box.setDefaultButton(now);
	box.exec();
	if (box.clickedButton() == now)
		download(release);
	else if (box.clickedButton() == skip)
		writeConfig("skip-version.txt", release.version);
}

void Updater::update()
{
	if (!available_.version.isEmpty())
		download(available_);
}

void Updater::download(const Release &release)
{
	if (busy_)
		return;
	busy_ = true;
	const QString dir = workDir();
	QDir(dir).removeRecursively();
	QDir().mkpath(dir);
	const QString archive = QDir(dir).filePath(release.assetName);

	progress_ = new QProgressDialog(T("Update.Downloading").arg(release.version), QString(), 0, 0, mainWindow());
	progress_->setWindowTitle(T("Update.Title"));
	progress_->setWindowModality(Qt::WindowModal);
	progress_->setMinimumDuration(0);
	progress_->show();

	auto *curl = new QProcess(this);
	connect(curl, &QProcess::finished, this, [this, curl, release, archive](int code, QProcess::ExitStatus) {
		const QString error = QString::fromUtf8(curl->readAllStandardError()).trimmed();
		curl->deleteLater();
		if (code != 0 || QFileInfo(archive).size() == 0) {
			busy_ = false;
			fail(T("Update.DownloadFailed"),
			     QString("%1 %2: %3").arg(tool(curl), QString::number(code), error));
			return;
		}
		verify(release, archive);
	});
	connect(curl, &QProcess::errorOccurred, this, [this, curl](QProcess::ProcessError error) {
		if (error != QProcess::FailedToStart)
			return;
		curl->deleteLater();
		busy_ = false;
		fail(T("Update.DownloadFailed"), curl->errorString());
	});
	QString program;
	QStringList args;
	downloadCommand(release.url, archive, 300, false, &program, &args);
	curl->start(program, args);
}

// El hash esperado sale del "digest" del asset en la API de GitHub o, si falta, del SHA256SUMS
// de la Release. Sin ninguno de los dos no se instala nada.
void Updater::verify(const Release &release, const QString &archive)
{
	if (!release.sha256.isEmpty()) {
		installIfMatches(release.version, archive, release.sha256);
		return;
	}
	if (release.sumsUrl.isEmpty()) {
		busy_ = false;
		fail(T("Update.NoChecksum"), T("Update.Problem.NoSums"));
		return;
	}
	const QString sums = QDir(workDir()).filePath("SHA256SUMS");
	auto *curl = new QProcess(this);
	connect(curl, &QProcess::finished, this, [this, curl, release, archive, sums](int code, QProcess::ExitStatus) {
		const QString error = QString::fromUtf8(curl->readAllStandardError()).trimmed();
		curl->deleteLater();
		QFile file(sums);
		if (code != 0 || !file.open(QIODevice::ReadOnly)) {
			busy_ = false;
			fail(T("Update.NoChecksum"),
			     QString("%1 %2: %3").arg(tool(curl), QString::number(code), error));
			return;
		}
		installIfMatches(release.version, archive, sha256FromSums(file.readAll(), release.assetName));
	});
	connect(curl, &QProcess::errorOccurred, this, [this, curl](QProcess::ProcessError error) {
		if (error != QProcess::FailedToStart)
			return;
		curl->deleteLater();
		busy_ = false;
		fail(T("Update.NoChecksum"), curl->errorString());
	});
	QString program;
	QStringList args;
	downloadCommand(release.sumsUrl, sums, 30, false, &program, &args);
	curl->start(program, args);
}

void Updater::installIfMatches(const QString &version, const QString &archive, const QString &expected)
{
	if (expected.isEmpty()) {
		busy_ = false;
		fail(T("Update.NoChecksum"), T("Update.Problem.NotListed"));
		return;
	}
	const QString actual = sha256OfFile(archive);
	if (actual != expected) {
		busy_ = false;
		obs_log(LOG_WARNING, "update %s: SHA-256 mismatch (expected %s, got %s)", version.toUtf8().constData(),
			expected.toUtf8().constData(), actual.toUtf8().constData());
		fail(T("Update.VerifyFailed"), T("Update.Problem.Hash").arg(expected, actual));
		return;
	}
	obs_log(LOG_INFO, "update %s: SHA-256 verified", version.toUtf8().constData());
	install(version, archive);
}

void Updater::install(const QString &version, const QString &archive)
{
	const QString extracted = QDir(workDir()).filePath("unpacked");
	QDir().mkpath(extracted);
#if defined(_WIN32)
	const int unpack = QProcess::execute(systemTool("tar"), {"-xf", archive, "-C", extracted});
#elif defined(__APPLE__)
	const int unpack = QProcess::execute(systemTool("ditto"), {"-x", "-k", archive, extracted});
#else
	const int unpack = QProcess::execute(systemTool("tar"), {"-xzf", archive, "-C", extracted});
#endif
	QString problem;
	bool ok = false;
	if (unpack != 0)
		problem = T("Update.Problem.Unzip").arg(QDir::toNativeSeparators(archive), QString::number(unpack));
	else
		ok = installPlugin(extracted, &problem);

	busy_ = false;
	if (progress_) {
		progress_->close();
		progress_->deleteLater();
		progress_ = nullptr;
	}
	if (!ok) {
		obs_log(LOG_WARNING, "could not install version %s", version.toUtf8().constData());
		fail(T("Update.InstallFailed"), problem);
		return;
	}
	obs_log(LOG_INFO, "version %s installed; it is used after restarting OBS", version.toUtf8().constData());
	QMessageBox::information(mainWindow(), T("Update.Title"), T("Update.Done").arg(version));
}

void Updater::fail(const QString &message, const QString &detail)
{
	if (!detail.isEmpty())
		obs_log(LOG_WARNING, "update: %s", detail.toUtf8().constData());
	if (progress_) {
		progress_->close();
		progress_->deleteLater();
		progress_ = nullptr;
	}
	QMessageBox box(mainWindow());
	box.setWindowTitle(T("Update.Title"));
	box.setIcon(QMessageBox::Warning);
	box.setText(message);
	if (!detail.isEmpty())
		box.setInformativeText(detail);
	QPushButton *open = box.addButton(T("Update.OpenPage"), QMessageBox::AcceptRole);
	box.addButton(QMessageBox::Close);
	box.exec();
	if (box.clickedButton() == open)
		QDesktopServices::openUrl(QUrl(QString::fromLatin1(kReleasesPage)));
}

} // namespace acm
