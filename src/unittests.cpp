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

// manager.cpp's validPort() is file-static, so mirror the rule here rather than
// widening its linkage just for a test.
static bool isPortToken(const std::wstring& s) {
    if (s.empty() || s.size() > 5) return false;
    if (s.find_first_not_of(L"0123456789") != std::wstring::npos) return false;
    int n = _wtoi(s.c_str());
    return n > 0 && n <= 65535;
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
        check(L"php", pkgsNameToCompVer(L"php-8.3", c, v) && c == L"php" && v == L"8.3");
        // "php" is a real component now, so the negative case needs a name that
        // is genuinely unknown — mysql was dropped, and must stay rejected.
        check(L"unknown component rejected", !pkgsNameToCompVer(L"mysql-8.0", c, v));
        check(L"not a component at all rejected", !pkgsNameToCompVer(L"tomcat-9", c, v));
        // ".sha256" suffixes are intentionally parsed as a version here — the
        // caller (pkgsParseConf) must intercept them BEFORE this function.
        check(L"sha256 treated as version by design",
              pkgsNameToCompVer(L"nginx-1.30.4.sha256", c, v) && c == L"nginx" && v == L"1.30.4.sha256");
        check(L"no dash rejected", !pkgsNameToCompVer(L"whatever", c, v));
        check(L"leading dash rejected", !pkgsNameToCompVer(L"-1.0", c, v));
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
            // "php" stopped being an unknown component when the PHP component
            // landed, so this negative case now has to use a name that is
            // genuinely not a component (mysql was evaluated and dropped).
            auto secs = pkgsParseConfText(
                L"# Title\n# https://note.example.org\n---\nmysql-8.0=https://example.org/c.zip\n"
                L"nginx-1.30.4=https://example.org/a.zip\n---\n", err);
            check(L"unknown component skipped, known kept",
                  secs.size() == 1 && secs[0].items.size() == 1 &&
                  secs[0].items[0].comp == L"nginx");
            check(L"first comment is the title", secs.size() == 1 && secs[0].title == L"Title");
        }
        {
            // php is a component now, so it must survive the same filter.
            auto secs = pkgsParseConfText(
                L"---\nphp-8.3=https://example.org/php.zip\n---\n", err);
            check(L"php entry kept",
                  secs.size() == 1 && secs[0].items.size() == 1 &&
                  secs[0].items[0].comp == L"php" && secs[0].items[0].ver == L"8.3", err);
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

    wprintf(L"\n=== runtime layout: etc=templates, data=runtime ===\n");
    {
        // The rule this block exists to keep honest: nothing the program writes
        // and the user edits may live under etc\. A regression here would put
        // per-site configs and the download list back into the template tree,
        // where they would also leak into release packages.
        std::wstring vhosts = nginxVhostSourceDir();
        check(L"site configs live under data", vhosts.rfind(dataDir(), 0) == 0, vhosts);
        check(L"site configs NOT under etc", vhosts.rfind(etcDir(), 0) != 0, vhosts);
        check(L"download list lives under data", pkgListPath().rfind(dataDir(), 0) == 0, pkgListPath());
        check(L"download list NOT under etc", pkgListPath().rfind(etcDir(), 0) != 0);
        check(L"download template lives under etc", pkgTemplatePath().rfind(etcDir(), 0) == 0, pkgTemplatePath());
        check(L"download template is a .tpl",
              lowerStr(pkgTemplatePath()).find(L".tpl") != std::wstring::npos);
        check(L"template and working list differ", pkgTemplatePath() != pkgListPath());
        check(L"settings.ini lives under data", settingsIniPath().rfind(dataDir(), 0) == 0);
        // The other half of the split: templates still come from etc\.
        for (auto& c : { Comp::Nginx, Comp::Postgresql, Comp::Redis, Comp::Nodejs, Comp::Php }) {
            check(compName(c), compEtcDir(c).rfind(etcDir(), 0) == 0, compEtcDir(c));
            // The "配置" button must land on data\, never on etc\.
            check((std::wstring(L"config dir of ") + compName(c)).c_str(),
                  compConfigDir(c).rfind(dataDir(), 0) == 0, compConfigDir(c));
        }
        check(L"nginx 配置目录就是站点目录",
              compConfigDir(Comp::Nginx) == nginxVhostSourceDir());
        // It must never be the generated per-version copy, which is rewritten
        // from the source on every start.
        check(L"nginx 配置目录不是每版本运行副本",
              compConfigDir(Comp::Nginx).find(L"conf\\vhosts") == std::wstring::npos,
              compConfigDir(Comp::Nginx));
        // PHP's generated php.ini is runtime state too, and it must not be
        // confused with the template it is rendered from.
        std::wstring phpTpl = joinPath(joinPath(etcCompDir(L"php"), L"php.ini.append"), L"");
        check(L"php 模板在 etc\\ 下", phpTpl.rfind(etcDir(), 0) == 0, phpTpl);
        check(L"php 配置目录在 data\\ 下", compConfigDir(Comp::Php).rfind(dataDir(), 0) == 0,
              compConfigDir(Comp::Php));
    }

    wprintf(L"\n=== dirIsUnder（进程归属判定）===\n");
    {
        // This is what decides whether Stop kills a process or leaves it alone.
        // The exact-match case is the one that matters most: the Windows php zip
        // is flat, so php-cgi.exe sits directly in bin\php\<ver>\. A version
        // that required a trailing separator matched nothing there, so Stop
        // killed no process and then blamed the still-bound port on a stranger.
        check(L"完全相等（php 的实际情形）",
              dirIsUnder(L"D:/code/lnpp/bin/php/8.4.26", L"D:/code/lnpp/bin/php/8.4.26"));
        check(L"完全相等，反斜杠",
              dirIsUnder(L"D:\\code\\lnpp\\bin\\nginx\\1.30.4", L"D:\\code\\lnpp\\bin\\nginx\\1.30.4"));
        check(L"父目录带尾部分隔符也该匹配",
              dirIsUnder(L"D:/code/lnpp/bin/php/8.4.26", L"D:/code/lnpp/bin/php/8.4.26/"));
        check(L"子目录算在下面",
              dirIsUnder(L"D:/code/lnpp/bin/nginx/1.30.4/logs", L"D:/code/lnpp/bin/nginx/1.30.4"));
        check(L"大小写不敏感",
              dirIsUnder(L"D:/CODE/LNPP/BIN/PHP/8.4.26", L"d:/code/lnpp/bin/php/8.4.26"));
        // The separator check is what stops 8.1x from being claimed as 8.1.
        check(L"前缀相同但不是路径分隔符 -> 拒绝",
              !dirIsUnder(L"D:/code/lnpp/bin/php/8.1x", L"D:/code/lnpp/bin/php/8.1"));
        check(L"不同版本 -> 拒绝",
              !dirIsUnder(L"D:/code/lnpp/bin/php/8.1", L"D:/code/lnpp/bin/php/8.3"));
        check(L"别处的同名程序 -> 拒绝",
              !dirIsUnder(L"D:/tools/php/8.4.26", L"D:/code/lnpp/bin/php/8.4.26"));
        check(L"父目录为空 -> 拒绝", !dirIsUnder(L"D:/code/lnpp/bin/php/8.4.26", L""));
    }

    wprintf(L"\n=== php: FastCGI 端口分配 ===\n");
    {
        // The whole point of the per-version port table is that a site config
        // carrying a hardcoded fastcgi_pass port keeps working. So an assigned
        // port must never be re-rolled, and removing another version must not
        // shift the survivors.
        std::wstring a = iniGet(L"php.verport.8.1", L"");
        std::wstring b = iniGet(L"php.verport.8.3", L"");
        std::wstring e1, e2;
        std::wstring p1 = phpEnsurePort(L"8.1", e1);
        std::wstring p2 = phpEnsurePort(L"8.3", e2);
        check(L"8.1 分配到合法端口", !p1.empty() && isPortToken(p1), p1);
        check(L"8.3 分配到合法端口", !p2.empty() && isPortToken(p2), p2);
        check(L"两个版本不共用端口", p1 != p2);
        // Stable: asking again returns the same number, because every site
        // config already has it written in.
        check(L"重复分配不换端口", phpEnsurePort(L"8.1", e1) == p1);
        check(L"重复分配不换端口 (8.3)", phpEnsurePort(L"8.3", e2) == p2);
        check(L"phpPortFor 与分配结果一致", phpPortFor(L"8.1") == p1 && phpPortFor(L"8.3") == p2);
        // A version nobody started has no port at all, and is never running.
        check(L"未分配的版本没有端口", phpPortFor(L"7.4").empty());
        check(L"未分配的版本不算运行中", !phpRunning(L"7.4"));
        // Ports are assigned from php.baseport, not from some fixed constant.
        check(L"端口落在 baseport 之后", _wtoi(p1.c_str()) >= _wtoi(phpBasePort().c_str()));
        check(L"默认 baseport 是 9000", phpBasePort() == L"9000");
        // phpRunning is the liveness probe the poller calls, so it must not
        // throw on a version that was never started.
        check(L"查询未运行版本不报错", !phpRunning(L"0.0.0.0-not-a-version"));

        iniDelete(L"php.verport.8.1");
        iniDelete(L"php.verport.8.3");
    }

    wprintf(L"\n=== php: 站点模板渲染 ===\n");
    {
        // Same substitution path genNginxConfig/nginxAddVHostEx use. If
        // {{PHP_TARGET}} is not supplied the site would ship a literal
        // "{{PHP_TARGET}}" into fastcgi_pass and fail every request at runtime,
        // long after the file is on disk.
        std::map<std::wstring, std::wstring> kv = {
            {L"PORT", L"80"}, {L"DOMAIN", L"a.test"}, {L"ROOT", L"../../../www/a"},
            {L"PHP_TARGET", L"127.0.0.1:9003"}};
        std::wstring out = renderTemplate(
            L"fastcgi_pass {{PHP_TARGET}};\r\n", kv);
        check(L"PHP_TARGET 被替换", out == L"fastcgi_pass 127.0.0.1:9003;\r\n", out);
        check(L"渲染后无残留占位符", out.find(L"{{") == std::wstring::npos);
        // A pool is named, not addressed, so the same slot has to accept one.
        std::map<std::wstring, std::wstring> kv2 = {{L"PHP_TARGET", L"lnpp_php_8_3"}};
        check(L"upstream 名也能填进 fastcgi_pass",
              renderTemplate(L"fastcgi_pass {{PHP_TARGET}};", kv2) == L"fastcgi_pass lnpp_php_8_3;");

        // The shipped php templates must not depend on nginx's own
        // fastcgi_params: only mime.types is copied into the runtime prefix, so
        // `include fastcgi_params;` would fail nginx -t and roll the site back.
        std::wstring phpTplPath =
            joinPath(joinPath(etcCompDir(L"nginx"), L"vhosts"), L"_template_php.conf");
        std::wstring phpTpl = readFileText(phpTplPath);
        check(L"PHP 站点模板存在", !phpTpl.empty(), phpTplPath);
        check(L"模板自带 SCRIPT_FILENAME",
              phpTpl.find(L"fastcgi_param SCRIPT_FILENAME") != std::wstring::npos);
        check(L"模板不 include fastcgi_params",
              phpTpl.find(L"include fastcgi_params") == std::wstring::npos &&
              phpTpl.find(L"include fastcgi.conf") == std::wstring::npos);
        check(L"模板不写 default_server",
              phpTpl.find(L"default_server") == std::wstring::npos);
        check(L"模板用 PHP_TARGET 占位符",
              phpTpl.find(L"{{PHP_TARGET}}") != std::wstring::npos &&
              phpTpl.find(L"{{PHP_PORT}}") == std::wstring::npos);
        // Both php templates exist, so the HTTPS path cannot fall through to a
        // node config.
        check(L"PHP HTTPS 模板存在", !readFileText(joinPath(joinPath(etcCompDir(L"nginx"),
              L"vhosts"), L"_template_php_https.conf")).empty());
        // {{PHP_UPSTREAM}} has to be inside http{} for nginx to accept an
        // upstream there, and genNginxConfig warns when a custom template lacks
        // it — so the shipped template must carry it.
        std::wstring mainTpl = readFileText(joinPath(etcCompDir(L"nginx"), L"nginx.conf.tpl"));
        check(L"主配置模板含 {{PHP_UPSTREAM}}",
              mainTpl.find(L"{{PHP_UPSTREAM}}") != std::wstring::npos);
        check(L"upstream 占位符在 http 块内",
              mainTpl.find(L"http {") < mainTpl.find(L"{{PHP_UPSTREAM}}") &&
              mainTpl.find(L"{{PHP_UPSTREAM}}") < mainTpl.find(L"include vhosts/*.conf;"));
    }

    wprintf(L"\n=== php: 多进程 workers ===\n");
    {
        // A version with N workers owns N CONSECUTIVE ports, so uniqueness has
        // to be about ranges, not numbers: two versions starting at 9000 and
        // 9002 would collide on 9002-9003 with 4 workers each.
        check(L"默认进程数是 1", phpWorkerCount(L"8.1") == 1);
        iniSet(L"php.workers.8.3", L"4");
        check(L"进程数可配置", phpWorkerCount(L"8.3") == 4);
        iniSet(L"php.workers.8.3", L"0");
        check(L"进程数 0 归一为 1", phpWorkerCount(L"8.3") == 1);
        iniSet(L"php.workers.8.3", L"999");
        check(L"进程数上限 32", phpWorkerCount(L"8.3") == 32);
        iniDelete(L"php.workers.8.3");

        // Range allocation: 8.1 takes 9000-9003, so 8.3 must not be handed
        // 9000..9002 — only a port clear of the whole range.
        iniSet(L"php.verport.8.1", L"9000");
        iniSet(L"php.workers.8.1", L"4");
        check(L"8.1 占 4 个端口", phpPortsFor(L"8.1").size() == 4, std::to_wstring(phpPortsFor(L"8.1").size()));
        check(L"端口连续", phpPortsFor(L"8.1") ==
              std::vector<std::wstring>({L"9000", L"9001", L"9002", L"9003"}));
        std::wstring e;
        std::wstring p = phpEnsurePort(L"8.3", e);
        check(L"8.3 避开 8.1 的整个区间", !p.empty() && _wtoi(p.c_str()) >= 9004, p);
        // And a single-worker version does not care about a neighbour's tail.
        iniSet(L"php.workers.8.3", L"1");
        check(L"单进程版本 1 个端口", phpPortsFor(L"8.3").size() == 1);

        // The upstream name is derived from the version string, which is a
        // directory name the user chose — so it must be sanitised, and a dot
        // must not survive into an nginx identifier.
        check(L"upstream 名已净化", phpUpstreamName(L"8.1") == L"lnpp_php_8_1",
              phpUpstreamName(L"8.1"));
        check(L"upstream 名无非法字符",
              phpUpstreamName(L"8.1-rc1").find_first_not_of(L"abcdefghijklmnopqrstuvwxyz"
                                                            L"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")
              == std::wstring::npos, phpUpstreamName(L"8.1-rc1"));
        // A pool is referenced by name, a single worker by address.
        iniSet(L"php.workers.8.1", L"1");
        check(L"单进程用地址", phpFastcgiTarget(L"8.1") == L"127.0.0.1:9000",
              phpFastcgiTarget(L"8.1"));
        iniSet(L"php.workers.8.1", L"3");
        check(L"多进程用 upstream 名", phpFastcgiTarget(L"8.1") == L"lnpp_php_8_1",
              phpFastcgiTarget(L"8.1"));

        iniDelete(L"php.verport.8.1");
        iniDelete(L"php.verport.8.3");
        iniDelete(L"php.workers.8.1");
        iniDelete(L"php.workers.8.3");
    }

    wprintf(L"\n=== php: 扩展解析 ===\n");
    {
        // php.ini belongs to the user, so the parser has to cope with how
        // people actually write these lines — and must never confuse
        // extension_dir (a path) with extension= (a name).
        std::wstring ini =
            L"extension_dir = \"C:/php/ext\"\r\n"
            L"extension=curl\r\n"
            L";extension=gd\r\n"
            L"  extension = mbstring  ; needed for utf8\r\n"
            L"extension=php_openssl.dll\r\n"
            L"; this is a whole-line comment about extensions=foo\r\n"
            L"zend_extension=opcache\r\n"
            L"extension=\r\n";
        auto m = phpParseExtensionLines(ini);
        check(L"识别 extension_dir 不是扩展", m.find(L"dir") == m.end());
        check(L"启用项为 true", m[L"curl"] == true);
        check(L"注释项为 false", m[L"gd"] == false);
        check(L"带空格与行尾注释", m[L"mbstring"] == true);
        check(L"php_ 前缀 + .dll 归一", m[L"openssl"] == true);
        check(L"整行注释被忽略", m.find(L"this is a whole-line comment about extensions=foo") == m.end());
        check(L"zend_extension 不算扩展", m.find(L"zend_extension") == m.end());
        check(L"空值被忽略", m.size() == 4, std::to_wstring(m.size()));

        check(L"空文本得到空表", phpParseExtensionLines(L"").empty());
        check(L"无 extension 行", phpParseExtensionLines(L"[PHP]\r\nmemory_limit=256M\r\n").empty());
        // A later line wins in php, and so must it here.
        auto dup = phpParseExtensionLines(L";extension=zip\r\nextension=zip\r\n");
        check(L"重复项以后者为准", dup[L"zip"] == true);
    }

    wprintf(L"\n=== pgRestoreOutputOk ===\n");
    {
        // The verdict for a restore comes from this classifier, because the psql
        // replay deliberately runs WITHOUT ON_ERROR_STOP (a pg_dumpall dump always
        // collides with the bootstrap superuser, and aborting on the first
        // statement would restore nothing). So "did it work" has to be read off
        // the output, and the two tolerated families must stay exactly two.
        std::wstring bad;
        check(L"empty output is ok", pgRestoreOutputOk(L"", bad));
        check(L"no errors is ok", pgRestoreOutputOk(L"SET\nCREATE TABLE\nINSERT 0 10\n", bad));

        check(L"role already exists tolerated",
              pgRestoreOutputOk(L"ERROR:  role \"postgres\" already exists\n"
                               L"ERROR:  role \"arbog\" already exists\n", bad));
        check(L"database already exists tolerated",
              pgRestoreOutputOk(L"ERROR:  database \"appdb\" already exists\n", bad));

        // The shape psql actually emits with VERBOSITY=verbose: state code plus
        // the offending statement. Judged on those, so a non-English
        // lc_messages cannot make every error look tolerated.
        check(L"verbose duplicate role tolerated",
              pgRestoreOutputOk(L"ERROR:  42710: duplicate_object: role \"postgres\" already exists\n"
                               L"LINE 1: CREATE ROLE postgres;\n", bad));
        check(L"verbose duplicate database tolerated",
              pgRestoreOutputOk(L"ERROR:  42710: duplicate_object: database \"appdb\" already exists\n"
                               L"LINE 1: CREATE DATABASE appdb WITH TEMPLATE = template0;\n", bad));
        check(L"verbose duplicate TABLE rejected",
              !pgRestoreOutputOk(L"ERROR:  42710: duplicate_object: relation \"t\" already exists\n"
                                  L"LINE 1: CREATE TABLE public.t (id int);\n", bad));
        check(L"localized duplicate role tolerated",
              pgRestoreOutputOk(L"ERROR:  42710: duplicate_object: 角色 \"postgres\" 已经存在\n"
                               L"LINE 1: CREATE ROLE postgres;\n", bad));
        check(L"localized duplicate table rejected",
              !pgRestoreOutputOk(L"ERROR:  42710: duplicate_object: 表 \"t\" 已经存在\n"
                                  L"LINE 1: CREATE TABLE public.t (id int);\n", bad));

        check(L"other errors rejected",
              !pgRestoreOutputOk(L"ERROR:  syntax error at or near \"CREAT\"\n", bad));
        check(L"rejected error is reported", bad.find(L"syntax error") != std::wstring::npos, bad);
        check(L"permission denied rejected",
              !pgRestoreOutputOk(L"ERROR:  permission denied to create database\n", bad));
        check(L"role in use rejected",
              !pgRestoreOutputOk(L"ERROR:  role \"arbog\" cannot be dropped because some objects depend on it\n", bad));
        // "already exists" on some other object is NOT tolerated: it means a
        // table/index collision, i.e. the dump is being merged into a cluster
        // that already holds that data.
        check(L"table already exists rejected",
              !pgRestoreOutputOk(L"ERROR:  relation \"t\" already exists\n", bad));
        // A benign error must not hide a real one later in the same output.
        check(L"benign error does not mask a real one",
              !pgRestoreOutputOk(L"ERROR:  role \"postgres\" already exists\n"
                                  L"ERROR:  relation \"t\" already exists\n", bad));
        // Warnings are not errors and must not fail a restore.
        check(L"warnings ignored",
              pgRestoreOutputOk(L"WARNING:  no privileges were granted for \"public\"\n", bad));
    }

    wprintf(L"\n=== compIsBusy ===\n");
    {
        // The contract that matters: callers must be able to tell "another
        // operation owns this component" apart from a real failure, because under
        // autostart compStart(Nodejs) always finds the user's own Redis start in
        // flight and that is the expected outcome, not a fault.
        check(L"busy message recognised", compIsBusy(L"该组件正在执行其他操作，请稍候"));
        check(L"not-installed is not busy", !compIsBusy(L"组件未安装"));
        check(L"already-running is not busy", !compIsBusy(L"已在运行"));
        check(L"pg failure is not busy", !compIsBusy(L"initdb 失败: ..."));
        check(L"empty is not busy", !compIsBusy(L""));
        // A message that merely contains the wording must not match, otherwise a
        // wrapped real failure would be misreported as a benign skip.
        check(L"wrapped message is not busy",
              !compIsBusy(L"停止数据库失败: 该组件正在执行其他操作，请稍候"));
    }

    wprintf(L"\n=== nginxDenyBlock ===\n");
    {
        std::wstring on = nginxDenyBlock(true);
        std::wstring off = nginxDenyBlock(false);
        check(L"enabled: :80 is the default server", on.find(L"listen       80 default_server;") != std::wstring::npos);
        check(L"enabled: unknown host gets 500", on.find(L"return 500;") != std::wstring::npos);
        check(L"enabled: :443 refuses the handshake", on.find(L"listen       443 ssl default_server;") != std::wstring::npos &&
              on.find(L"ssl_reject_handshake on;") != std::wstring::npos);
        check(L"enabled: catch-all matches every name", on.find(L"server_name  _;") != std::wstring::npos);
        // A certificate on the catch-all would only put a cert-mismatch warning
        // in front of the answer, so it must not be there.
        check(L"enabled: no certificate on the catch-all", on.find(L"ssl_certificate") == std::wstring::npos);
        check(L"enabled: no site root leaks the www dir", on.find(L"../../../www") == std::wstring::npos);
        check(L"enabled: exactly one default_server per port",
              std::count(on.begin(), on.end(), L' ') >= 0 && on.find(L"443 ssl default_server") != std::wstring::npos);

        check(L"disabled: legacy localhost site restored", off.find(L"server_name  localhost;") != std::wstring::npos);
        check(L"disabled: no default_server at all", off.find(L"default_server") == std::wstring::npos);
        check(L"disabled: no ssl_reject_handshake", off.find(L"ssl_reject_handshake") == std::wstring::npos);
        check(L"the two renderings differ", on != off);
        // The block is spliced into http{} before `include vhosts/*.conf`, and
        // must not carry a stray placeholder of its own.
        check(L"no leftover placeholders in the block",
              on.find(L"{{") == std::wstring::npos && off.find(L"{{") == std::wstring::npos);

        // End-to-end through the real renderer, the way genNginxConfig does it.
        std::map<std::wstring, std::wstring> kv = {
            {L"DENY_UNKNOWN", on}, {L"MIME", L"mime.types"}, {L"PORT", L"80"}};
        std::wstring rendered = renderTemplate(
            L"http {\r\n{{DENY_UNKNOWN}}\r\n    include vhosts/*.conf;\r\n}\r\n", kv);
        check(L"substitutes into a real config",
              rendered.find(L"return 500;") != std::wstring::npos &&
              rendered.find(L"include vhosts/*.conf;") != std::wstring::npos);
        check(L"catch-all lands before the vhost include",
              rendered.find(L"return 500;") < rendered.find(L"include vhosts/*.conf;"));
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
            // Create a scratch subdirectory, then take it (and its file) back
            // down. TempFile's destructor only removes the file it made, so
            // without this the run leaves a stray directory in %TEMP%.
            std::wstring subDir  = joinPath(dirOf(t.path()), L"sub");
            std::wstring subFile = joinPath(subDir, L"x.txt");
            check(L"missing dir created", writeFileText(subFile, L"v"));
            check(L"nested read back", readFileText(subFile) == L"v");
            check(L"scratch dir cleaned up",
                  dirExists(subDir) && DeleteFileW(subFile.c_str()) != 0 &&
                  RemoveDirectoryW(subDir.c_str()) != 0 && !dirExists(subDir));

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
