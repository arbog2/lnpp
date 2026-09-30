#pragma once
#ifndef LNPP_MANAGER_H
#define LNPP_MANAGER_H

#include "common.h"
#include "proc.h"

enum class Comp {
    Nginx = 0,
    Postgresql = 1,
    Redis = 2,
    Nodejs = 3,
    Php = 4,
    Count = 5
};

// What a vhost runs: the node template (static files first, then proxy_pass to
// the local nodejs port) or the php template (static files first, then
// fastcgi_pass to a php-cgi instance). A site belongs to nginx; PHP only decides
// which template it is rendered from and which FastCGI port it points at.
enum class SiteKind {
    Node = 0,
    Php = 1
};

struct ComponentStatus {
    bool installed = false;
    bool running = false;
    std::wstring currentVersion;
    std::vector<std::wstring> versions;
    std::wstring message;
};

struct PM2App {
    int id = -1;
    std::wstring name;
    std::wstring status;
    std::wstring interpreter;
    int restarts = 0;
    // OS pid of the running child (0 when pm2 reports none). pm2 marks an app
    // "online" from its own bookkeeping, so a row whose pid is gone is stale.
    DWORD pid = 0;
    // True when pm2 says online but that pid is not alive: the app died without
    // pm2 noticing (OOM kill, hard crash). Filled in by nodePm2List().
    bool stale = false;
};

struct VHost {
    std::wstring name;
    std::wstring domain;
    std::wstring port;                       // display: all listen ports, comma joined
    std::vector<std::wstring> ports;         // every port this site listens on
    std::wstring root;
    SiteKind kind = SiteKind::Node;
    // What a php site's fastcgi_pass names, verbatim: "127.0.0.1:9000" for a
    // single php-cgi, or the upstream name (e.g. "lnpp_php_8_3") for a pool.
    // Kept as written so the vhost list shows exactly what nginx will use.
    std::wstring phpTarget;
};

// ---- Component discovery ----
std::vector<std::wstring> compVersions(Comp c);
// Natural descending version comparison used by compVersions ("1.30" > "1.9").
bool naturalGt(const std::wstring& a, const std::wstring& b);
bool compVersionUsable(Comp c, const std::wstring& ver);
ComponentStatus compStatus(Comp c);
// Same as compStatus but never spawns helper processes (redis-cli / pm2);
// meant for background status polling.
ComponentStatus compStatusQuick(Comp c);
bool compRunningQuick(Comp c);
const wchar_t* compName(Comp c);
const wchar_t* compDisplay(Comp c);

// ---- Runtime layout ----
// etc\ holds templates only; data\ holds everything the program runs on.
// prepareRuntimeLayout() creates data\ when missing, seeds data\packages.conf
// from etc\packages.conf.tpl, and migrates per-site vhost files that used to
// live in etc\nginx\vhosts\ into data\nginx\vhosts\. Call once at startup.
void prepareRuntimeLayout();
// Where the user's per-site nginx configs live (data\nginx\vhosts\).
std::wstring nginxVhostSourceDir();

// ---- Path helpers ----
std::wstring compBinDir(Comp c);
std::wstring compBinDirVer(Comp c, const std::wstring& ver);
std::wstring compEtcDir(Comp c);
std::wstring compDataDir(Comp c);
std::wstring compDataVerDir(Comp c, const std::wstring& ver);
// Directory the "配置" button opens: the component's live, hand-editable
// configuration, which is always under data\ (etc\ holds templates only).
std::wstring compConfigDir(Comp c);

// ---- Common lifecycle ----
bool compStart(Comp c, std::wstring& err);
bool compStop(Comp c, std::wstring& err);
bool compSwitchVersion(Comp c, const std::wstring& ver, std::wstring& err);
bool compIsRunning(Comp c);
// True when an operation was refused only because another thread is already
// driving the same component. That is benign — under autostart, for instance,
// compStart(Nodejs) always finds the user's own Redis start in flight — so
// callers should report it as a skip rather than a failure.
bool compIsBusy(const std::wstring& err);

// ---- Per-component ----
// nginx
bool nginxTestConfig(const std::wstring& ver, std::wstring& out);
bool nginxReload(std::wstring& err);
std::vector<VHost> nginxListVHosts();
bool nginxAddVHost(const std::wstring& name, const std::wstring& domain,
                   const std::wstring& port, std::wstring& err);
