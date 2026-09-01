#include "downloader.h"
#include "manager.h"
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
    //
    // A companion "name.sha256=hex" entry attaches an optional integrity digest
    // to a package; it is attached after the section is fully parsed, so the
    // digest may appear before or after its package line.
    PkgSection cur;
    std::wstring pendingTitle;
    std::map<std::wstring, std::wstring> shaMap;
    bool fresh = true;   // true right after a "---" or at file start
    auto flush = [&]() {
        // Bind the pending title to the section even if it has no items
        // (a section like "# Deprecated\n# https://...\n---" would
        // otherwise lose its title and confuse the user).
        if (!cur.items.empty() || !pendingTitle.empty()) {
            if (cur.title.empty()) cur.title = pendingTitle;
            for (auto& it : cur.items) {
                auto s = shaMap.find(it.name);
                if (s != shaMap.end()) it.sha256 = s->second;
            }
            sections.push_back(cur);
        }
        cur = PkgSection();
        pendingTitle.clear();
        shaMap.clear();
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
            if (fresh && pendingTitle.empty()) {
                pendingTitle = trimStr(line.substr(1));
                fresh = false;
            }
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
        // Digest companion entries MUST be recognized before the generic
        // component parse: pkgsNameToCompVer would otherwise happily accept
        // "Nginx-1.30.4.sha256" as a package with version "1.30.4.sha256".
        const std::wstring SUFFIX = L".sha256";
        if (item.name.size() > SUFFIX.size() &&
            lowerStr(item.name.substr(item.name.size() - SUFFIX.size())) == SUFFIX) {
            std::wstring base = item.name.substr(0, item.name.size() - SUFFIX.size());
            if (!base.empty()) shaMap[base] = item.url;
            continue;
        }
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
    const Comp components[] = {Comp::Nginx, Comp::Nodejs, Comp::Postgresql, Comp::Redis};
    for (Comp c : components) {
        if (!compVersions(c).empty()) return false;
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

// Compute the SHA-256 hex digest (lowercase, 64 chars) of a file using the
// OS-provided certutil. Returns "" on any failure. Zero extra dependencies
// and no code that parses the zip ourselves.
static std::wstring fileSha256(const std::wstring& path) {
    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring certutil = joinPath(sysDir, L"certutil.exe");
    if (!fileExists(certutil)) return L"";
    RunResult r = runProcessCapture(certutil,
        L"-hashfile \"" + path + L"\" SHA256", dirOf(path), 60000);
    if (!r.ok) return L"";
    // Output looks like:
    //   SHA256 的 <path> 哈希:
    //   abc123...64 hex chars...
    //   CertUtil: -hashfile 命令成功完成。
    // Accept any line that is exactly 64 hex chars after stripping spaces
    // (certutil may group pairs on some locales).
    std::wstring hex;
    std::wstringstream ss(r.output);
    std::wstring line;
    while (std::getline(ss, line)) {
        std::wstring t = lowerStr(trimStr(line));
        t.erase(std::remove_if(t.begin(), t.end(), [](wchar_t c) {
            return c == L' ' || c == L'\t' || c == L'\r';
        }), t.end());
        if (t.size() == 64 &&
            t.find_first_not_of(L"0123456789abcdef") == std::wstring::npos) {
            hex = t;
            break;
        }
    }
    return hex;
}

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
    // PowerShell fallback (tar.exe absent). Write a temp .ps1 and run it with
    // -File: the old "-Command -" variant tried to pipe the script through
    // stdin, which runProcessCapture never does, so the first call always
    // failed and was dead code; the short command-line fallback also broke on
    // paths containing quotes. A script file avoids both problems.
    std::wstring psFile = zipFile + L".ps1";
    std::wstring script = L"Expand-Archive -LiteralPath '"
                        + zipFile + L"' -DestinationPath '"
                        + destDir + L"' -Force";
    if (!writeFileText(psFile, script)) {
        err = L"无法写入临时解压脚本";
        return false;
    }
    RunResult r = runProcessCapture(L"powershell.exe",
        L"-NoProfile -ExecutionPolicy Bypass -File \"" + psFile + L"\"",
        destDir, 600000);
    DeleteFileW(psFile.c_str());
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

    auto progressSafe = [&progress](const std::wstring& s, DWORD d, DWORD t) {
        if (progress) progress(s, d, t);
    };
    progressSafe(L"下载中", 0, 0);
    if (!pkgsDownload(item.url, zipFile,
                      [&](DWORD d, DWORD t) { progressSafe(L"下载中", d, t); },
                      cancel, err)) {
        shDeleteTree(root);   // never leave a partial download behind
        return false;
    }
    if (cancel && cancel->load()) { err = L"已取消"; shDeleteTree(root); return false; }

    // Integrity check: when the conf carries a sha256 digest, verify the
    // downloaded bytes before extracting anything. A mismatch means the file
    // is corrupt or the source was tampered with — bail out and clean up.
    if (!item.sha256.empty()) {
        progressSafe(L"校验中", 0, 0);
        std::wstring hex = fileSha256(zipFile);
        if (hex.empty()) {
            err = L"无法计算下载文件的 SHA256（certutil 不可用？）";
            shDeleteTree(root);
            return false;
        }
        if (lowerStr(item.sha256) != hex) {
            err = L"SHA256 校验失败：期望 " + item.sha256 + L"，实际 " + hex;
            shDeleteTree(root);
            return false;
        }
    }

    progressSafe(L"解压中", 0, 0);
    if (!pkgsExtractZip(zipFile, extractDir, err)) {
        shDeleteTree(root);
        return false;
    }

    // nginx/node/pg zips wrap everything in one top-level folder; redis ships
    // loose files. Normalize to "the folder that holds component files".
    std::wstring srcDir = extractDir;
    {
        auto subs = listSubDirs(extractDir);
        auto files = listFiles(extractDir, L"*");
        if (subs.size() == 1 && files.empty()) srcDir = joinPath(extractDir, subs[0]);
    }

    progressSafe(L"安装中", 0, 0);
    makeDirs(binCompDir(comp));
    if (!shCopyDir(srcDir, target)) {
        err = L"复制到 " + target + L" 失败";
        shDeleteTree(root);
        return false;
    }

    Comp c = comp == L"nginx" ? Comp::Nginx :
             comp == L"nodejs" ? Comp::Nodejs :
             comp == L"postgresql" ? Comp::Postgresql : Comp::Redis;
    if (!compVersionUsable(c, ver)) {
        err = L"安装完成但缺少组件核心文件: " + target;
        shDeleteTree(target);
        shDeleteTree(root);
        return false;
    }

    shDeleteTree(root);
    progressSafe(L"完成", 0, 0);
    return true;
}
