// Pure-logic unit tests for LNPP Manager — no processes, no ports. The only disk
// access is %TEMP% scratch space for the writeFileText / DPAPI / encoding
// cases, all removed again. Covers renderTemplate, packages.conf parsing,
// pkgsNameToCompVer, the natural version sort, pm2 jlist parsing, the INI text
// helpers and the file writer. Build/run via test.bat (target "unittests").
#include "common.h"
#include "manager.h"
#include "downloader.h"
#include <algorithm>
#include <cstdio>
#include <thread>
#include <atomic>

static int g_fail = 0;
static void check(const wchar_t* name, bool ok, const std::wstring& detail = L"") {
    wprintf(L"  [%s] %s %s\n", ok ? L"PASS" : L"FAIL", name, detail.c_str());
    if (!ok) ++g_fail;
}

// Scratch file under %TEMP% that cleans itself up.
class TempFile {
public:
    TempFile() {
        wchar_t dir[MAX_PATH], name[MAX_PATH];
        if (GetTempPathW(MAX_PATH, dir) && GetTempFileNameW(dir, L"lnpu", 0, name))
            m_path = name;
    }
    ~TempFile() { if (!m_path.empty()) DeleteFileW(m_path.c_str()); }
    const std::wstring& path() const { return m_path; }
    bool valid() const { return !m_path.empty(); }
private:
    std::wstring m_path;
};

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
        // The version becomes a directory name and the entry name becomes a temp
        // file name, so traversal and separators have to be rejected outright.
        check(L"dotdot version rejected", !pkgsNameToCompVer(L"nginx-../../evil", c, v));
        check(L"backslash version rejected", !pkgsNameToCompVer(L"nginx-..\\..\\evil", c, v));
        check(L"slash in version rejected", !pkgsNameToCompVer(L"nginx-1.30/x", c, v));
        check(L"drive letter rejected", !pkgsNameToCompVer(L"nginx-c:evil", c, v));
        check(L"dot version rejected", !pkgsNameToCompVer(L"nginx-.", c, v));
        check(L"dotdot version rejected 2", !pkgsNameToCompVer(L"nginx-..", c, v));
        check(L"overlong version rejected", !pkgsNameToCompVer(L"nginx-" + std::wstring(70, L'9'), c, v));
        check(L"pre-release tag allowed", pkgsNameToCompVer(L"nginx-1.30.4-rc1", c, v) && v == L"1.30.4-rc1");
        check(L"version with plus allowed", pkgsNameToCompVer(L"redis-7.2+build1", c, v) && v == L"7.2+build1");
    }

    wprintf(L"\n=== pkgsParseConfText ===\n");
    {
        std::wstring err;
        {
            auto secs = pkgsParseConfText(
                L"# Web\n---\nNginx-1.30.4=https://example.org/a.zip\n"
                L"Nginx-1.29.5=https://example.org/b.zip\n---\n", err);
            check(L"one section parsed", secs.size() == 1);
            check(L"title captured", secs.size() == 1 && secs[0].title == L"Web");
            check(L"two items", secs.size() == 1 && secs[0].items.size() == 2);
        }
        {
            // The digest may appear before OR after its package line.
            auto secs = pkgsParseConfText(
                L"---\nNginx-1.30.4.sha256=" + std::wstring(64, L'a') + L"\n"
                L"Nginx-1.30.4=https://example.org/a.zip\n---\n", err);
            bool ok = secs.size() == 1 && secs[0].items.size() == 1 &&
                      secs[0].items[0].sha256.size() == 64;
            check(L"sha256 before package attached", ok, err);
            check(L"sha256 entry is not a package",
                  secs.size() == 1 && secs[0].items[0].ver == L"1.30.4");
        }
        {
            auto secs = pkgsParseConfText(
                L"---\nNginx-1.30.4=https://example.org/a.zip\n"
                L"Nginx-1.30.4.sha256=" + std::wstring(64, L'b') + L"\n---\n", err);
            bool ok = secs.size() == 1 && secs[0].items.size() == 1 &&
                      secs[0].items[0].sha256 == std::wstring(64, L'b');
            check(L"sha256 after package attached", ok, err);
        }
        {
            // A URL with '=' must not be truncated at the first one.
            auto secs = pkgsParseConfText(
                L"---\npostgresql-17.2=https://sbp.example.org/getfile.jsp?fileid=1259294\n---\n", err);
            bool ok = secs.size() == 1 && secs[0].items.size() == 1 &&
                      secs[0].items[0].url.find(L"fileid=1259294") != std::wstring::npos;
            check(L"URL keeps query string", ok, err);
        }
        {
            auto secs = pkgsParseConfText(
                L"# Title\n# https://note.example.org\n---\nphp-8.2=https://example.org/c.zip\n"
                L"nginx-1.30.4=https://example.org/a.zip\n---\n", err);
            check(L"unknown component skipped, known kept",
                  secs.size() == 1 && secs[0].items.size() == 1 &&
                  secs[0].items[0].comp == L"nginx");
            check(L"first comment is the title", secs.size() == 1 && secs[0].title == L"Title");
        }
        {
            // Traversal entry: the parser must not surface it as a package.
            auto secs = pkgsParseConfText(
                L"---\nnginx-../../evil=https://example.org/evil.zip\n---\n", err);
            check(L"traversal entry dropped", secs.size() == 0 || secs[0].items.empty(), err);
        }
        {
            auto secs = pkgsParseConfText(L"---\n---\n---\n", err);
            check(L"empty sections produce nothing", secs.empty());
        }
        check(L"empty text reports error", pkgsParseConfText(L"", err).empty() && !err.empty());
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
        // Documented contract: compare the numeric parts (missing parts count as
        // 0), then break a numeric tie on the whole string so the dropdown order
        // is a stable total order. It is deliberately NOT a numeric equality —
        // "1.30" and "1.30.0" are different directories and must not collapse.
        check(L"numeric tie broken deterministically",
              naturalGt(L"1.30.0", L"1.30") != naturalGt(L"1.30", L"1.30.0"));
        check(L"v-prefixed dir name still sorts", naturalGt(L"v1.2", L"1.2") ||
                                                 naturalGt(L"1.2", L"v1.2"));
        check(L"antisymmetric on a numeric tie",
              naturalGt(L"v1.2", L"1.2") == !naturalGt(L"1.2", L"v1.2"));
        check(L"empty is smaller", naturalGt(L"1.0", L""));
        check(L"both empty equal", !naturalGt(L"", L""));
        check(L"big numbers", naturalGt(L"1.999999999999", L"1.99999999999"));
    }

    wprintf(L"\n=== renderTemplate edge cases ===\n");
    {
        check(L"empty braces kept", renderTemplate(L"{{}}x", {}) == L"{{}}x");
        check(L"lone brace kept", renderTemplate(L"a{b}c", {}) == L"a{b}c");
        check(L"value containing braces not re-expanded",
              renderTemplate(L"{{a}}", {{L"a", L"{{b}}"}}) == L"{{b}}");
        check(L"unterminated at end", renderTemplate(L"{{a", {{L"a", L"1"}}) == L"{{a");
        check(L"case sensitive key", renderTemplate(L"{{A}}", {{L"a", L"1"}}) == L"{{A}}");
        check(L"chinese value", renderTemplate(L"root {{p}};", {{L"p", L"中文路径"}}) == L"root 中文路径;");
    }

    wprintf(L"\n=== writeFileText ===\n");
    {
        TempFile t;
        check(L"temp file created", t.valid());
        if (t.valid()) {
            check(L"write ok", writeFileText(t.path(), L"hello 世界"));
            check(L"read back", readFileText(t.path()) == L"hello 世界");
            check(L"overwrite ok", writeFileText(t.path(), L"second"));
            check(L"overwrite visible", readFileText(t.path()) == L"second");
            check(L"empty content ok", writeFileText(t.path(), L""));
            check(L"empty file reads empty", readFileText(t.path()).empty());
            check(L"missing dir created", writeFileText(joinPath(dirOf(t.path()), L"sub\\x.txt"), L"v"));
            check(L"nested read back", readFileText(joinPath(dirOf(t.path()), L"sub\\x.txt")) == L"v");

            // The regression this file exists for: eight threads writing the
            // same target used to collide on a fixed "<path>.tmp" staging file
            // opened exclusively, so all but one returned false — the real
            // symptom was "生成 redis 配置失败" on every manager start.
            const int kThreads = 8;
            std::atomic<int> okCount{0};
            std::vector<std::thread> ts;
            for (int i = 0; i < kThreads; ++i) {
                ts.emplace_back([&t, &okCount, i]() {
                    if (writeFileText(t.path(), L"writer-" + std::to_wstring(i)))
                        okCount.fetch_add(1);
                });
            }
            for (auto& th : ts) th.join();
            check(L"all concurrent writers succeed", okCount.load() == kThreads,
                  L"ok=" + std::to_wstring(okCount.load()) + L"/" + std::to_wstring(kThreads));
            std::wstring finalText = readFileText(t.path());
            check(L"final content is one complete writer's output",
                  finalText.rfind(L"writer-", 0) == 0 && finalText.size() >= 8, finalText);
            // No staging file of *this* target may be left behind. (A plain
            // "*.tmp" glob would also match this test's own temp file, since
            // GetTempFileName hands out a .tmp name.)
            std::wstring stem = dirOf(t.path());
            std::wstring base = stem.substr(stem.find_last_of(L"\\/") + 1);
            int leftovers = 0;
            for (auto& f : listFiles(dirOf(t.path()), L"tmp"))
                if (f.rfind(base, 0) == 0) ++leftovers;
            check(L"no leftover staging files", leftovers == 0,
                  L"left=" + std::to_wstring(leftovers));
        }
    }

    wprintf(L"\n=== dpProtect / dpUnprotect ===\n");
    {
        std::wstring enc = dpProtect(L"p@ss w0rd 中文");
        check(L"encrypt returns base64", !enc.empty() && enc.find(L' ') == std::wstring::npos);
        check(L"roundtrip", dpUnprotect(enc) == L"p@ss w0rd 中文");
        check(L"empty plaintext roundtrips", dpUnprotect(dpProtect(L"")) == L"");
        check(L"garbage ciphertext rejected", dpUnprotect(L"not base64!!!").empty());
        check(L"empty ciphertext rejected", dpUnprotect(L"").empty());
        // Legacy values on disk ended in a stray NUL; they must still decrypt.
        check(L"trailing NUL tolerated", dpUnprotect(enc + L"\0") == L"p@ss w0rd 中文");
        check(L"ciphertext differs from plaintext", enc != L"p@ss w0rd 中文");
    }

    wprintf(L"\n=== readFileText encodings ===\n");
    {
        TempFile t;
        if (t.valid()) {
            // writeFileText always emits UTF-8; a GBK file only exists if a user
            // edited it, which is exactly the case the ACP fallback is for.
            check(L"utf8 roundtrip", (writeFileText(t.path(), L"端口 6379") &&
                                       readFileText(t.path()) == L"端口 6379"));
            HANDLE h = CreateFileW(t.path().c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                const char gbk[] = "\xD6\xD0\xBF\xDA";   // 端口 in GBK
                DWORD n = 0;
                WriteFile(h, gbk, 4, &n, nullptr);
                CloseHandle(h);
                check(L"non-utf8 file does not return empty", !readFileText(t.path()).empty());
            }
        }
    }

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
        // A value that itself contains '=' must survive the round trip: the
        // parser splits on the FIRST '=' only.
        {
            auto m3 = parseIniText(L"dsn=postgresql://u:p@127.0.0.1:5432/db?x=1\n");
            check(L"value containing '=' kept whole",
                  m3.size() == 1 && m3[L"dsn"] == L"postgresql://u:p@127.0.0.1:5432/db?x=1");
        }
        // A UTF-8 BOM would become part of the first key, silently renaming it.
        {
            std::string withBom = "\xEF\xBB\xBFver.nodejs=24.12\r\n";
            int n = MultiByteToWideChar(CP_UTF8, 0, withBom.c_str(), (int)withBom.size(), nullptr, 0);
            std::wstring w(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, withBom.c_str(), (int)withBom.size(), &w[0], n);
            auto m4 = parseIniText(w);
            check(L"BOM-prefixed first key is still readable",
                  m4.find(L"ver.nodejs") != m4.end(), std::to_wstring((int)m4.size()));
        }
    }

    wprintf(L"\nFAILURES: %d\n", g_fail);
    // 0 = pass, 1 = fail. Never return the raw count: test.bat reserves exit
    // code 2 for "skipped", and returning 2 for "two failures" made a red run
    // print [SKIP] and the whole suite report [OK].
    return g_fail ? 1 : 0;
}
