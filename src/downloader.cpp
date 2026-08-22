#include "downloader.h"
#include "process.h"
#include <winhttp.h>
#include <shellapi.h>

// ============================ conf parsing ============================

static std::wstring pkgsConfPath() { return joinPath(rootDir(), L"packages.conf"); }

std::vector<PkgSection> pkgsParseConf(std::wstring& err) {
    std::vector<PkgSection> sections;
    std::wstring text = readFileText(pkgsConfPath());
    if (text.empty()) { err = L"未找到 packages.conf"; return sections; }

    // Each section: ["# title" ] then "---" delimiter, then name=url entries, then
    // "---". A "#" comment is the title of the section and precedes its delimiter;
    // only the FIRST comment after a "---" (or file start) is used (secondary "#
    // https://..." notes are ignored).
    PkgSection cur;
    std::wstring pendingTitle;
    bool fresh = true;   // true right after a "---" or at file start
    auto flush = [&]() {
        if (!cur.items.empty()) sections.push_back(cur);
        cur = PkgSection();
        fresh = true;
    };
    std::wstringstream ss(text);
    std::wstring line;
    while (std::getline(ss, line)) {
        line = trimStr(line);
        if (line.empty()) continue;
        if (line == L"---") { flush(); continue; }
        if (line[0] == L'#') {
            // only the first comment of a section becomes its title; secondary notes
            // like "# https://..." must not clobber it
            if (fresh) { pendingTitle = trimStr(line.substr(1)); fresh = false; }
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || eq == 0) continue;
        if (cur.items.empty()) cur.title = pendingTitle;   // attach to first entry
        fresh = false;
        PkgItem item;
        item.name = trimStr(line.substr(0, eq));
        item.url  = trimStr(line.substr(eq + 1));
        if (item.name.empty() || item.url.empty()) continue;
        std::wstring comp, ver;
        if (!pkgsNameToCompVer(item.name, comp, ver)) continue;   // unknown component
        item.comp = comp;
        item.ver = ver;
        cur.items.push_back(item);
    }
    flush();
    if (sections.empty()) err = L"packages.conf 中没有可用条目";
    return sections;
}

bool pkgsNameToCompVer(const std::wstring& name, std::wstring& comp, std::wstring& ver) {
    size_t dash = name.find(L'-');
    if (dash == std::wstring::npos || dash == 0) return false;
    comp = lowerStr(name.substr(0, dash));
    ver = name.substr(dash + 1);
    if (comp == L"node") comp = L"nodejs";   // alias: dir is bin\nodejs
    if (ver.empty()) return false;
    return comp == L"nginx" || comp == L"nodejs" || comp == L"postgresql" || comp == L"redis";
}

bool pkgsNeedSetup() {
    for (const wchar_t* c : {L"nginx", L"nodejs", L"postgresql", L"redis"}) {
        if (!listSubDirs(binCompDir(c)).empty()) return false;
    }
    return true;
}

// ============================ fs helpers ============================

// SHFileOperation wants a double-null-terminated path list.
static std::wstring shPath(const std::wstring& p) {
    std::wstring s = p;
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    s.push_back(L'\0');
    s.push_back(L'\0');
    return s;
}

static bool shCopyDir(const std::wstring& from, const std::wstring& to) {
    SHFILEOPSTRUCTW op = {0};
    op.hwnd = nullptr;
    op.wFunc = FO_COPY;
    std::wstring f = shPath(from), t = shPath(to);
    op.pFrom = f.c_str();
    op.pTo = t.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

static bool shDeleteTree(const std::wstring& path) {
    if (!dirExists(path)) return true;
    SHFILEOPSTRUCTW op = {0};
    op.wFunc = FO_DELETE;
    std::wstring f = shPath(path);
    op.pFrom = f.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0;
}

static std::wstring tempWorkRoot() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return joinPath(tmp, L"lnpp_dl");
}

// ============================ download ============================

