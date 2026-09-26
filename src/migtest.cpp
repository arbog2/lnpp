// DESTRUCTIVE integration test. See test.bat: it migrates the REAL PostgreSQL
// cluster under this directory (pg_dumpall -> initdb -> psql -f), which rewrites
// data\postgresql\<ver>. Run it only when you have a backup, and only on purpose.
#include "common.h"
#include "proc.h"
#include "manager.h"

// 0 = pass, 1 = fail, 2 = skipped. test.bat prints [SKIP] for 2 — a test that
// changed nothing must never be reported as green.
#define RC_PASS 0
#define RC_FAIL 1
#define RC_SKIP 2

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

// Run SQL through a temp file, like the manager's own pgRunSql. The old test
// assumed appdb.t already existed on the machine it was written on, so on any
// other box the very first assertion failed for a reason that had nothing to do
// with migrations. The test now creates its own seed row.
static bool execVer(const std::wstring& ver, const std::wstring& db, const std::wstring& sql) {
    std::wstring psql = joinPath(joinPath(compBinDirVer(Comp::Postgresql, ver), L"bin"), L"psql.exe");
    wchar_t tmpDir[MAX_PATH], tmpName[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmpDir) || !GetTempFileNameW(tmpDir, L"lnpmt", 0, tmpName))
        return false;
    std::wstring f = tmpName;
    if (!writeFileText(f, L"\\encoding UTF8\r\n" + sql + L"\r\n")) { DeleteFileW(f.c_str()); return false; }
    RunResult r = runProcessCapture(psql,
        L"-h 127.0.0.1 -p " + pgPort() + L" -U " + pgUser() + L" -d " + db +
        L" -v ON_ERROR_STOP=1 -f \"" + f + L"\"",
        compBinDirVer(Comp::Postgresql, ver), 30000, {{L"PGPASSWORD", pgPassword()}});
    DeleteFileW(f.c_str());
    return r.ok;
}

int wmain() {
    Comp c = Comp::Postgresql;
    std::wstring err, out;

    wprintf(L"!!! 破坏性测试：将重建真实的 PostgreSQL 集群（%s）\n", compDataDir(c).c_str());

    // Pick the two installed PG versions automatically (compVersions is
    // sorted newest-first), so the test doesn't hardcode 17/18.
    std::vector<std::wstring> vers = compVersions(c);
    if (vers.size() < 2) {
        wprintf(L"需要至少两个已安装的 PostgreSQL 版本才能做迁移测试（已装 %d 个），跳过。\n", (int)vers.size());
        wprintf(L"\nSKIPPED\n");
        return RC_SKIP;
    }
    std::wstring vNew = vers[0];
    std::wstring vOld = vers[1];

    // ensure seeded: appdb.t(id=1,'hello') — start from the old version
    wprintf(L"=== ensure data (on %s) ===\n", vOld.c_str());
    if (compStatus(c).currentVersion != vOld)
        check((L"switch to " + vOld).c_str(), compSwitchVersion(c, vOld, err), err);
    if (!compIsRunning(c)) { check((L"start " + vOld).c_str(), compStart(c, err), err); Sleep(2000); }
    bool seeded = execVer(vOld, L"appdb", L"CREATE TABLE IF NOT EXISTS t (id int primary key, v text);"
                                            L"INSERT INTO t (id, v) VALUES (1, 'hello') ON CONFLICT (id) DO NOTHING;");
    // appdb may not exist on this machine; fall back to the always-present
    // postgres database with a test-specific table name.
    std::wstring seedDb = L"appdb";
    std::wstring seedSql = L"SELECT v FROM t WHERE id=1;";
    if (!seeded) {
        wprintf(L"  appdb 不可用，改用 postgres 库\n");
        seedDb = L"postgres";
        seedSql = L"SELECT v FROM lnpp_mt_t WHERE id=1;";
        seeded = execVer(vOld, seedDb, L"CREATE TABLE IF NOT EXISTS lnpp_mt_t (id int primary key, v text);"
                                         L"INSERT INTO lnpp_mt_t (id, v) VALUES (1, 'hello') ON CONFLICT (id) DO NOTHING;");
    }
    check(L"seed data written", seeded);
    check(L"seed data present",
          queryVer(vOld, seedDb, seedSql, out) && out.find(L"hello") != std::wstring::npos, out);

    wprintf(L"\n=== %s -> %s ===\n", vOld.c_str(), vNew.c_str());
    check(L"switch", compSwitchVersion(c, vNew, err), err);
    Sleep(2000);
    check(L"current is new", compStatus(c).currentVersion == vNew);
    check(L"running new", compIsRunning(c));
    check(L"data in new", queryVer(vNew, seedDb.c_str(), seedSql.c_str(), out) &&
          out.find(L"hello") != std::wstring::npos, out);

    wprintf(L"\n=== %s -> %s (back) ===\n", vNew.c_str(), vOld.c_str());
    check(L"switch back", compSwitchVersion(c, vOld, err), err);
    Sleep(2000);
    check(L"current is old", compStatus(c).currentVersion == vOld);
    check(L"running old", compIsRunning(c));
    check(L"data in old", queryVer(vOld, seedDb.c_str(), seedSql.c_str(), out) &&
          out.find(L"hello") != std::wstring::npos, out);

    wprintf(L"\n=== cleanup ===\n");
    check(L"stop", compStop(c, err), err);
    Sleep(1200);
    check(L"stopped", !compIsRunning(c));

    wprintf(L"\nFAILURES: %d\n", g_fail);
    return g_fail ? RC_FAIL : RC_PASS;
}
