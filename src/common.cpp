#include "common.h"
#include <wincrypt.h>   // CryptProtectData / CryptStringToBinary (DPAPI)

std::wstring exeDir() {
    static std::wstring cached;
    if (!cached.empty()) return cached;
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    std::wstring path(buf, n);
    size_t pos = path.find_last_of(L"\\/");
    cached = (pos == std::wstring::npos) ? L"." : path.substr(0, pos);
    return cached;
}

std::wstring rootDir() { return exeDir(); }

std::wstring joinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    wchar_t last = a[a.size() - 1];
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring binDir()    { return joinPath(rootDir(), L"bin"); }
std::wstring etcDir()    { return joinPath(rootDir(), L"etc"); }
std::wstring dataDir()   { return joinPath(rootDir(), L"data"); }
std::wstring backupDir() { return joinPath(rootDir(), L"backup"); }
std::wstring wwwDir()    { return joinPath(rootDir(), L"www"); }
std::wstring logsDir()   { return joinPath(rootDir(), L"logs"); }

std::wstring binCompDir(const std::wstring& name) { return joinPath(binDir(), name); }
std::wstring etcCompDir(const std::wstring& name) { return joinPath(etcDir(), name); }
std::wstring dataCompDir(const std::wstring& name) { return joinPath(dataDir(), name); }
std::wstring dataCompVerDir(const std::wstring& name, const std::wstring& ver) {
    return joinPath(dataCompDir(name), ver);
}

std::wstring settingsIniPath() { return joinPath(dataDir(), L"settings.ini"); }

std::wstring wstrfmt(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    wchar_t buf[4096];
    _vsnwprintf_s(buf, 4096, _TRUNCATE, fmt, args);
    va_end(args);
    return std::wstring(buf);
}

bool dirExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool fileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool makeDirs(const std::wstring& path) {
    if (path.empty()) return false;
    if (dirExists(path)) return true;
    // Create intermediate directories
    std::wstring cur;
    for (size_t i = 0; i < path.size(); ++i) {
        wchar_t c = path[i];
        cur += c;
        if (c == L'\\' || c == L'/' || i == path.size() - 1) {
            if (!dirExists(cur)) {
                if (!CreateDirectoryW(cur.c_str(), nullptr)) {
                    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
                }
            }
        }
    }
    return true;
}

