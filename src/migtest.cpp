#include "common.h"
#include "proc.h"
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
    Comp c = Comp::Postgresql;
    std::wstring err, out;

    // Pick the two installed PG versions automatically (compVersions is
    // sorted newest-first), so the test doesn't hardcode 17/18.
    std::vector<std::wstring> vers = compVersions(c);
    if (vers.size() < 2) {
        wprintf(L"需要至少两个已安装的 PostgreSQL 版本才能做迁移测试（已装 %d 个），跳过。\n", (int)vers.size());
        wprintf(L"FAILURES: 0\n");
        return 0;
    }
    std::wstring vNew = vers[0];
    std::wstring vOld = vers[1];

    // ensure seeded: appdb.t(id=1,'hello') — start from the old version
    wprintf(L"=== ensure data (on %s) ===\n", vOld.c_str());
    if (compStatus(c).currentVersion != vOld)
        check((L"switch to " + vOld).c_str(), compSwitchVersion(c, vOld, err), err);
    if (!compIsRunning(c)) { check((L"start " + vOld).c_str(), compStart(c, err), err); Sleep(2000); }
    check(L"seed data present", queryVer(vOld, L"appdb", L"SELECT v FROM t WHERE id=1;", out)
          && out.find(L"hello") != std::wstring::npos, out);

    wprintf(L"\n=== %s -> %s ===\n", vOld.c_str(), vNew.c_str());
    check(L"switch", compSwitchVersion(c, vNew, err), err);
    Sleep(2000);
    check(L"current is new", compStatus(c).currentVersion == vNew);
    check(L"running new", compIsRunning(c));
    check(L"data in new", queryVer(vNew, L"appdb", L"SELECT v FROM t WHERE id=1;", out)
          && out.find(L"hello") != std::wstring::npos, out);

    wprintf(L"\n=== %s -> %s (back) ===\n", vNew.c_str(), vOld.c_str());
    check(L"switch", compSwitchVersion(c, vOld, err), err);
    Sleep(2000);
    check(L"current is old", compStatus(c).currentVersion == vOld);
    check(L"running old", compIsRunning(c));
    check(L"data in old", queryVer(vOld, L"appdb", L"SELECT v FROM t WHERE id=1;", out)
          && out.find(L"hello") != std::wstring::npos, out);

    wprintf(L"\n=== cleanup ===\n");
    check(L"stop", compStop(c, err), err);
    Sleep(1200);
    check(L"stopped", !compIsRunning(c));

    wprintf(L"\nFAILURES: %d\n", g_fail);
    return g_fail;
}