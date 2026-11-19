/*
AC Motion Mask - per-OS installers of the updater
Copyright (C) 2026 Alejandro Grajales (alejoxcode)
SPDX-License-Identifier: GPL-2.0-or-later

Reemplazan el plugin donde está instalado ahora (el binario cargado y su carpeta de datos), así
funciona tanto si se instaló con el instalador como si se copió a mano.
  - macOS: reemplaza ac-motion-mask.plugin; si está en /Library (instalador .pkg), pide la
    contraseña de administrador. Le quita la marca de "descargado de internet".
  - Windows: copia el .dll y los datos con un PowerShell "Ejecutar como administrador". El .dll
    cargado se renombra a .old (lo borra la actualización siguiente).
  - Linux: reemplaza el .so y los datos si están en una carpeta del usuario; si vino en un
    paquete del sistema (.deb), lo actualiza ese paquete.
*/

#include "updater.hpp"

#include <obs-module.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <string>
#else
#include <cstdio>
#endif

namespace acm {

namespace {

inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

#ifndef _WIN32
// Copia un árbol de carpetas encima de otro (crea lo que falte, pisa lo que exista).
bool copyTree(const QString &from, const QString &to, QString *problem)
{
	QDir source(from);
	QDirIterator it(from, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
	while (it.hasNext()) {
		const QString file = it.next();
		const QString target = QDir(to).filePath(source.relativeFilePath(file));
		QDir().mkpath(QFileInfo(target).absolutePath());
		QFile existing(target);
		if (existing.exists() && !existing.remove()) {
			*problem = T("Update.Problem.Replace").arg(target, existing.errorString());
			return false;
		}
		QFile copy(file);
		if (!copy.copy(target)) {
			*problem = T("Update.Problem.Copy").arg(target, copy.errorString());
			return false;
		}
	}
	return true;
}
#endif

#if defined(_WIN32)
const char kInstallScript[] = R"PS(param([string]$Dll, [string]$Data, [string]$TargetDll, [string]$TargetData)
$ErrorActionPreference = 'Stop'
$dir = Split-Path $TargetDll
$file = Split-Path $TargetDll -Leaf
$backup = ''
$copying = $false
try {
  Get-ChildItem -LiteralPath $dir -Filter "$file.*.old" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
  if (Test-Path -LiteralPath $TargetDll) {
    $name = "$file.$([DateTimeOffset]::Now.ToUnixTimeSeconds()).old"
    Rename-Item -LiteralPath $TargetDll -NewName $name
    $backup = Join-Path $dir $name
  }
  $copying = $true
  Copy-Item -LiteralPath $Dll -Destination $TargetDll -Force
  New-Item -ItemType Directory -Force -Path $TargetData | Out-Null
  robocopy $Data $TargetData /E /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "robocopy $LASTEXITCODE" }
} catch {
  if ($copying) { Remove-Item -LiteralPath $TargetDll -Force -ErrorAction SilentlyContinue }
  if ($backup) { Rename-Item -LiteralPath $backup -NewName $file -ErrorAction SilentlyContinue }
  exit 1
}
exit 0
)PS";

int runElevated(const QString &program, const QString &params, QString *problem)
{
	const std::wstring file = program.toStdWString();
	const std::wstring args = params.toStdWString();
	SHELLEXECUTEINFOW info = {};
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
	info.lpVerb = L"runas";
	info.lpFile = file.c_str();
	info.lpParameters = args.c_str();
	info.nShow = SW_HIDE;
	if (!ShellExecuteExW(&info) || !info.hProcess) {
		const DWORD error = GetLastError();
		*problem = error == ERROR_CANCELLED ? T("Update.Cancelled")
						    : T("Update.Problem.Elevate").arg(QString::number(error));
		return -1;
	}
	if (WaitForSingleObject(info.hProcess, 180000) == WAIT_TIMEOUT) {
		CloseHandle(info.hProcess);
		*problem = T("Update.Problem.Timeout");
		return -1;
	}
	DWORD code = 1;
	GetExitCodeProcess(info.hProcess, &code);
	CloseHandle(info.hProcess);
	return int(code);
}
#endif

} // namespace

#if defined(_WIN32)