std::vector<std::wstring> listSubDirs(const std::wstring& path) {
    std::vector<std::wstring> result;
    if (!dirExists(path)) return result;
    std::wstring pattern = joinPath(path, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return result;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wstring name = fd.cFileName;
            if (name != L"." && name != L"..") result.push_back(name);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::wstring> listFiles(const std::wstring& path, const std::wstring& ext) {
    std::vector<std::wstring> result;
    if (!dirExists(path)) return result;
    std::wstring pattern = joinPath(path, L"*." + ext);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return result;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            result.push_back(fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(result.begin(), result.end());
    return result;
}

std::wstring readFileText(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER size;
    GetFileSizeEx(h, &size);
    if (size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024) {
        // Empty or absurdly large; refuse to allocate blindly. 64MB cap is well
        // above any template/ini we read; protects against corrupt size fields.
        CloseHandle(h);
        return L"";
    }
    std::string bytes((size_t)size.QuadPart, '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(h, &bytes[0], (DWORD)size.QuadPart, &read, nullptr);
    CloseHandle(h);
    if (!ok || read == 0) return L"";
    bytes.resize(read);
    if (bytes.empty()) return L"";
    // Decode as UTF-8; fall back to the ANSI code page for non-UTF-8 files
    // (older confs / logs). The buffer is sized from the *same* code page as
    // the conversion that fills it — the old code sized it from the UTF-8
    // length but wrote the ACP result into it, which can overrun on byte
    // sequences whose UTF-8 and ACP character counts differ.
    UINT cp = CP_UTF8;
    int len = MultiByteToWideChar(cp, 0, bytes.c_str(), (int)bytes.size(), nullptr, 0);
    if (len <= 0) {
        cp = CP_ACP;
        len = MultiByteToWideChar(cp, 0, bytes.c_str(), (int)bytes.size(), nullptr, 0);
    }
    if (len <= 0) return L"";
    std::wstring w(len, L'\0');
    MultiByteToWideChar(cp, 0, bytes.c_str(), (int)bytes.size(), &w[0], len);
    return w;
}

bool writeFileText(const std::wstring& path, const std::wstring& content) {
    makeDirs(dirOf(path));
    // Write to a sibling temp file then rename, so a crash or power loss
    // mid-write can never leave the target half-written (settings.ini would
    // otherwise be corrupted and every preference lost).
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    // Convert wide to UTF-8
    int len = WideCharToMultiByte(CP_UTF8, 0, content.c_str(), (int)content.size(), nullptr, 0, nullptr, nullptr);
    std::string bytes(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, content.c_str(), (int)content.size(), &bytes[0], len, nullptr, nullptr);
    DWORD written = 0;
    bool ok = WriteFile(h, bytes.c_str(), (DWORD)bytes.size(), &written, nullptr) != FALSE;
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::wstring dirOf(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : path.substr(0, pos);
}

bool copyFileW2(const std::wstring& from, const std::wstring& to) {
    return CopyFileW(from.c_str(), to.c_str(), FALSE) != FALSE;
}

std::wstring replaceAll(const std::wstring& s, const std::wstring& from, const std::wstring& to) {
    if (from.empty()) return s;
    std::wstring result = s;
    size_t pos = 0;
    while ((pos = result.find(from, pos)) != std::wstring::npos) {
        result.replace(pos, from.size(), to);
        pos += to.size();
    }
    return result;
}

std::wstring renderTemplate(const std::wstring& tpl, const std::map<std::wstring, std::wstring>& kv) {
    // Single-pass scan: copy tpl into result, but whenever we see a {{name}}
    // placeholder we look it up in kv. If the name isn't a key, leave the
    // placeholder untouched (so unknown substitutions don't vanish silently).
    // The single-pass approach also prevents cascade substitution: a value
    // containing "{{OTHER}}" used to be re-expanded, which was wrong.
    std::wstring result;
    result.reserve(tpl.size());
    size_t i = 0;
    while (i < tpl.size()) {
        if (i + 1 < tpl.size() && tpl[i] == L'{' && tpl[i + 1] == L'{') {
            size_t end = tpl.find(L"}}", i + 2);
            if (end != std::wstring::npos) {
                std::wstring name = trimStr(tpl.substr(i + 2, end - (i + 2)));
                auto it = kv.find(name);
                if (it != kv.end()) {
                    result += it->second;
                    i = end + 2;
                    continue;
                }
            }
        }
        result += tpl[i++];
    }
    return result;
}

std::wstring lowerStr(const std::wstring& s) {
    std::wstring r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::towlower);
    return r;
}

std::wstring trimStr(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring toForward(const std::wstring& s) {
    return replaceAll(s, L"\\", L"/");
}

std::wstring nowStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return wstrfmt(L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

std::wstring nowText() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return wstrfmt(L"%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

// ---- INI ----
// All keys are normalized to lower case on read/write so callers don't have
// to remember the exact spelling (ver.nodejs vs Ver.NodeJS). The on-disk
// file is therefore always written with lower-case keys, even if the original
// code paths used mixed case.
//
// Writes are debounced: iniSet / iniDelete mutate the in-memory map and
// schedule a flush after ~200 ms. Repeated sets (e.g. toggling 4 autostart
// checkboxes in a row) collapse into a single disk write.
//
// Threading: the cache is touched from several threads at once — the
// background status / pm2 pollers (compStatusQuick -> compStatus -> iniSet
// for the current version) and the UI thread (the 50 ms flush timer plus
// every user action that toggles a setting). A std::map under concurrent
// insert + iteration is a hard crash, so every access goes through
// g_iniMtx. Disk writes run outside the lock on a snapshot, so a slow or
// contended disk never blocks a poller.
std::map<std::wstring, std::wstring> g_iniCache;
bool g_iniLoaded = false;
bool g_iniDirty = false;
DWORD g_iniLastChangeTick = 0;
constexpr DWORD INI_DEBOUNCE_MS = 200;
std::mutex g_iniMtx;

// Caller must hold g_iniMtx.
static std::map<std::wstring, std::wstring>& iniMapLocked() {
    if (!g_iniLoaded) {
        g_iniLoaded = true;
        std::wstring path = settingsIniPath();
        if (fileExists(path)) {
            std::wstring content = readFileText(path);
            std::wstringstream ss(content);
            std::wstring line;
            while (std::getline(ss, line)) {
                size_t eq = line.find(L'=');
                if (eq != std::wstring::npos) {
                    std::wstring k = lowerStr(trimStr(line.substr(0, eq)));
                    std::wstring v = trimStr(line.substr(eq + 1));
                    if (!k.empty()) g_iniCache[k] = v;
                }
            }
        }
    }
    return g_iniCache;
}

// Caller must NOT hold g_iniMtx: this does file IO.
static void iniWriteSnapshot(const std::map<std::wstring, std::wstring>& snapshot) {
    std::wstring content;
    for (auto& p : snapshot) {
        content += p.first + L"=" + p.second + L"\r\n";
    }
    writeFileText(settingsIniPath(), content);
}

void iniFlushIfDue() {
    std::map<std::wstring, std::wstring> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_iniMtx);
        if (!g_iniDirty) return;
        DWORD now = GetTickCount();
        if (now - g_iniLastChangeTick < INI_DEBOUNCE_MS) return;
        iniMapLocked();   // never flush an unloaded cache: that would blank the file
        snapshot = g_iniCache;
        g_iniDirty = false;
    }
    iniWriteSnapshot(snapshot);
}

void iniFlushNow() {
    std::map<std::wstring, std::wstring> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_iniMtx);
        if (!g_iniDirty) return;
        iniMapLocked();   // never flush an unloaded cache: that would blank the file
        snapshot = g_iniCache;
        g_iniDirty = false;
    }
    iniWriteSnapshot(snapshot);
}

std::wstring iniGet(const std::wstring& key, const std::wstring& def) {
    std::lock_guard<std::mutex> lk(g_iniMtx);
    auto& m = iniMapLocked();
    auto it = m.find(lowerStr(key));
    return it == m.end() ? def : it->second;
}

void iniSet(const std::wstring& key, const std::wstring& val) {
    std::lock_guard<std::mutex> lk(g_iniMtx);
    iniMapLocked()[lowerStr(key)] = val;
    g_iniDirty = true;
    g_iniLastChangeTick = GetTickCount();
}

bool iniDelete(const std::wstring& key) {
    std::lock_guard<std::mutex> lk(g_iniMtx);
    auto& m = iniMapLocked();
    auto it = m.find(lowerStr(key));
    if (it == m.end()) return false;
    m.erase(it);
    g_iniDirty = true;
    g_iniLastChangeTick = GetTickCount();
    return true;
}

// ---- DPAPI secret protection ----
std::wstring dpProtect(const std::wstring& plain) {
    DATA_BLOB in = {(DWORD)(plain.size() * sizeof(wchar_t)), (BYTE*)plain.c_str()};
    DATA_BLOB out = {0, nullptr};
    if (!CryptProtectData(&in, L"LNPP settings", nullptr, nullptr, nullptr, 0, &out))
        return L"";
    // base64 without CR/LF so the ciphertext survives the INI format
    DWORD len = 0;
    CryptBinaryToStringW(out.pbData, out.cbData,
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
    std::wstring r(len, L'\0');
    if (len > 0)
        CryptBinaryToStringW(out.pbData, out.cbData,
                             CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &r[0], &len);
    LocalFree(out.pbData);
    return r;
}

std::wstring dpUnprotect(const std::wstring& enc) {
    if (enc.empty()) return L"";
    DWORD len = 0;
    if (!CryptStringToBinaryW(enc.c_str(), (DWORD)enc.size(), CRYPT_STRING_BASE64,
                              nullptr, &len, nullptr, nullptr))
        return L"";
    std::vector<BYTE> bytes(len);
    if (!CryptStringToBinaryW(enc.c_str(), (DWORD)enc.size(), CRYPT_STRING_BASE64,
                              bytes.data(), &len, nullptr, nullptr))
        return L"";
    DATA_BLOB in = {len, bytes.data()};
    DATA_BLOB out = {0, nullptr};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
        return L"";
    std::wstring r((wchar_t*)out.pbData, out.cbData / sizeof(wchar_t));
    LocalFree(out.pbData);
    return r;
}

// ---- Log ----
std::mutex g_logMtx;
HANDLE g_logFile = INVALID_HANDLE_VALUE;

void logMsg(const std::wstring& tag, const std::wstring& msg) {
    std::lock_guard<std::mutex> lock(g_logMtx);
    if (g_logFile == INVALID_HANDLE_VALUE) {
        makeDirs(logsDir());
        g_logFile = CreateFileW(joinPath(logsDir(), L"lnpp.log").c_str(),
                                FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    std::wstring line = nowText() + L" [" + tag + L"] " + msg + L"\r\n";
    int len = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
    std::string bytes(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &bytes[0], len, nullptr, nullptr);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_logFile, bytes.c_str(), (DWORD)bytes.size(), &written, nullptr);
    }
}