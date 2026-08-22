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
    Comp c = Comp::Postgresql;
    std::wstring err, out;

    // ensure seeded: appdb.t(id=1,'hello')
    wprintf(L"=== ensure data ===\n");
    check(L"current is 17", compStatus(c).currentVersion == L"17");
    if (!compIsRunning(c)) { check(L"start 17", compStart(c, err), err); Sleep(2000); }
    check(L"seed data present", queryVer(L"17", L"appdb", L"SELECT v FROM t WHERE id=1;", out)
          && out.find(L"hello") != std::wstring::npos, out);

    wprintf(L"\n=== 17 -> 18 ===\n");
    check(L"switch", compSwitchVersion(c, L"18", err), err);
    Sleep(2000);
    check(L"current 18", compStatus(c).currentVersion == L"18");
    check(L"running 18", compIsRunning(c));
    check(L"data in 18", queryVer(L"18", L"appdb", L"SELECT v FROM t WHERE id=1;", out)
          && out.find(L"hello") != std::wstring::npos, out);

    wprintf(L"\n=== 18 -> 17 (back) ===\n");
    check(L"switch", compSwitchVersion(c, L"17", err), err);
    Sleep(2000);
    check(L"current 17", compStatus(c).currentVersion == L"17");
    check(L"running 17", compIsRunning(c));
    check(L"data in 17", queryVer(L"17", L"appdb", L"SELECT v FROM t WHERE id=1;", out)
          && out.find(L"hello") != std::wstring::npos, out);

    wprintf(L"\n=== cleanup ===\n");
    check(L"stop", compStop(c, err), err);
    Sleep(1200);
    check(L"stopped", !compIsRunning(c));

    wprintf(L"\nFAILURES: %d\n", g_fail);
    return g_fail;
}