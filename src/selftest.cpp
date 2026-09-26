// DESTRUCTIVE integration test. See test.bat: it starts and stops the real
// components under this directory and switches the active nginx version.
#include "common.h"
#include "proc.h"
#include "manager.h"

// 0 = pass, 1 = fail, 2 = skipped.
#define RC_PASS 0
#define RC_FAIL 1
#define RC_SKIP 2

static int g_fail = 0;
static void check(const wchar_t* name, bool ok, const std::wstring& detail = L"") {
    wprintf(L"  [%s] %s %s\n", ok ? L"PASS" : L"FAIL", name, detail.c_str());
    if (!ok) ++g_fail;
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
            int before = (int)nginxListVHosts().size();
            check(L"add vhost", nginxAddVHost(L"final1", L"final1.local", L"8081", err), err);
            std::wstring vc = L"list=" + std::to_wstring(before + 1);
            check(vc.c_str(), nginxListVHosts().size() == before + 1);
            check(L"del vhost", nginxRemoveVHost(L"final1", err), err);
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