static bool crackUrl(const std::wstring& url, std::wstring& host, std::wstring& path, bool& secure) {
    URL_COMPONENTSW uc = {0};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[512] = {0};
    wchar_t pathBuf[4096] = {0};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = 512;
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return false;
    host.assign(uc.lpszHostName, uc.dwHostNameLength);
    path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

bool pkgsDownload(const std::wstring& url, const std::wstring& destFile,
                  const std::function<void(DWORD, DWORD)>& progress,
                  std::atomic<bool>* cancel, std::wstring& err) {
    std::wstring host, path;
    bool secure = false;
    if (!crackUrl(url, host, path, secure)) {
        err = L"URL 无法解析: " + url;
        return false;
    }

    bool ok = false;
    HINTERNET ses = nullptr, con = nullptr, req = nullptr;
    HANDLE hfile = INVALID_HANDLE_VALUE;
    ses = WinHttpOpen(L"LNPP-Manager/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { err = L"WinHttpOpen 失败"; goto done; }
    WinHttpSetTimeouts(ses, 15000, 20000, 30000, 30000);
    con = WinHttpConnect(ses, host.c_str(),
                         secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!con) { err = L"连接 " + host + L" 失败"; goto done; }
    req = WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { err = L"创建请求失败"; goto done; }
    // EDB/github release links answer with redirects; GET follows them by default
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        err = L"发送请求失败（网络或代理问题）";
        goto done;
    }
    {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            err = L"服务器返回 HTTP " + std::to_wstring(status);
            goto done;
        }
    }
    {
        DWORD total = 0, sz = sizeof(total);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &total, &sz, WINHTTP_NO_HEADER_INDEX);

        hfile = CreateFileW(destFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hfile == INVALID_HANDLE_VALUE) { err = L"无法写入临时文件: " + destFile; goto done; }

        DWORD done = 0;
        for (;;) {
            if (cancel && cancel->load()) {
                CloseHandle(hfile); hfile = INVALID_HANDLE_VALUE;
                DeleteFileW(destFile.c_str());
                err = L"已取消";
                goto done;
            }
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) { err = L"读取响应失败"; goto done; }
            if (avail == 0) break;
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!WinHttpReadData(req, buf.data(), avail, &read)) { err = L"读取数据失败"; goto done; }
            if (read == 0) break;
            DWORD written = 0;
            if (!WriteFile(hfile, buf.data(), read, &written, nullptr) || written != read) {
                err = L"写入临时文件失败（磁盘空间不足？）";
                goto done;
            }
            done += written;
            if (progress) progress(done, total);
        }
        ok = true;
    }

done:
    if (hfile != INVALID_HANDLE_VALUE) CloseHandle(hfile);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

// ============================ extraction ============================

bool pkgsExtractZip(const std::wstring& zipFile, const std::wstring& destDir, std::wstring& err) {
    if (!makeDirs(destDir)) { err = L"无法创建解压目录"; return false; }
    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring tar = joinPath(sysDir, L"tar.exe");
    if (fileExists(tar)) {
        RunResult r = runProcessCapture(tar,
            L"-xf \"" + zipFile + L"\" -C \"" + destDir + L"\"", destDir, 600000);
        if (r.ok) return true;
        err = L"tar 解压失败: " + r.output;
        return false;
    }
    // fallback: PowerShell 5.1 Expand-Archive
    RunResult r = runProcessCapture(L"powershell.exe",
        L"-NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" +
        zipFile + L"' -DestinationPath '" + destDir + L"' -Force\"",
        destDir, 600000);
    if (r.ok) return true;
    err = L"解压失败: " + r.output;
    return false;
}

// ============================ install pipeline ============================

bool pkgsInstall(const PkgItem& item,
                 const std::function<void(const std::wstring&, DWORD, DWORD)>& progress,
                 std::atomic<bool>* cancel, std::wstring& err) {
    std::wstring comp, ver;
    if (!pkgsNameToCompVer(item.name, comp, ver)) {
        err = L"无法识别的包名: " + item.name;
        return false;
    }
    std::wstring target = joinPath(binCompDir(comp), ver);
    if (dirExists(target)) {
        err = L"版本已存在，不覆盖: " + target;
        return false;
    }

    std::wstring root = tempWorkRoot();
    shDeleteTree(root);
    if (!makeDirs(root)) { err = L"无法创建临时目录"; return false; }
    std::wstring zipFile = joinPath(root, item.name + L".zip");
    std::wstring extractDir = joinPath(root, L"extract");
    if (!makeDirs(extractDir)) { err = L"无法创建解压目录"; return false; }

    progress(L"下载中", 0, 0);
    if (!pkgsDownload(item.url, zipFile,
                      [&](DWORD d, DWORD t) { progress(L"下载中", d, t); },
                      cancel, err)) {
        return false;
    }
    if (cancel && cancel->load()) { err = L"已取消"; return false; }

    progress(L"解压中", 0, 0);
    if (!pkgsExtractZip(zipFile, extractDir, err)) return false;

    // nginx/node/pg zips wrap everything in one top-level folder; redis ships
    // loose files. Normalize to "the folder that holds component files".
    std::wstring srcDir = extractDir;
    {
        auto subs = listSubDirs(extractDir);
        auto files = listFiles(extractDir, L"*");
        if (subs.size() == 1 && files.empty()) srcDir = joinPath(extractDir, subs[0]);
    }

    progress(L"安装中", 0, 0);
    makeDirs(binCompDir(comp));
    if (!shCopyDir(srcDir, target)) {
        err = L"复制到 " + target + L" 失败";
        shDeleteTree(root);
        return false;
    }

    shDeleteTree(root);
    progress(L"完成", 0, 0);
    return true;
}