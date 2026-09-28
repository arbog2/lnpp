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
    Count = 4
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
    std::wstring port;
    std::wstring root;
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
bool nginxAddVHostEx(const std::wstring& name, const std::wstring& domain,
                     const std::wstring& port, bool ssl,
                     const std::wstring& certPath, const std::wstring& keyPath,
                     const std::wstring& root, std::wstring& err);
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

#endif