// kind/phpVer only matter for SiteKind::Php: phpVer selects the php-cgi instance
// whose FastCGI port is written into the template. An empty or unusable phpVer
// falls back to the default version (ver.php).
bool nginxAddVHostEx(const std::wstring& name, const std::wstring& domain,
                     const std::wstring& port, bool ssl,
                     const std::wstring& certPath, const std::wstring& keyPath,
                     const std::wstring& root,
                     SiteKind kind, const std::wstring& phpVer,
                     std::wstring& err);
bool nginxRemoveVHost(const std::wstring& name, std::wstring& err);

// postgresql
bool pgInit(Comp c, const std::wstring& ver, const std::wstring& user,
            const std::wstring& password, const std::wstring& port, std::wstring& err);
bool pgChangePassword(Comp c, const std::wstring& user, const std::wstring& password, std::wstring& err);
bool pgCreateUser(Comp c, const std::wstring& user, const std::wstring& password, std::wstring& err);
bool pgDropUser(Comp c, const std::wstring& user, std::wstring& err);
bool pgBackup(Comp c, std::wstring& backupFile, std::wstring& err);
// Backups available to restore, newest first (backup\*.sql).
std::vector<std::wstring> pgListBackups();
// Replay a pg_dumpall dump. reinit=true rebuilds the cluster first — its data
// directory is moved aside, never deleted, so a failed restore can be rolled
// back. On success err carries a note (the location of the preserved copy).
// This is destructive either way: the restore overwrites the databases.
bool pgRestoreBackup(Comp c, const std::wstring& backupFile, bool reinit, std::wstring& err);
// True when a psql replay produced no error other than "already exists" for a
// role or a database. A pg_dumpall dump always collides with the bootstrap
// superuser (it exists in every cluster, however fresh) and with any database
// that survived, so those are expected; anything else means an incomplete
// restore. firstRealError receives the first unexpected ERROR line.
bool pgRestoreOutputOk(const std::wstring& output, std::wstring& firstRealError);
bool pgDataInitialized(const std::wstring& ver);
bool pgListUsers(std::vector<std::wstring>& users, std::wstring& err);

// nodejs / pm2
bool nodePm2Installed();
std::vector<PM2App> nodePm2List();
// Parse `pm2 jlist` JSON text into PM2App records (pure text parsing).
std::vector<PM2App> parsePm2List(const std::wstring& json);
bool nodePm2Restart(int id, std::wstring& err);
bool nodePm2Stop(int id, std::wstring& err);
bool nodePm2RestartAll(std::wstring& err);
bool nodePm2Delete(int id, std::wstring& err);
bool nodePm2Resurrect(std::wstring& err);

// ---- php (FastCGI) ----
// Windows has no PHP-FPM: the only FastCGI-capable binary in the official
// build is php-cgi.exe, run as `php-cgi -c <ini> -b 127.0.0.1:<port>`. So a PHP
// "instance" here is one php-cgi process per version, and PHP is a resident
// component like the other four — with one difference: several versions run at
// once, each on its own port, so a project pinned to 8.1 and one pinned to 8.3
// can be served side by side. ver.php only names the default (which version new
// sites start out pointing at); it does not gate which versions run.
struct PhpInstance {
    std::wstring version;
    std::wstring port;                        // first FastCGI port, "" if none yet
    int workers = 1;                          // php-cgi processes for this version
    bool running = false;
    bool isDefault = false;                   // the version ver.php points at
};
// Assigned port for a version, or "" if it never got one. Read-only on purpose:
// copying a version into bin\php\ must not rewrite settings.ini, so a port is
// only ever allocated when that version is actually started.
std::wstring phpPortFor(const std::wstring& ver);
// Every port a version owns, consecutive from phpPortFor(). One entry with the
// default single worker.
std::vector<std::wstring> phpPortsFor(const std::wstring& ver);
// php-cgi processes per version (settings.ini: php.workers.<ver>, default 1,
// clamped to 1..32). Windows has no FPM, so this is the entire concurrency
// setting for a version: N processes serve N requests at a time.
int phpWorkerCount(const std::wstring& ver);
// Allocate and persist a port when the version has none. Reserves workers()
// consecutive ports and never overlaps another version's range, nor a port
// something else is listening on.
std::wstring phpEnsurePort(const std::wstring& ver, std::wstring& err);
bool phpRunning(const std::wstring& ver);
bool phpStart(const std::wstring& ver, std::wstring& err);
bool phpStop(const std::wstring& ver, std::wstring& err);
std::vector<PhpInstance> phpListPool();
// nginx upstream block for {{PHP_UPSTREAM}}: one entry per version running more
// than one php-cgi, empty when they all run a single process. An upstream must
// be declared once inside http{} — the same name in two vhosts is a fatal
// "duplicate upstream" — so this can only be spliced into the main config.
std::wstring phpUpstreamBlock();
// Name of a version's upstream, sanitised from the version string.
std::wstring phpUpstreamName(const std::wstring& ver);
// What a php site's fastcgi_pass should be: the upstream name for a pool, the
// bare 127.0.0.1:<port> address for a single worker.
std::wstring phpFastcgiTarget(const std::wstring& ver);
// Site names in data\nginx\vhosts\*.conf that target a given FastCGI port,
// matching both the address form and the upstream form, so a version can be
// stopped or removed only after saying what will break.
std::vector<std::wstring> phpSitesUsingPort(const std::wstring& port);

