#pragma once
#ifndef LNPP_DOWNLOADER_H
#define LNPP_DOWNLOADER_H

#include "common.h"

// One entry of packages.conf: "Nginx-1.30.4=https://..."
struct PkgItem {
    std::wstring name;   // raw entry name, e.g. Nginx-1.30.4
    std::wstring url;
    std::wstring comp;   // mapped component dir: nginx/nodejs/postgresql/redis/php
    std::wstring ver;    // text after the first '-', e.g. 1.30.4
    std::wstring sha256; // optional lowercase 64-char hex; "" = not verified.
                         // Fed by a companion "Nginx-1.30.4.sha256=hex" entry.
};

struct PkgSection {
    std::wstring title;  // from "# comment" lines, e.g. Web Servers
    std::vector<PkgItem> items;
};

// Parse data\packages.conf. Unknown components (mysql/apache/tomcat/...) are
// skipped.
std::vector<PkgSection> pkgsParseConf(std::wstring& err);
// Same parser on raw text, so the whole conf grammar is unit-testable without
// writing a packages.conf into the runtime root (which needs admin rights and
// would race a real download).
std::vector<PkgSection> pkgsParseConfText(const std::wstring& text, std::wstring& err);
// First-run layout: creates data\ and seeds data\packages.conf from the
// etc\packages.conf.tpl template. Also migrates a pre-v1.5.3 root-level
// packages.conf into data\. Called once at startup.
void prepareDownloadList();

// Map an entry name like "node-24.12" to component dir "nodejs" + version "24.12".
bool pkgsNameToCompVer(const std::wstring& name, std::wstring& comp, std::wstring& ver);

// True when bin\ is missing or none of the known components has any version dir.
bool pkgsNeedSetup();

// Download url to destFile (overwrites). progress(doneBytes, totalBytes) is called
// per chunk; totalBytes may be 0 when the server sends no content-length.
// *cancel is polled between chunks; on cancel the partial file is deleted and
// false returned with err=L"已取消".
bool pkgsDownload(const std::wstring& url, const std::wstring& destFile,
                  const std::function<void(DWORD, DWORD)>& progress,
                  std::atomic<bool>* cancel, std::wstring& err);

// Extract a .zip into destDir using tar.exe, falling back to PowerShell Expand-Archive.
bool pkgsExtractZip(const std::wstring& zipFile, const std::wstring& destDir, std::wstring& err);

// Full pipeline for one entry: download -> extract -> place into bin\<comp>\<ver>.
// progress(stageText, doneBytes, totalBytes); stage is one of 下载中/解压中/安装中/完成.
bool pkgsInstall(const PkgItem& item,
                 const std::function<void(const std::wstring&, DWORD, DWORD)>& progress,
                 std::atomic<bool>* cancel, std::wstring& err);

#endif