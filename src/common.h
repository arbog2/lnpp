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
// Named wstrfmt, not wsprintf: windows.h defines a wsprintf macro that would
// expand into our declaration and break it.
std::wstring wstrfmt(const wchar_t* fmt, ...);
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
// Every function below is safe to call from any thread; the cache is guarded
// internally. Keys are normalized to lower case, so "Ver.NodeJS" and
// "ver.nodejs" are the same setting.
std::wstring iniGet(const std::wstring& key, const std::wstring& def);
void iniSet(const std::wstring& key, const std::wstring& val);
bool iniDelete(const std::wstring& key);
// Writes are debounced; the UI thread calls iniFlushIfDue() from its idle
// hook to drain pending changes. Use iniFlushNow() before exit / before
// reading the on-disk file from outside the cache.
void iniFlushIfDue();
void iniFlushNow();
// Pure text helpers (no IO, no global state) — parse/serialize "k=v" lines;
// keys are lowercased and trimmed on parse, comment/blank lines are skipped.
std::map<std::wstring,std::wstring> parseIniText(const std::wstring& content);
std::wstring serializeIni(const std::map<std::wstring,std::wstring>& m);

// ---- Config paths ----
std::wstring settingsIniPath();

// ---- Secret protection (DPAPI) ----
// Encrypt/decrypt a string with the current Windows user's DPAPI key.
// dpProtect returns "" on failure; the ciphertext is base64 (no CR/LF) so it
// can live in the INI file. Keys are bound to the user account: on another
// machine the ciphertext cannot be decrypted (callers must fall back to
// asking the user again).
std::wstring dpProtect(const std::wstring& plain);
std::wstring dpUnprotect(const std::wstring& enc);

// ---- Log ----
void logMsg(const std::wstring& tag, const std::wstring& msg);

#endif