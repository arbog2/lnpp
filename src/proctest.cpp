#include "common.h"
#include "process.h"
#include "manager.h"

static int g_fail = 0;
static void check(const wchar_t* name, bool ok, const std::wstring& detail = L"") {
    wprintf(L"  [%s] %s %s\n", ok ? L"PASS" : L"FAIL", name, detail.c_str());
    if (!ok) ++g_fail;
}

int wmain() {
    std::wstring err;

    wprintf(L"=== Node.js version switch test ===\n");
    // start a test app under current version, then switch version and verify pm2 survives
    check(L"pm2 installed", nodePm2Installed());

    // start current version (resurrect) and verify running
    if (!compIsRunning(Comp::Nodejs)) {
        check(L"start node", compStart(Comp::Nodejs, err), err);
        Sleep(1500);
    }
    check(L"running before switch", compIsRunning(Comp::Nodejs));

    // switch between the two installed node versions (e.g. 24.12 <-> 22.21)
    ComponentStatus st = compStatus(Comp::Nodejs);
    std::wstring from = st.currentVersion;
    std::wstring to;
    for (const auto& v : st.versions) {
        if (v != from) { to = v; break; }
    }
    if (to.empty()) {
        wprintf(L"  only one node version installed, skipping switch\n");
        wprintf(L"\nFAILURES: %d\n", g_fail);
        return g_fail;
    }
    wprintf(L"  switching %s -> %s\n", from.c_str(), to.c_str());

    check(L"switch version", compSwitchVersion(Comp::Nodejs, to, err), err);
    Sleep(2000);
    st = compStatus(Comp::Nodejs);
    std::wstring cur1 = L"current is " + to;
    check(cur1.c_str(), st.currentVersion == to);
    check(L"running after switch", compIsRunning(Comp::Nodejs));

    // switch back
    check(L"switch back", compSwitchVersion(Comp::Nodejs, from, err), err);
    Sleep(2000);
    st = compStatus(Comp::Nodejs);
    std::wstring cur2 = L"current is " + from;
    check(cur2.c_str(), st.currentVersion == from);
    check(L"running after switch back", compIsRunning(Comp::Nodejs));

    // stop
    check(L"stop node", compStop(Comp::Nodejs, err), err);
    Sleep(1500);
    check(L"stopped", !compIsRunning(Comp::Nodejs));

    wprintf(L"\nFAILURES: %d\n", g_fail);
    return g_fail;
}