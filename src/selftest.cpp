#include "common.h"
#include "process.h"
#include "manager.h"

static int g_fail = 0;
static void check(const wchar_t* name, bool ok, const std::wstring& detail = L"") {
    wprintf(L"  [%s] %s %s\n", ok ? L"PASS" : L"FAIL", name, detail.c_str());
    if (!ok) ++g_fail;
}

static bool queryVer(const std::wstring& ver, const std::wstring& db, const std::wstring& sql, std::wstring& out) {
    std::wstring psql = joinPath(joinPath(compBinDirVer(Comp::Postgresql, ver), L"bin"), L"psql.exe");
    RunResult r = runProcessCapture(psql,
        L"-h 127.0.0.1 -p " + pgPort() + L" -U " + pgUser() +
        L" -d " + db + L" -t -c \"" + sql + L"\"",
        compBinDirVer(Comp::Postgresql, ver), 15000, {{L"PGPASSWORD", pgPassword()}});
    out = r.output;
    return r.ok;
}

int wmain() {
    std::wstring err, out;

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
        std::wstring target = (st.currentVersion == L"1.28") ? L"1.30" : L"1.28";
        std::wstring tname = L"switch to " + target;
        check(tname.c_str(), compSwitchVersion(Comp::Nginx, target, err), err);
        Sleep(1200);
        check(L"running", compIsRunning(Comp::Nginx));
        check(L"add vhost", nginxAddVHost(L"final1", L"final1.local", L"8081", err), err);
        check(L"list=1", nginxListVHosts().size() == 1);
        check(L"del vhost", nginxRemoveVHost(L"final1", err), err);
        check(L"stop", compStop(Comp::Nginx, err), err);
        Sleep(800);
        check(L"stopped", !compIsRunning(Comp::Nginx));
    }

    wprintf(L"\n=== PostgreSQL ===\n");
    {
        bool init = pgDataInitialized(L"17");
        if (!init) check(L"init 17", pgInit(Comp::Postgresql, L"17", L"postgres", L"postgres", L"5432", err), err);
        if (!compIsRunning(Comp::Postgresql)) check(L"start", compStart(Comp::Postgresql, err), err);
        Sleep(2000);
        check(L"running", compIsRunning(Comp::Postgresql));
        std::wstring backupFile;
        check(L"backup", pgBackup(Comp::Postgresql, backupFile, err) && fileExists(backupFile), err);
        check(L"stop", compStop(Comp::Postgresql, err), err);
        Sleep(1200);
        check(L"stopped", !compIsRunning(Comp::Postgresql));
    }

    wprintf(L"\n=== Node.js / pm2 ===\n");
    {
        check(L"pm2 installed", nodePm2Installed());
        wprintf(L"  apps=%d\n", (int)nodePm2List().size());
    }

    wprintf(L"\nFAILURES: %d\n", g_fail);
    return g_fail;
}