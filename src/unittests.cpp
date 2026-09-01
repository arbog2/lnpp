// Pure-logic unit tests for LNPP Manager — no processes, no ports, no disk
// writes. Covers renderTemplate, pkgsNameToCompVer, the natural version sort,
// pm2 jlist parsing and the INI text helpers. Build/run via test.bat (target
// "unittests").
#include "common.h"
#include "manager.h"
#include "downloader.h"
#include <algorithm>
#include <cstdio>

static int g_fail = 0;
static void check(const wchar_t* name, bool ok, const std::wstring& detail = L"") {
    wprintf(L"  [%s] %s %s\n", ok ? L"PASS" : L"FAIL", name, detail.c_str());
    if (!ok) ++g_fail;
}

int wmain() {
    wprintf(L"=== renderTemplate ===\n");
    {
        std::map<std::wstring, std::wstring> kv = {{L"host", L"example.com"}, {L"port", L"8080"}};
        check(L"basic substitution", renderTemplate(L"server_name {{host}};", kv) == L"server_name example.com;");
        check(L"unknown placeholder kept", renderTemplate(L"{{host}}/{{missing}}", kv) == L"example.com/{{missing}}");
        check(L"no cascade substitution", renderTemplate(L"{{host}}", {{L"host", L"{{port}}"}, {L"port", L"9"}}) == L"{{port}}");
        check(L"placeholder key trimmed", renderTemplate(L"{{  host  }}", kv) == L"example.com");
        check(L"two placeholders", renderTemplate(L"{{host}}:{{port}}", kv) == L"example.com:8080");
        check(L"empty template", renderTemplate(L"", kv).empty());
        check(L"unclosed placeholder kept literal", renderTemplate(L"{host} {{port", kv) == L"{host} {{port");
    }

    wprintf(L"\n=== pkgsNameToCompVer ===\n");
    {
        std::wstring c, v;
        check(L"nginx", pkgsNameToCompVer(L"nginx-1.30.4", c, v) && c == L"nginx" && v == L"1.30.4");
        check(L"node alias -> nodejs", pkgsNameToCompVer(L"node-24.12", c, v) && c == L"nodejs" && v == L"24.12");
        check(L"postgresql", pkgsNameToCompVer(L"postgresql-17.1", c, v) && c == L"postgresql" && v == L"17.1");
        check(L"redis", pkgsNameToCompVer(L"redis-7.2", c, v) && c == L"redis" && v == L"7.2");
        // ".sha256" suffixes are intentionally parsed as a version here — the
        // caller (pkgsParseConf) must intercept them BEFORE this function.
        check(L"sha256 treated as version by design",
              pkgsNameToCompVer(L"nginx-1.30.4.sha256", c, v) && c == L"nginx" && v == L"1.30.4.sha256");
        check(L"no dash rejected", !pkgsNameToCompVer(L"whatever", c, v));
        check(L"leading dash rejected", !pkgsNameToCompVer(L"-1.0", c, v));
        check(L"unknown component rejected", !pkgsNameToCompVer(L"php-8.0", c, v));
        check(L"empty version rejected", !pkgsNameToCompVer(L"nginx-", c, v));
    }

    wprintf(L"\n=== naturalGt (version sort) ===\n");
    {
        std::vector<std::wstring> v = {L"1.9", L"1.30", L"1.30.1", L"2.0", L"1.10", L"1.30.10"};
        std::sort(v.begin(), v.end(), naturalGt);
        check(L"descending order",
              v == std::vector<std::wstring>({L"2.0", L"1.30.10", L"1.30.1", L"1.30", L"1.10", L"1.9"}));
        check(L"equal values", !naturalGt(L"1.30", L"1.30"));
        check(L"longer suffix wins", naturalGt(L"1.30.1", L"1.30"));
        check(L"tie broken by case", naturalGt(L"B", L"a"));
    }

    wprintf(L"\n=== parsePm2List ===\n");
    {
        std::wstring json = L"[{\"pm_id\":0,\"name\":\"app1\",\"status\":\"online\","
                            L"\"pm2_env\":{\"exec_interpreter\":\"node\",\"restart_time\":3}},"
                            L"{\"pm_id\":1,\"name\":\"app2\",\"status\":\"stopped\"}]";
        auto apps = parsePm2List(json);
        check(L"two apps parsed", apps.size() == 2);
        check(L"app1 fields",
              apps.size() >= 1 && apps[0].id == 0 && apps[0].name == L"app1" &&
              apps[0].status == L"online" && apps[0].interpreter == L"node" && apps[0].restarts == 3);
        check(L"app2 fields",
              apps.size() >= 2 && apps[1].id == 1 && apps[1].name == L"app2" && apps[1].status == L"stopped");
        check(L"nested braces handled", parsePm2List(L"[{\"pm_id\":0,\"name\":\"x\",\"env\":{\"a\":{\"b\":1}}}]").size() == 1);
        check(L"empty array", parsePm2List(L"[]").empty());
        check(L"garbage text", parsePm2List(L"not json").empty());
        check(L"no name -> skipped", parsePm2List(L"[{\"pm_id\":3}]").empty());
    }

    wprintf(L"\n=== parseIniText / serializeIni ===\n");
    {
        auto m = parseIniText(L"key1=val1\r\nKey.Two =  spaced  \r\n\r\n# comment\r\n=orphan\r\n");
        check(L"keys lowercased+trimmed", m.size() == 2 && m[L"key1"] == L"val1" && m[L"key.two"] == L"spaced");
        check(L"comment/blank/orphan lines skipped", m.find(L"# comment") == m.end() && m.find(L"") == m.end());

        std::wstring s = serializeIni(m);
        auto m2 = parseIniText(s);
        check(L"roundtrip preserves values", m2[L"key1"] == L"val1" && m2[L"key.two"] == L"spaced");
        check(L"roundtrip count", m2.size() == 2);
        check(L"empty map serializes to empty", serializeIni({}).empty());
        check(L"crlf line endings", s.find(L"\r\n") != std::wstring::npos);
    }

    wprintf(L"\nFAILURES: %d\n", g_fail);
    return g_fail;
}
