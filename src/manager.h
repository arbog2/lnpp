#pragma once
#ifndef LNPP_MANAGER_H
#define LNPP_MANAGER_H

#include "common.h"
#include "process.h"

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

// ---- Path helpers ----
std::wstring compBinDir(Comp c);
std::wstring compBinDirVer(Comp c, const std::wstring& ver);
std::wstring compEtcDir(Comp c);
std::wstring compDataDir(Comp c);
std::wstring compDataVerDir(Comp c, const std::wstring& ver);

// ---- Common lifecycle ----
bool compStart(Comp c, std::wstring& err);
bool compStop(Comp c, std::wstring& err);
bool compSwitchVersion(Comp c, const std::wstring& ver, std::wstring& err);
bool compIsRunning(Comp c);

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
bool pgDataInitialized(const std::wstring& ver);
bool pgListUsers(std::vector<std::wstring>& users, std::wstring& err);

// redis
bool redisTestConfig(const std::wstring& ver, std::wstring& out);

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
bool genRedisConfig(const std::wstring& ver);
bool genPgConfig(const std::wstring& ver, const std::wstring& dataDir);

// ---- Pg connection settings ----
std::wstring pgUser();
std::wstring pgPassword();
std::wstring pgPort();
std::wstring nginxPort();
std::wstring redisPort();
std::wstring nodejsPort();

#endif
