// DESTRUCTIVE integration test. See test.bat: it starts and stops the real
// components under this directory and switches the active nginx version.
#include "common.h"
#include "proc.h"
#include "manager.h"
#include <winhttp.h>

// 0 = pass, 1 = fail, 2 = skipped.
#define RC_PASS 0
#define RC_FAIL 1
#define RC_SKIP 2

static int g_fail = 0;
static void check(const wchar_t* name, bool ok, const std::wstring& detail = L"") {
    wprintf(L"  [%s] %s %s\n", ok ? L"PASS" : L"FAIL", name, detail.c_str());
    if (!ok) ++g_fail;
}

// GET http://<hostHeader>/ and report the status code, or 0 when the request
// could not be completed at all (connection refused, TLS handshake refused).
// hostHeader lets us send an arbitrary Host header, which is the whole point:
// a real browser reaching the box by IP sends Host: <the ip>.
static int httpStatus(const std::wstring& port, const std::wstring& hostHeader) {
    std::wstring url = L"http://127.0.0.1:" + port + L"/";
    HINTERNET ses = WinHttpOpen(L"lnpp-selftest/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return 0;
    WinHttpSetTimeouts(ses, 5000, 5000, 5000, 5000);
    HINTERNET con = WinHttpConnect(ses, L"127.0.0.1", INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!con) { WinHttpCloseHandle(ses); return 0; }
    HINTERNET req = WinHttpOpenRequest(con, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    int status = 0;
    if (req) {
        std::wstring headers = L"Host: " + hostHeader + L"\r\n";
        WinHttpAddRequestHeaders(req, headers.c_str(), (DWORD)-1,
                                 WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD);
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                               0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
            DWORD code = 0, sz = sizeof(code);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
            status = (int)code;
        }
        WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return status;
}

// NOTE: no queryVer() helper here on purpose — selftest never runs SQL (that is
// migtest's job), and keeping an unused static function only produced a C4505
// warning in the /W4 build.
int wmain() {
    std::wstring err, out;
    wprintf(L"!!! 破坏性测试：会真实启停本目录下的组件\n");

    wprintf(L"=== Redis ===\n");
    {
        if (!compIsRunning(Comp::Redis)) check(L"start", compStart(Comp::Redis, err), err);
        Sleep(1200);
        check(L"running", compIsRunning(Comp::Redis));
        check(L"stop", compStop(Comp::Redis, err), err);
        Sleep(800);
        check(L"stopped", !compIsRunning(Comp::Redis));
    }

    wprintf(L"\n=== nginx ===\n");
    {
        ComponentStatus st = compStatus(Comp::Nginx);
        std::wstring origin = st.currentVersion;
        // Only a real switch can be tested. The old code fell back to
        // "switch to the current version", which compSwitchVersion rejects with
        // 已是当前版本 — a guaranteed FAIL on a single-version install that had
        // nothing to do with nginx.
        std::wstring target;
        for (const auto& v : st.versions) {
            if (v != origin) { target = v; break; }
        }
        if (target.empty()) {
            wprintf(L"  只安装了一个 nginx 版本（%s），跳过版本切换用例\n", origin.c_str());
        } else {
            std::wstring tname = L"switch to " + target;
            check(tname.c_str(), compSwitchVersion(Comp::Nginx, target, err), err);
            Sleep(1200);
            check(L"running", compIsRunning(Comp::Nginx));

            // The catch-all that refuses IP access and unconfigured domains.
            // Only meaningful when the feature is on, and only checkable while
            // nginx is up.
            if (lowerStr(iniGet(L"nginx.block_unknown_host", L"true")) != L"false") {
                std::wstring port = nginxPort();
                int byIp = httpStatus(port, L"127.0.0.1");
                check(L"IP 直连被兜底拒绝 (500)", byIp == 500, L"got " + std::to_wstring(byIp));
                int unknown = httpStatus(port, L"no-such-host.invalid");
                check(L"未配置域名被兜底拒绝 (500)", unknown == 500, L"got " + std::to_wstring(unknown));
                // A site created below must still answer, otherwise the block
                // would be rejecting everything rather than just unknown hosts.
                check(L"新增站点仍可访问 (add vhost)", nginxAddVHost(L"final1", L"final1.local", L"8081", err), err);
                int known = httpStatus(port, L"final1.local");
                check(L"已配置域名不被兜底影响", known != 500 && known != 0,
                      L"got " + std::to_wstring(known));
                int before = (int)nginxListVHosts().size();
                check(L"del vhost", nginxRemoveVHost(L"final1", err), err);
                std::wstring vc = L"list=" + std::to_wstring(before);
                check(vc.c_str(), nginxListVHosts().size() == before);
            } else {
                wprintf(L"  nginx.block_unknown_host=false，跳过兜底校验\n");
            }
            check(L"stop", compStop(Comp::Nginx, err), err);
            Sleep(800);
            check(L"stopped", !compIsRunning(Comp::Nginx));
            // Put the machine back the way we found it: the old version is
            // still selected, the vhost is gone, but nothing is running again.
            // Leaving the user on a version the test picked is a nasty surprise.
            if (compStatus(Comp::Nginx).currentVersion != origin) {
                check((L"restore version " + origin).c_str(),
                      compSwitchVersion(Comp::Nginx, origin, err), err);
                Sleep(600);
                if (compIsRunning(Comp::Nginx)) compStop(Comp::Nginx, err);
            }
        }
    }

    wprintf(L"\n=== PostgreSQL ===\n");
    {
        std::vector<std::wstring> vers = compVersions(Comp::Postgresql);
        if (vers.empty()) {
            wprintf(L"  未安装 PostgreSQL，跳过\n");
        } else {
            std::wstring ver = vers[0];   // compVersions 降序：最新在前
            bool init = pgDataInitialized(ver);
            if (!init) check((L"init " + ver).c_str(),
                             pgInit(Comp::Postgresql, ver, L"postgres", L"postgres", L"5432", err), err);
            if (!compIsRunning(Comp::Postgresql)) check(L"start", compStart(Comp::Postgresql, err), err);
            Sleep(2000);
            check(L"running", compIsRunning(Comp::Postgresql));
            std::wstring backupFile;
            check(L"backup", pgBackup(Comp::Postgresql, backupFile, err) && fileExists(backupFile), err);
            check(L"stop", compStop(Comp::Postgresql, err), err);
            Sleep(1200);
            check(L"stopped", !compIsRunning(Comp::Postgresql));
        }
    }

    wprintf(L"\n=== Node.js / pm2 ===\n");
    {
        // Read-only: never resurrect or kill here, the user's apps must not be
        // disturbed by a test that only wants to look.
        check(L"pm2 installed", nodePm2Installed());
        wprintf(L"  apps=%d (只读查询，未启停任何进程)\n", (int)nodePm2List().size());
    }

    wprintf(L"\nFAILURES: %d\n", g_fail);
    return g_fail ? RC_FAIL : RC_PASS;
}