bool installPlugin(const QString &extracted, QString *problem)
{
	const QString dll = QDir(extracted).filePath("ac-motion-mask/bin/64bit/ac-motion-mask.dll");
	const QString data = QDir(extracted).filePath("ac-motion-mask/data");
	if (!QFileInfo::exists(dll) || !QFileInfo(data).isDir()) {
		*problem = T("Update.Problem.ZipNoFolder");
		return false;
	}
	const QString targetDll = moduleBinary();
	const QString targetData = moduleDataPath();
	if (QFileInfo(targetDll).fileName().compare("ac-motion-mask.dll", Qt::CaseInsensitive) != 0 ||
	    targetData.isEmpty()) {
		*problem = T("Update.Problem.NotInPlugins").arg(QDir::toNativeSeparators(targetDll));
		return false;
	}

	const QString script = QDir(workDir()).filePath("instalar.ps1");
	QFile scriptFile(script);
	if (!scriptFile.open(QIODevice::WriteOnly) || scriptFile.write(kInstallScript) < 0) {
		*problem = T("Update.Problem.Write").arg(QDir::toNativeSeparators(script));
		return false;
	}
	scriptFile.close();

	auto arg = [](const char *name, const QString &value) {
		return QString(" -%1 \"%2\"").arg(QString::fromUtf8(name), QDir::toNativeSeparators(value));
	};
	QString params =
		QString("-NoProfile -ExecutionPolicy Bypass -File \"%1\"").arg(QDir::toNativeSeparators(script));
	params += arg("Dll", dll) + arg("Data", data) + arg("TargetDll", targetDll) + arg("TargetData", targetData);

	const int code = runElevated("powershell.exe", params, problem);
	if (code > 0)
		*problem = T("Update.Problem.CopyTo").arg(QDir::toNativeSeparators(targetDll), QString::number(code));
	return code == 0;
}

#elif defined(__APPLE__)

namespace {

QString shellQuote(QString s)
{
	return "'" + s.replace("'", "'\\''") + "'";
}

QString appleScriptQuote(QString s)
{
	return "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
}

} // namespace

// En macOS el binario cargado sigue funcionando aunque se reemplace el paquete.
bool installPlugin(const QString &extracted, QString *problem)
{
	QDir dir = QFileInfo(moduleBinary()).absoluteDir(); // …/ac-motion-mask.plugin/Contents/MacOS
	dir.cdUp();
	dir.cdUp();
	const QString root = dir.absolutePath();
	const QString fresh = QDir(extracted).filePath("ac-motion-mask.plugin");
	if (QFileInfo(root).fileName() != "ac-motion-mask.plugin") {
		*problem = T("Update.Problem.OwnFolder").arg(root);
		return false;
	}
	if (!QFileInfo(fresh).isDir()) {
		*problem = T("Update.Problem.ZipNoBundle");
		return false;
	}

	const QString backup = root + ".old";
	if (QFileInfo(QFileInfo(root).absolutePath()).isWritable()) {
		QDir(backup).removeRecursively();
		if (!QDir().rename(root, backup)) {
			*problem = T("Update.Problem.Move").arg(root);
			return false;
		}
		if (!QDir().rename(fresh, root) && !copyTree(fresh, root, problem)) {
			QDir(root).removeRecursively();
			QDir().rename(backup, root); // deja el plugin como estaba
			return false;
		}
		QDir(backup).removeRecursively();
	} else {
		/* /Library/Application Support/obs-studio/plugins (instalador .pkg): con permiso de
		 * administrador, y si algo falla vuelve el plugin de antes. */
		const QString cmd = QString("rm -rf %2 && mv %1 %2 && (/usr/bin/ditto %3 %1 || (rm -rf %1; mv %2 %1; "
					    "exit 1)) && rm -rf %2")
					    .arg(shellQuote(root), shellQuote(backup), shellQuote(fresh));
		const QString script =
			QString("do shell script %1 with administrator privileges").arg(appleScriptQuote(cmd));
		const int code = QProcess::execute("/usr/bin/osascript", {"-e", script});
		if (code != 0) {
			*problem = T("Update.Problem.Admin").arg(root);
			return false;
		}
	}
	// Sin la marca de "descargado de internet", macOS no vuelve a pedir permiso para abrirlo.
	QProcess::execute("/usr/bin/xattr", {"-cr", root});
	return true;
}

#else

bool installPlugin(const QString &extracted, QString *problem)
{
	const QString binary = moduleBinary();
	const QString dataDir = moduleDataPath();
	const QString fresh = QDir(extracted).filePath("ac-motion-mask");
	const QString freshSo = fresh + "/bin/64bit/ac-motion-mask.so";
	if (!QFileInfo::exists(freshSo) || !QFileInfo(fresh + "/data").isDir()) {
		*problem = T("Update.Problem.ArchiveNoFolder");
		return false;
	}
	const bool userInstall = binary.startsWith(QDir::homePath() + "/") &&
				 QFileInfo(QFileInfo(binary).absolutePath()).isWritable() &&
				 QFileInfo(dataDir).isWritable();
	if (!userInstall) {
		*problem = T("Update.Problem.NotInLinux").arg(binary);
		return false;
	}
	// rename() cambia el archivo de una vez: el .so cargado sigue en memoria hasta reiniciar OBS.
	const QString temp = binary + ".new";
	QFile::remove(temp);
	if (!QFile::copy(freshSo, temp)) {
		*problem = T("Update.Problem.CopyFile").arg(temp);
		return false;
	}
	if (std::rename(QFile::encodeName(temp).constData(), QFile::encodeName(binary).constData()) != 0) {
		QFile::remove(temp);
		*problem = T("Update.Problem.ReplaceFile").arg(binary);
		return false;
	}
	return copyTree(fresh + "/data", dataDir, problem);
}

#endif

} // namespace acm
