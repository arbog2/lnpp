#pragma once
#ifndef LNPP_COMMON_H
#define LNPP_COMMON_H

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <sstream>
#include <algorithm>
#include <functional>
#include <chrono>
#include <cstdio>
#include <cwchar>

// ---- Path utils ----
std::wstring exeDir();
std::wstring rootDir();
std::wstring joinPath(const std::wstring& a, const std::wstring& b);
std::wstring binDir();
std::wstring etcDir();
std::wstring dataDir();
std::wstring backupDir();
std::wstring wwwDir();
std::wstring logsDir();

std::wstring binCompDir(const std::wstring& name);
std::wstring etcCompDir(const std::wstring& name);
std::wstring dataCompDir(const std::wstring& name);
std::wstring dataCompVerDir(const std::wstring& name, const std::wstring& ver);

// ---- String utils ----
std::wstring wsprintf(const wchar_t* fmt, ...);
std::vector<std::wstring> listSubDirs(const std::wstring& path);
std::vector<std::wstring> listFiles(const std::wstring& path, const std::wstring& ext);
bool dirExists(const std::wstring& path);
bool fileExists(const std::wstring& path);
bool makeDirs(const std::wstring& path);
std::wstring dirOf(const std::wstring& path);
std::wstring readFileText(const std::wstring& path);
bool writeFileText(const std::wstring& path, const std::wstring& content);
bool copyFileW2(const std::wstring& from, const std::wstring& to);
std::wstring replaceAll(const std::wstring& s, const std::wstring& from, const std::wstring& to);
std::wstring renderTemplate(const std::wstring& tpl, const std::map<std::wstring,std::wstring>& kv);
std::wstring lowerStr(const std::wstring& s);
std::wstring trimStr(const std::wstring& s);
std::wstring toForward(const std::wstring& s);
std::wstring nowStamp();
std::wstring nowText();

// ---- INI (key=value, section-less) ----
std::wstring iniGet(const std::wstring& key, const std::wstring& def);
void iniSet(const std::wstring& key, const std::wstring& val);
bool iniDelete(const std::wstring& key);
// Writes are debounced; the UI thread calls iniFlushIfDue() from its idle
// hook to drain pending changes. Use iniFlushNow() before exit / before
// reading the on-disk file from outside the cache.
void iniFlushIfDue();
void iniFlushNow();

// ---- Config paths ----
std::wstring settingsIniPath();

// ---- Log ----
void logMsg(const std::wstring& tag, const std::wstring& msg);

#endif