// ---- php extensions ----
struct PhpExtension {
    std::wstring name;        // without the php_ prefix or .dll suffix, e.g. "curl"
    std::wstring dll;         // file name in bin\php\<ver>\ext, e.g. "php_curl.dll"
    bool enabled = false;     // an uncommented extension= line in php.ini
    bool loaded = false;      // this build actually ships the dll
};
// What php.ini currently asks for, keyed by extension name. Parsed from text so
// the whole rule is unit-testable; a line is "extension=<name>" optionally
// prefixed with ';' (commented out = not enabled).
std::map<std::wstring, bool> phpParseExtensionLines(const std::wstring& iniText);
// The extensions this version could load: the dlls present in its ext\ directory,
// plus anything php.ini already mentions (so an entry pointing at a missing dll
// is visible as broken rather than silently dropped).
std::vector<PhpExtension> phpListExtensions(const std::wstring& ver, std::wstring& err);
// Rewrite php.ini so exactly `wanted` is enabled, leaving every other line —
// including comments and unrelated settings — untouched. Returns the number of
// extension lines changed, or -1 on failure.
int phpApplyExtensions(const std::wstring& ver, const std::vector<std::wstring>& wanted,
                      std::wstring& err);

// ---- Config generation ----
bool genNginxConfig(const std::wstring& ver);
// The catch-all server blocks substituted into the {{DENY_UNKNOWN}} placeholder
// of etc\nginx\nginx.conf.tpl. blockUnknown=true refuses every request whose
// Host / SNI matches no site (:80 -> 500, :443 -> TLS handshake refusal);
// false restores the old localhost-only site. Toggled by the
// nginx.block_unknown_host setting so a mistake is one ini edit away from being
// undone. Public for unit tests.
std::wstring nginxDenyBlock(bool blockUnknown);
bool genRedisConfig(const std::wstring& ver);
bool genPgConfig(const std::wstring& ver, const std::wstring& dataDir);
// Write data\php\<ver>\php.ini (shipped php.ini-production + rendered
// etc\php\php.ini.append). Generated once and then left alone: the "配置"
// button opens that file and users enable extensions in it, so regenerating on
// every start would silently drop their edits.
bool genPhpConfig(const std::wstring& ver, std::wstring& err);

// ---- Log rotation ----
// Rotate the component's log file(s) if they passed 8 MB, keeping 2 generations
// (x.log.1 / x.log.2). MUST only be called while the component is down: Windows
// refuses to rename a log file a running server still holds open. compStart()
// calls it automatically before spawning.
void rotateCompLogs(Comp c, const std::wstring& ver);

// ---- Pg connection settings ----
std::wstring pgUser();
std::wstring pgPassword();
std::wstring pgPort();
std::wstring nginxPort();
std::wstring redisPort();
std::wstring nodejsPort();
// First port handed out to a php-cgi instance (settings.ini: php.baseport).
std::wstring phpBasePort();

#endif
