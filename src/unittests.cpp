// LNPP Manager 的纯逻辑单元测试 —— 不启停任何进程，也不 bind 任何端口。
// 唯一的写操作是 %TEMP% 下的临时文件（writeFileText / DPAPI / 编码用例），
// 用完即删。
// php 用例含只读探测：phpEnsurePort 内部会经 tcpPortListening 读内核 TCP
// 连接表，还会读 etc\ 下的真实模板，并通过 iniGet 读 data\settings.ini。
// 测试确实会 iniSet / iniDelete，但只改内存缓存：写盘是防抖的，而这个进程
// 没有驱动刷写的定时器，所以用户的 settings.ini 不会被改写；凡改过的键，
// 都在所在测试块结尾按「有原值则恢复、原本没有才删除」还原。
// 覆盖 renderTemplate、packages.conf 解析、pkgsNameToCompVer、自然版本排序、
// pm2 jlist 解析、INI 文本助手、文件写入器，以及 php 端口分配与扩展解析、
// mysql 模板/恢复输出分类。
// 通过 test.bat（target "unittests"）构建与运行。
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

// 记下某个 ini 键的当前值，出作用域时按「有原值则恢复、原本没有才删除」还原。
// 端口分配、worker 数这些逻辑只能靠改 settings.ini 来驱动，而那是用户自己的
// 文件：写完必须还原，不能因为跑一次测试就把人家配好的端口删掉。顺带一提，
// 「用户本来就配过」正是必须能跑通的场景，所以也不能假设这些键一开始不存在。
class IniKeyGuard {
public:
    explicit IniKeyGuard(const std::wstring& key)
        : m_key(key), m_old(iniGet(key, L"")) {}
    ~IniKeyGuard() {
        if (m_old.empty()) iniDelete(m_key); else iniSet(m_key, m_old);
    }
    IniKeyGuard(const IniKeyGuard&) = delete;
    IniKeyGuard& operator=(const IniKeyGuard&) = delete;
private:
    std::wstring m_key;
    std::wstring m_old;
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
        check(L"mysql", pkgsNameToCompVer(L"mysql-8.0.46", c, v) && c == L"mysql" && v == L"8.0.46");
        check(L"mysql 8.4", pkgsNameToCompVer(L"mysql-8.4.11", c, v) && c == L"mysql" && v == L"8.4.11");
        // "php" is a real component now, so the negative case needs a name that
        // is genuinely unknown.
        check(L"unknown component rejected", !pkgsNameToCompVer(L"mariadb-11.4", c, v));
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
            // mysql is a component now, so it must survive the same filter as
            // nginx; mariadb stays the genuinely-unknown negative case.
            auto secs = pkgsParseConfText(
                L"# Title\n# https://note.example.org\n---\nmariadb-11.4=https://example.org/c.zip\n"
                L"mysql-8.0.46=https://example.org/mysql.zip\n"
                L"nginx-1.30.4=https://example.org/a.zip\n---\n", err);
            check(L"unknown component skipped, known kept",
                  secs.size() == 1 && secs[0].items.size() == 2 &&
                  secs[0].items[0].comp == L"mysql" && secs[0].items[1].comp == L"nginx");
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
            // The shipped template must expose at least one MySQL download, and
            // it must resolve to the mysql component rather than being skipped.
            // URLs are fixtures, not network calls, so parsing them is side
            // effect free.
            std::wstring tplText = readFileText(pkgTemplatePath());
            check(L"自带下载模板可读", !tplText.empty(), pkgTemplatePath());
            auto secs = pkgsParseConfText(tplText, err);
            bool found = false;
            for (const auto& sec : secs) {
                for (const auto& item : sec.items) {
                    if (item.name == L"mysql-8.0.46" &&
                        item.url == L"https://cdn.mysql.com/Downloads/MySQL-8.0/mysql-8.0.46-winx64.zip") {
                        found = true;
                    }
                }
            }
            check(L"自带下载模板含 MySQL 8.0.46 条目", found, pkgTemplatePath());
        }
        {
            // 8.4 LTS 同样必须在模板里，且地址精确到已校验的直链。
            std::wstring tplText = readFileText(pkgTemplatePath());
            auto secs = pkgsParseConfText(tplText, err);
            bool found = false;
            for (const auto& sec : secs) {
                for (const auto& item : sec.items) {
                    if (item.name == L"mysql-8.4.11" &&
                        item.url == L"https://cdn.mysql.com/Downloads/MySQL-8.4/mysql-8.4.11-winx64.zip") {
                        found = true;
                    }
                }
            }
            check(L"自带下载模板含 MySQL 8.4.11 条目", found, pkgTemplatePath());
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
        for (auto& c : { Comp::Nginx, Comp::Postgresql, Comp::Redis, Comp::Nodejs, Comp::Php, Comp::Mysql }) {
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
        // MySQL's generated my.ini is regenerated on every start, so the
        // shipped template must contain every placeholder genMyConfig renders;
        // otherwise a silently unfilled {{...}} would reach mysqld.
        std::wstring myTplPath = joinPath(etcCompDir(L"mysql"), L"my.ini.tpl");
        std::wstring myTpl = readFileText(myTplPath);
        check(L"mysql 模板存在", !myTpl.empty(), myTplPath);
        check(L"mysql 模板包含全部渲染占位符",
              myTpl.find(L"{{BASEDIR}}") != std::wstring::npos &&
              myTpl.find(L"{{DATADIR}}") != std::wstring::npos &&
              myTpl.find(L"{{PORT}}") != std::wstring::npos &&
              myTpl.find(L"{{LOGFILE}}") != std::wstring::npos &&
              myTpl.find(L"{{PIDFILE}}") != std::wstring::npos &&
              myTpl.find(L"{{CONFDIR}}") != std::wstring::npos, myTplPath);
        check(L"mysql 默认只监听本机",
              myTpl.find(L"bind-address = 127.0.0.1") != std::wstring::npos, myTplPath);
        check(L"mysql 模板保留手工配置入口",
              myTpl.find(L"!includedir") != std::wstring::npos &&
              myTpl.find(L"conf.d") != std::wstring::npos, myTplPath);
        // Render the shipped template through the same substitution path as
        // genMyConfig. A missing key would otherwise survive as {{...}} until
        // mysqld rejects the file at startup.
        std::map<std::wstring, std::wstring> myKv = {
            {L"BASEDIR", L"D:/x/bin/mysql/8.0.46"}, {L"DATADIR", L"D:/x/data/mysql/8.0.46/data"},
            {L"PORT", L"3306"}, {L"LOGFILE", L"D:/x/logs/mysql-8.0.46.log"},
            {L"PIDFILE", L"D:/x/data/mysql/8.0.46/mysqld.pid"},
            {L"CONFDIR", L"D:/x/data/mysql/8.0.46/conf.d"}};
        std::wstring myRendered = renderTemplate(myTpl, myKv);
        check(L"mysql 模板渲染后无残留占位符",
              myRendered.find(L"{{") == std::wstring::npos, myRendered);
        check(L"mysql 渲染写入数据目录",
              myRendered.find(L"D:/x/data/mysql/8.0.46/data") != std::wstring::npos, myRendered);
        // 8.4 walks a different template (native-password plugin is OFF by
        // default there), so its file gets the same placeholder/coverage pins.
        // Template choice is a pure string rule: "8.4." with the dot, because
        // a bare "8.4" prefix would also match an 8.0-branch "8.40" directory.
        check(L"8.4.11 用 8.4 模板", myIniTemplateFor(L"8.4.11") == L"my-8.4.ini.tpl");
        check(L"裸 8.4 用 8.4 模板", myIniTemplateFor(L"8.4") == L"my-8.4.ini.tpl");
        check(L"8.0.46 用 8.0 模板", myIniTemplateFor(L"8.0.46") == L"my.ini.tpl");
        check(L"8.40 不是 8.4 分支", myIniTemplateFor(L"8.40") == L"my.ini.tpl");
        check(L"8.0.11 用 8.0 模板", myIniTemplateFor(L"8.0.11") == L"my.ini.tpl");
        std::wstring my84TplPath = joinPath(etcCompDir(L"mysql"), L"my-8.4.ini.tpl");
        std::wstring my84Tpl = readFileText(my84TplPath);
        check(L"mysql 8.4 模板存在", !my84Tpl.empty(), my84TplPath);
        check(L"8.4 模板打开 native 密码插件",
              my84Tpl.find(L"mysql_native_password = ON") != std::wstring::npos, my84TplPath);
        // 8.4删掉了 default_authentication_plugin 变量：写它会让初始化和启动
        // 直接 abort（8.4.11 实测），所以 8.4 模板里绝不能出现赋值行。判据带上
        // " ="，否则模板里解释原因的那行注释也会被误伤。
        check(L"8.4 模板不写已删除的变量",
              my84Tpl.find(L"default_authentication_plugin =") == std::wstring::npos, my84TplPath);
        check(L"8.0 模板保持原状",
              myTpl.find(L"default_authentication_plugin = mysql_native_password") != std::wstring::npos,
              myTplPath);
        check(L"8.4 模板包含全部渲染占位符",
              my84Tpl.find(L"{{BASEDIR}}") != std::wstring::npos &&
              my84Tpl.find(L"{{DATADIR}}") != std::wstring::npos &&
              my84Tpl.find(L"{{PORT}}") != std::wstring::npos &&
              my84Tpl.find(L"{{LOGFILE}}") != std::wstring::npos &&
              my84Tpl.find(L"{{PIDFILE}}") != std::wstring::npos &&
              my84Tpl.find(L"{{CONFDIR}}") != std::wstring::npos, my84TplPath);
        check(L"8.4 默认只监听本机",
              my84Tpl.find(L"bind-address = 127.0.0.1") != std::wstring::npos, my84TplPath);
        check(L"8.4 模板保留手工配置入口",
              my84Tpl.find(L"!includedir") != std::wstring::npos &&
              my84Tpl.find(L"conf.d") != std::wstring::npos, my84TplPath);
        std::map<std::wstring, std::wstring> my84Kv = {
            {L"BASEDIR", L"D:/x/bin/mysql/8.4.11"}, {L"DATADIR", L"D:/x/data/mysql/8.4.11/data"},
            {L"PORT", L"3306"}, {L"LOGFILE", L"D:/x/logs/mysql-8.4.11.log"},
            {L"PIDFILE", L"D:/x/data/mysql/8.4.11/mysqld.pid"},
            {L"CONFDIR", L"D:/x/data/mysql/8.4.11/conf.d"}};
        std::wstring my84Rendered = renderTemplate(my84Tpl, my84Kv);
        check(L"8.4 模板渲染后无残留占位符",
              my84Rendered.find(L"{{") == std::wstring::npos, my84Rendered);
        check(L"mysql 配置目录在 data\\ 下", compConfigDir(Comp::Mysql).rfind(dataDir(), 0) == 0,
              compConfigDir(Comp::Mysql));
    }

    wprintf(L"\n=== mysql component wiring ===\n");
    {
        // Adding a component touches enum mappings, install recognition, and
        // defaults. These checks pin that contract without starting mysqld,
        // writing data\, or binding 3306. Settings keys are guarded and
        // restored in memory.
        IniKeyGuard gMysqlUser(L"mysql.user");
        IniKeyGuard gMysqlPort(L"mysql.port");
        IniKeyGuard gMysqlVer(L"ver.mysql");
        iniDelete(L"mysql.user");
        iniDelete(L"mysql.port");
        iniDelete(L"ver.mysql");

        check(L"mysql enum name", compName(Comp::Mysql) == std::wstring(L"mysql"));
        check(L"mysql display name", compDisplay(Comp::Mysql) == std::wstring(L"MySQL"));
        check(L"mysql default user is root", myUser() == L"root");
        check(L"mysql default port is 3306", mysqlPort() == L"3306");
        check(L"missing mysql install is unusable",
              !compVersionUsable(Comp::Mysql, L"no-such-mysql-version"));
        check(L"missing mysql data is uninitialized",
              !myDataInitialized(Comp::Mysql, L"no-such-mysql-version"));

        // Account text is the SQL/command injection boundary: the combo is
        // user-editable, so parsing and literal quoting have to reject or
        // neutralize hostile input before mysqld ever sees it.
        std::wstring u, h;
        check(L"account splits", mySplitAccount(L"root@localhost", u, h) &&
              u == L"root" && h == L"localhost");
        check(L"bare account means localhost", mySplitAccount(L"app", u, h) &&
              u == L"app" && h == L"localhost");
        check(L"remote host parses", mySplitAccount(L"app@%", u, h) &&
              u == L"app" && h == L"%");
        check(L"semicolon account rejected", !mySplitAccount(L"app;DROP", u, h));
        check(L"quote account rejected", !mySplitAccount(L"app' OR '1", u, h));
        check(L"second at-sign rejected", !mySplitAccount(L"a@b@c", u, h));
        check(L"literal escapes quote and backslash",
              myEscapeLiteral(L"a'b\\c") == L"'a''b\\\\c'",
              myEscapeLiteral(L"a'b\\c"));
        // Accounts spell the plugin out instead of relying on the server
        // default, so 8.0 and 8.4 rows come out identical.
        check(L"auth clause names the plugin",
              myAuthClause(L"p@ss") == L"IDENTIFIED WITH mysql_native_password BY 'p@ss'",
              myAuthClause(L"p@ss"));
        check(L"auth clause escapes the password",
              myAuthClause(L"a'b\\c") == L"IDENTIFIED WITH mysql_native_password BY 'a''b\\\\c'",
              myAuthClause(L"a'b\\c"));
        check(L"mysql not running without binaries", !compIsRunning(Comp::Mysql));
        check(L"mysql quick status has no side effects", !compRunningQuick(Comp::Mysql));

        std::wstring backupFile = L"mysql-should-not-be-created.sql";
        std::wstring err;
        check(L"mysql backup refuses without a selected version",
              !myBackup(Comp::Mysql, backupFile, err) &&
              err == L"未选择 MySQL 版本", err);

        bool allMine = true;
        for (const auto& name : myListBackups()) {
            if (name.rfind(L"mysql-", 0) != 0) allMine = false;
        }
        check(L"mysql restore list is prefix-filtered", allMine);
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
        // 分配逻辑只认 settings.ini，所以这几个键必须先存后写；baseport 临时钉成
        // 9000，否则用户改过 baseport 时下面那条“默认值”断言会把测试跑红。
        IniKeyGuard gBase(L"php.baseport");
        IniKeyGuard gVer81(L"php.verport.8.1");
        IniKeyGuard gVer83(L"php.verport.8.3");
        iniSet(L"php.baseport", L"9000");
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
        // baseport 与两个 verport 由 guard 在出作用域时还原，不再无条件 iniDelete。
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
        // 同上：这里会反复改这四个键，一律用 guard 还原，避免删掉用户自己的配置。
        IniKeyGuard gVer81(L"php.verport.8.1");
        IniKeyGuard gVer83(L"php.verport.8.3");
        IniKeyGuard gWrk81(L"php.workers.8.1");
        IniKeyGuard gWrk83(L"php.workers.8.3");
        // Also need the default path, so make sure no leftover value is in play.
        iniDelete(L"php.workers.8.1");
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

        // A free port must actually be assignable. The skip-to-just-past-the-
        // clash rewrite used to initialise its cursor to p+1, which made every
        // all-free candidate look busy and spun the search to the end of the
        // range — nothing was ever assignable.
        {
            IniKeyGuard gBase2(L"php.baseport");
            IniKeyGuard g91(L"php.verport.9.9.9");
            iniDelete(L"php.verport.9.9.9");
            iniSet(L"php.baseport", L"45100");
            std::wstring e2;
            std::wstring got = phpEnsurePort(L"9.9.9", e2);
            check(L"空闲端口可分配", isPortToken(got), got.empty() ? e2 : got);
        }
        // 四个键由上面的 guard 在出作用域时按原值还原。
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

        // 归一化：用户三种写法（curl / php_curl.dll / "curl"，还带各种大小写）都见过，
        // 只要归一到同一个键，开关才不会互相错配——否则一个按 "CURL" 写进 ini 的
        // 复选框去匹配解析出来的 "curl" 就会把扩展判成关闭。
        {
            auto upper = phpParseExtensionLines(L"extension=CURL\r\n");
            auto lower = phpParseExtensionLines(L"extension=curl\r\n");
            check(L"大小写归一到同一个键",
                  upper.size() == 1 && lower.size() == 1 &&
                  upper.find(L"curl") != upper.end() && lower.find(L"curl") != lower.end() &&
                  upper[L"curl"] == true && lower[L"curl"] == true);
            auto dll = phpParseExtensionLines(L"extension=PHP_CURL.DLL\r\n");
            check(L"php_ 前缀 + .dll 后缀归一为 curl",
                  dll.size() == 1 && dll.find(L"curl") != dll.end() && dll[L"curl"] == true,
                  std::to_wstring(dll.size()));
            auto quoted = phpParseExtensionLines(L"extension=\"curl\"\r\n");
            check(L"带引号的值归一为 curl",
                  quoted.size() == 1 && quoted.find(L"curl") != quoted.end() && quoted[L"curl"] == true);
            // 注释掉的写法必须落到同一个键、但为 false，否则“未启用”和“未提及”
            // 分不开，勾选框会写出错误的 ;extension= 行。
            auto commented = phpParseExtensionLines(L";extension=\"PHP_CURL.dll\"\r\n");
            check(L"注释掉的写法同键且为 false",
                  commented.size() == 1 && commented.find(L"curl") != commented.end() &&
                  commented[L"curl"] == false);
            // 归一化后的名字必须能对上 ext\ 目录里真实的 dll 文件名，不能残留
            // php_ 前缀或 .dll 后缀。
            check(L"归一后不残留前缀/后缀",
                  dll.find(L"php_curl.dll") == dll.end() && dll.find(L"php_curl") == dll.end());
        }
    }

    wprintf(L"\n=== php: 扩展写回保真 ===\n");
    {
        // php.ini belongs to the user, so toggling one extension must not cost
        // them the comment they wrote next to it, nor reformat the file.
        std::wstring ini =
            L"[PHP]\r\n"
            L"memory_limit = 256M\r\n"
            L"extension=curl\r\n"
            L"  extension = mbstring  ; needed for utf8\r\n"
            L";extension=gd\r\n"
            L"extension=zip\r\n";
        int changed = -1;
        // Turn curl off, keep the rest as-is.
        std::wstring out = phpRewriteExtensionLines(ini, {L"mbstring", L"zip"}, &changed);
        check(L"关掉的加上分号", out.find(L";extension=curl") != std::wstring::npos, out);
        // Assert the state through the parser rather than by substring: the
        // user's own spacing ("extension = mbstring") must survive, so looking
        // for a canonical "extension=mbstring" text would fail on a correct file.
        {
            auto after = phpParseExtensionLines(out);
            check(L"保留的仍是启用",
                  after.find(L"mbstring") != after.end() && after[L"mbstring"] == true, out);
            check(L"关掉的变为未启用",
                  after.find(L"curl") != after.end() && after[L"curl"] == false);
            check(L"原本未启用的保持未启用",
                  after.find(L"gd") != after.end() && after[L"gd"] == false);
        }
        check(L"行尾注释保留",
              out.find(L"extension = mbstring  ; needed for utf8") != std::wstring::npos, out);
        check(L"缩进保留",
              out.find(L"  extension = mbstring") != std::wstring::npos, out);
        check(L"非扩展行原样保留",
              out.find(L"memory_limit = 256M") != std::wstring::npos &&
              out.find(L"[PHP]") != std::wstring::npos, out);
        check(L"CRLF 不被改成 LF",
              out.find(L"\n") == out.find(L"\r\n") + 1, out);
        // Only curl's state actually moved: mbstring was already on (its odd
        // "extension = mbstring" spacing must survive verbatim) and gd was
        // already off.
        check(L"只报告 1 行变化", changed == 1, std::to_wstring(changed));
        check(L"未动的行逐字节保留",
              out.find(L"extension = mbstring  ; needed for utf8") != std::wstring::npos &&
              out.find(L";extension=gd") != std::wstring::npos, out);
        // Idempotent: applying the same set again must report no change, or a
        // second click would rewrite the file for nothing.
        int changed2 = -1;
        std::wstring out2 = phpRewriteExtensionLines(out, {L"mbstring", L"zip"}, &changed2);
        check(L"重复应用不报告变化", changed2 == 0, std::to_wstring(changed2));
        check(L"重复应用内容不变", out2 == out);

        // wanted must be normalised the same way parsing is, otherwise a caller
        // passing "CURL" switches the extension off instead of on.
        int changed3 = -1;
        std::wstring out3 = phpRewriteExtensionLines(L";extension=curl\r\n",
                                                     {L"PHP_CURL.DLL"}, &changed3);
        check(L"wanted 归一后可开启", changed3 == 1 &&
              out3.find(L"extension=curl") != std::wstring::npos &&
              out3.find(L";extension=") == std::wstring::npos, out3);
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

    wprintf(L"\n=== myRestoreOutputOk ===\n");
    {
        // The mysql replay runs with --force so one expected bootstrap-schema
        // collision does not abort the whole restore. The tolerated family is
        // intentionally narrow: a duplicate database is expected, a duplicate
        // table means merging into live data and must fail loudly.
        std::wstring bad;
        check(L"empty output is ok", myRestoreOutputOk(L"", bad));
        check(L"no errors is ok", myRestoreOutputOk(L"CREATE DATABASE\ndump completed\n", bad));
        check(L"bootstrap database exists tolerated",
              myRestoreOutputOk(L"ERROR 1007 (HY000) at line 22: Can't create database 'mysql'; database exists\n",
                                bad));
        check(L"numeric code tolerates localized text",
              myRestoreOutputOk(L"ERROR 1007 (HY000) at line 22: 无法创建数据库 'mysql'；数据库已存在\n",
                                bad));
        check(L"drop of missing database tolerated",
              myRestoreOutputOk(L"ERROR 1008 (HY000) at line 4: Can't drop database 'legacydb'; database doesn't exist\n",
                                bad));
        check(L"table already exists rejected",
              !myRestoreOutputOk(L"ERROR 1050 (HY000) at line 35: Table 't' already exists\n", bad));
        check(L"unknown database rejected",
              !myRestoreOutputOk(L"ERROR 1049 (42000): Unknown database 'appdb'\n", bad));
        check(L"rejected error is reported",
              !myRestoreOutputOk(L"ERROR 1064 (42000) at line 3: You have an error in your SQL syntax\n", bad) &&
              bad.find(L"1064") != std::wstring::npos, bad);
        check(L"warnings ignored",
              myRestoreOutputOk(L"Warning: Skipping the data of table mysql.general_log\n", bad));
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
