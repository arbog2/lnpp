#include "manager.h"
#include <tlhelp32.h>

// ============================ Path helpers ============================

const wchar_t* compName(Comp c) {
    switch (c) {
        case Comp::Nginx: return L"nginx";
        case Comp::Postgresql: return L"postgresql";
        case Comp::Redis: return L"redis";
        case Comp::Nodejs: return L"nodejs";
        default: return L"";
    }
}

const wchar_t* compDisplay(Comp c) {
    switch (c) {
        case Comp::Nginx: return L"nginx";
        case Comp::Postgresql: return L"PostgreSQL";
        case Comp::Redis: return L"Redis";
        case Comp::Nodejs: return L"Node.js";
        default: return L"";
    }
}

std::wstring compBinDir(Comp c)       { return binCompDir(compName(c)); }
std::wstring compEtcDir(Comp c)       { return etcCompDir(compName(c)); }
std::wstring compDataDir(Comp c)      { return dataCompDir(compName(c)); }
std::wstring compDataVerDir(Comp c, const std::wstring& ver) {
    return dataCompVerDir(compName(c), ver);
}
std::wstring compBinDirVer(Comp c, const std::wstring& ver) {
    return joinPath(compBinDir(c), ver);
}

// ============================ Component discovery ============================

std::vector<std::wstring> compVersions(Comp c) {
    std::vector<std::wstring> versions;
    for (const auto& ver : listSubDirs(compBinDir(c))) {
        if (compVersionUsable(c, ver)) versions.push_back(ver);
    }
    // Sort version names naturally (for example 1.30 before 1.9).
    auto parts = [](const std::wstring& value) {
        std::vector<unsigned long long> result;
        size_t i = 0;
        while (i < value.size()) {
            while (i < value.size() && !iswdigit(value[i])) ++i;
            if (i == value.size()) break;
            unsigned long long n = 0;
            while (i < value.size() && iswdigit(value[i])) {
                n = n * 10 + (value[i] - L'0');
                ++i;
            }
            result.push_back(n);
        }
        return result;
    };
    std::sort(versions.begin(), versions.end(), [&](const auto& a, const auto& b) {
        auto pa = parts(a), pb = parts(b);
        size_t n = pa.size() > pb.size() ? pa.size() : pb.size();
        for (size_t i = 0; i < n; ++i) {
            auto va = i < pa.size() ? pa[i] : 0;
            auto vb = i < pb.size() ? pb[i] : 0;
            if (va != vb) return va > vb;
        }
        return lowerStr(a) > lowerStr(b);
    });
    return versions;
}

bool compVersionUsable(Comp c, const std::wstring& ver) {
    if (ver.empty() || !dirExists(compBinDirVer(c, ver))) return false;
    switch (c) {
        case Comp::Nginx:
            return fileExists(joinPath(compBinDirVer(c, ver), L"nginx.exe"));
        case Comp::Postgresql:
            return fileExists(joinPath(joinPath(compBinDirVer(c, ver), L"bin"), L"pg_ctl.exe"));
        case Comp::Redis:
            return fileExists(joinPath(compBinDirVer(c, ver), L"redis-server.exe"));
        case Comp::Nodejs:
            return fileExists(joinPath(compBinDirVer(c, ver), L"node.exe"));
        default:
            return false;
    }
}

ComponentStatus compStatusImpl(Comp c, bool quick) {
    ComponentStatus st;
    st.versions = compVersions(c);
    st.installed = !st.versions.empty();
    if (st.installed) {
        // Current version persisted in ini
        std::wstring key = std::wstring(L"ver.") + compName(c);
        st.currentVersion = iniGet(key, L"");
        if (st.currentVersion.empty() || !compVersionUsable(c, st.currentVersion)) {
            st.currentVersion = st.versions.front();
            iniSet(key, st.currentVersion);
        }
    }
    st.running = quick ? compRunningQuick(c) : compIsRunning(c);
    return st;
}

ComponentStatus compStatus(Comp c) { return compStatusImpl(c, false); }

// Same as compStatus but never spawns a helper process (used by background
// polling so the UI thread never blocks on redis-cli / pm2 process launches).
ComponentStatus compStatusQuick(Comp c) { return compStatusImpl(c, true); }

// ============================ Process helpers ============================

std::wstring compExe(Comp c, const std::wstring& ver, const std::wstring& exeName) {
    // Node/nginx/redis have exe at version root; postgresql has bin/ subdir
    if (c == Comp::Postgresql)
        return joinPath(joinPath(compBinDirVer(c, ver), L"bin"), exeName);
    return joinPath(compBinDirVer(c, ver), exeName);
}

static bool findExe(Comp c, const std::wstring& ver, const std::wstring& exeName, std::wstring& out) {
    out = compExe(c, ver, exeName);
    return fileExists(out);
}

// ---- nginx ----
static std::wstring nginxPidFile(const std::wstring& ver) {
    return joinPath(joinPath(compDataVerDir(Comp::Nginx, ver), L"logs"), L"nginx.pid");
}

static DWORD readPid(const std::wstring& file) {
    std::wstring s = trimStr(readFileText(file));
    if (s.empty()) return 0;
    try { return (DWORD)_wtoi(s.c_str()); } catch (...) { return 0; }
}

static bool anyNginxRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {0};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"nginx.exe") == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// nginx master = a nginx.exe process whose parent is not another nginx.exe
static DWORD nginxMasterPid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {0};
    pe.dwSize = sizeof(pe);
    std::vector<DWORD> pids, ppids;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"nginx.exe") == 0) {
                pids.push_back(pe.th32ProcessID);
                ppids.push_back(pe.th32ParentProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    for (size_t i = 0; i < pids.size(); ++i) {
        bool parentIsNginx = false;
        for (size_t j = 0; j < pids.size(); ++j)
            if (ppids[i] == pids[j]) { parentIsNginx = true; break; }
        if (!parentIsNginx) return pids[i];
    }
    return pids.empty() ? 0 : pids[0];
}

static bool nginxRunningVer(const std::wstring& ver) {
    DWORD pid = readPid(nginxPidFile(ver));
    if (isPidAlive(pid)) return true;
    if (!anyNginxRunning()) return false;
    // stale pidfile (e.g. nginx started outside the manager): repoint it at the
    // live master so `-s reload` / `-s quit` address the right process
    DWORD mp = nginxMasterPid();
    if (mp) writeFileText(nginxPidFile(ver), std::to_wstring(mp));
    return true;
}

// ---- postgresql ----
static std::wstring pgDataDir(Comp c, const std::wstring& ver) {
    return compDataVerDir(c, ver);
}

static bool pgRunningVer(Comp c, const std::wstring& ver) {
    std::wstring pidFile = joinPath(pgDataDir(c, ver), L"postmaster.pid");
    DWORD pid = readPid(pidFile);
    return isPidAlive(pid);
}

// ---- redis ----
static std::wstring redisPidFile(const std::wstring& ver) {
    return joinPath(compDataVerDir(Comp::Redis, ver), L"redis.pid");
}

static bool redisRunningVer(const std::wstring& ver) {
    // Pidfile is the source of truth. A redis started by us always writes
    // <data>/redis/<ver>/redis.pid, and we control the start, so any live
    // instance we own must be in there. Probing with `redis-cli ping` was
    // the wrong fallback: it would happily answer PONG for an unrelated
    // Redis on the same port, or stick around for a few seconds after a
    // clean shutdown while the kernel reclaims the socket — both cases
    // made the UI flicker "running → stopped" right after Stop.
    DWORD pid = readPid(redisPidFile(ver));
    return isPidAlive(pid);
}

// ---- nodejs / pm2 ----
static bool nodeExe(const std::wstring& ver, std::wstring& out) {
    out = joinPath(compBinDirVer(Comp::Nodejs, ver), L"node.exe");
    return fileExists(out);
}

// True iff the pm2 God daemon process is alive. Detected via the daemon pid
// file ($PM2_HOME\pm2.pid); we must NOT probe with `pm2 ping` / `pm2 pid`,
// because those silently re-spawn the daemon when it is stopped.
static bool nodeDaemonRunning() {
    wchar_t home[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"PM2_HOME", home, MAX_PATH);
    std::wstring homeDir;
    if (n == 0 || n >= MAX_PATH) {
        n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return false;
        homeDir = joinPath(home, L".pm2");
    } else {
        homeDir = home;
    }
    DWORD pid = readPid(joinPath(homeDir, L"pm2.pid"));
    return isPidAlive(pid);
}

static bool nodePm2Cmd(const std::wstring& ver, std::wstring& out) {
    out = joinPath(compBinDirVer(Comp::Nodejs, ver), L"pm2.cmd");
    if (fileExists(out)) return true;
    out = joinPath(compBinDirVer(Comp::Nodejs, ver), L"node_modules\\pm2\\bin\\pm2.js");
    if (fileExists(out)) return true;
    // fall back to pm2 in system PATH (global npm install)
    wchar_t buf[4096];
    DWORD n = GetEnvironmentVariableW(L"PATH", buf, 4096);
    if (n > 0) {
        std::wstring path(buf, n);
        std::wstringstream ss(path);
        std::wstring dir;
        while (std::getline(ss, dir, L';')) {
            std::wstring cand = joinPath(trimStr(dir), L"pm2.cmd");
            if (fileExists(cand)) { out = cand; return true; }
        }
    }
    out.clear();
    return false;
}

bool nodePm2Installed() {
    ComponentStatus st = compStatus(Comp::Nodejs);
    if (!st.installed) return false;
    std::wstring pm2;
    return nodePm2Cmd(st.currentVersion, pm2);
}

static RunResult runPm2(const std::vector<std::wstring>& args, int timeoutMs = 15000) {
    std::wstring ver = iniGet(L"ver.nodejs", L"");
    if (ver.empty()) return {};
    RunResult empty;
    if (!dirExists(compBinDirVer(Comp::Nodejs, ver))) return empty;
    std::wstring pm2;
    if (nodePm2Cmd(ver, pm2)) {
        std::wstring cmdArgs;
        for (auto& a : args) {
            if (!cmdArgs.empty()) cmdArgs += L" ";
            cmdArgs += a;
        }
        std::wstring wd = compBinDirVer(Comp::Nodejs, ver);
        // prepend the portable node dir to PATH so `node` (used by pm2 daemon and
        // forked apps) resolves to LNPP's bundled node, not a system-wide install
        wchar_t pathBuf[32768];
        DWORD n = GetEnvironmentVariableW(L"PATH", pathBuf, 32768);
        std::wstring path = wd + L";" + (n > 0 ? std::wstring(pathBuf, n) : L"");
        return runProcessCapture(pm2, cmdArgs, wd, timeoutMs, {{L"PATH", path}});
    }
    return empty;
}

// ============================ Common lifecycle ============================

bool compIsRunning(Comp c) {
    ComponentStatus st;
    st.currentVersion = iniGet(std::wstring(L"ver.") + compName(c), L"");
    if (st.currentVersion.empty()) return false;
    if (!compVersionUsable(c, st.currentVersion)) return false;
    switch (c) {
        case Comp::Nginx:      return nginxRunningVer(st.currentVersion);
        case Comp::Postgresql: return pgRunningVer(c, st.currentVersion);
        case Comp::Redis:      return redisRunningVer(st.currentVersion);
        case Comp::Nodejs:
            return nodeDaemonRunning();
        default: return false;
    }
}

// Lightweight liveness probe for background polling: identical to
// compIsRunning except Redis uses the pidfile only (no redis-cli spawn).
bool compRunningQuick(Comp c) {
    std::wstring ver = iniGet(std::wstring(L"ver.") + compName(c), L"");
    if (ver.empty()) return false;
    if (!dirExists(compBinDirVer(c, ver))) return false;
    switch (c) {
        case Comp::Nginx:      return nginxRunningVer(ver);
        case Comp::Postgresql: return pgRunningVer(c, ver);
        case Comp::Redis: {
            DWORD pid = readPid(redisPidFile(ver));
            return isPidAlive(pid);
        }
        case Comp::Nodejs:     return nodeDaemonRunning();
        default: return false;
    }
}

bool compStart(Comp c, std::wstring& err) {
    ComponentStatus st = compStatus(c);
    if (!st.installed) { err = L"组件未安装（bin 下无版本目录）"; return false; }
    if (st.running) { err = L"已在运行"; return false; }
    std::wstring ver = st.currentVersion;

    switch (c) {
        case Comp::Nginx: {
            if (!genNginxConfig(ver)) { err = L"生成 nginx 配置失败"; return false; }
            std::wstring exe;
            if (!findExe(Comp::Nginx, ver, L"nginx.exe", exe)) {
                err = L"未找到 nginx.exe"; return false;
            }
            // Validate config
            std::wstring tOut;
            if (!nginxTestConfig(ver, tOut)) {
                err = L"nginx -t 校验失败:\n" + tOut;
                return false;
            }
            std::wstring prefix = compDataVerDir(Comp::Nginx, ver);
            ProcInfo pi;
            if (!startProcessDetached(exe, L"-p \"" + toForward(prefix) + L"\"", prefix, pi)) {
                err = L"启动 nginx 失败"; return false;
            }
            // Give it a moment to write pid file
            for (int i = 0; i < 20; ++i) {
                if (nginxRunningVer(ver)) return true;
                Sleep(100);
            }
            err = L"nginx 启动后未检测到进程";
            return false;
        }
        case Comp::Postgresql: {
            std::wstring dataDir = pgDataDir(c, ver);
            if (!pgDataInitialized(ver)) {
                err = L"数据库尚未初始化，请先点击「初始化数据库」";
                return false;
            }
            std::wstring pgCtl;
            if (!findExe(Comp::Postgresql, ver, L"pg_ctl.exe", pgCtl)) {
                err = L"未找到 pg_ctl.exe"; return false;
            }
            std::wstring logFile = joinPath(logsDir(), L"postgresql-" + ver + L".log");
            std::wstring args = L"-D \"" + dataDir + L"\" -l \"" + logFile + L"\" start -w";
            ProcInfo pi;
            if (!startProcessDetached(pgCtl, args, compBinDirVer(c, ver), pi)) {
                err = L"启动 PostgreSQL 失败"; return false;
            }
            // wait for it
            WaitForSingleObject(pi.hProcess, 30000);
            if (!pgRunningVer(c, ver)) {
                err = L"PostgreSQL 启动失败，请查看日志 " + logFile;
                return false;
            }
            return true;
        }
        case Comp::Redis: {
            if (!genRedisConfig(ver)) { err = L"生成 redis 配置失败"; return false; }
            std::wstring exe;
            if (!findExe(Comp::Redis, ver, L"redis-server.exe", exe)) {
                err = L"未找到 redis-server.exe"; return false;
            }
            std::wstring conf = joinPath(compDataVerDir(Comp::Redis, ver), L"redis.conf");
            ProcInfo pi;
            if (!startProcessDetached(exe, L"\"" + conf + L"\"", compBinDirVer(c, ver), pi)) {
                err = L"启动 Redis 失败"; return false;
            }
            for (int i = 0; i < 20; ++i) {
                if (redisRunningVer(ver)) return true;
                Sleep(100);
            }
            err = L"Redis 启动后未检测到进程";
            return false;
        }
        case Comp::Nodejs: {
            // pm2 resurrect restores previously saved processes
            std::wstring e;
            if (!nodePm2Resurrect(e)) {
                err = e;
                return false;
            }
            return true;
        }
        default: return false;
    }
}

bool compStop(Comp c, std::wstring& err) {
    ComponentStatus st = compStatus(c);
    if (!st.installed) { err = L"组件未安装"; return false; }
    std::wstring ver = st.currentVersion;

    switch (c) {
        case Comp::Nginx: {
            std::wstring exe;
            if (!findExe(Comp::Nginx, ver, L"nginx.exe", exe)) {
                err = L"未找到 nginx.exe"; return false;
            }
            std::wstring prefix = compDataVerDir(Comp::Nginx, ver);
            // graceful quit first
            RunResult r = runProcessCapture(exe, L"-s quit -p \"" + toForward(prefix) + L"\"",
                                            prefix, 10000);
            // fallback: kill by pidfile
            DWORD pid = readPid(nginxPidFile(ver));
            if (isPidAlive(pid)) killProcessByPid(pid);
            for (int i = 0; i < 30; ++i) {
                if (!nginxRunningVer(ver)) return true;
                Sleep(100);
            }
            err = L"nginx 无法停止";
            return false;
        }
        case Comp::Postgresql: {
            std::wstring pgCtl;
            if (!findExe(Comp::Postgresql, ver, L"pg_ctl.exe", pgCtl)) {
                err = L"未找到 pg_ctl.exe"; return false;
            }
            std::wstring dataDir = pgDataDir(c, ver);
            RunResult r = runProcessCapture(pgCtl,
                L"-D \"" + dataDir + L"\" stop -m fast -w",
                compBinDirVer(c, ver), 30000);
            if (!r.ok) {
                // force kill postmaster pid
                std::wstring pidFile = joinPath(dataDir, L"postmaster.pid");
                DWORD pid = readPid(pidFile);
                if (isPidAlive(pid)) killProcessByPid(pid);
            }
            return !pgRunningVer(c, ver);
        }
        case Comp::Redis: {
            // try shutdown via redis-cli
            std::wstring cli;
            if (findExe(Comp::Redis, ver, L"redis-cli.exe", cli)) {
                runProcessCapture(cli, L"-p " + redisPort() + L" shutdown nosave", 
                                  compBinDirVer(c, ver), 5000);
            }
            DWORD pid = readPid(redisPidFile(ver));
            if (isPidAlive(pid)) killProcessByPid(pid);
            for (int i = 0; i < 30; ++i) {
                if (!redisRunningVer(ver)) return true;
                Sleep(100);
            }
            err = L"Redis 无法停止";
            return false;
        }
        case Comp::Nodejs: {
            RunResult r = runPm2({L"kill"}, 8000);
            if (r.ok) return true;
            // fallback: kill the pm2 daemon process
            RunResult jl = runPm2({L"pid"}, 5000);
            if (jl.ok) {
                std::wstring pidStr = trimStr(jl.output);
                if (!pidStr.empty()) {
                    DWORD pid = (DWORD)_wtoi(pidStr.c_str());
                    if (isPidAlive(pid)) killProcessByPid(pid);
                }
            }
            err = r.output.empty() ? L"pm2 kill 失败" : r.output;
            return !compIsRunning(Comp::Nodejs);
        }
        default: return false;
    }
}

bool compSwitchVersion(Comp c, const std::wstring& ver, std::wstring& err) {
    ComponentStatus st = compStatus(c);
    if (!st.installed) { err = L"组件未安装"; return false; }
    if (st.currentVersion == ver) { err = L"已是当前版本"; return false; }
    // verify new version dir exists
    if (!compVersionUsable(c, ver)) { err = L"版本不可用或目录不存在: " + ver; return false; }

    // Stop current
    if (st.running) {
        if (!compStop(c, err)) { err = L"停止旧版本失败: " + err; return false; }
    }

    // Postgresql special: migrate data
    if (c == Comp::Postgresql) {
        std::wstring oldVer = st.currentVersion;
        std::wstring oldData = pgDataDir(c, oldVer);
        std::wstring newData = pgDataDir(c, ver);
        bool oldInit = pgDataInitialized(oldVer);

        // Need old server running to dump
        bool oldWasRunning = pgRunningVer(c, oldVer);
        if (oldInit && !oldWasRunning) {
            iniSet(std::wstring(L"ver.postgresql"), oldVer);
            std::wstring serr;
            if (!compStart(c, serr)) {
                err = L"启动旧版本以备份失败: " + serr;
                return false;
            }
            Sleep(2000);
        }

        std::wstring backupFile;
        if (oldInit) {
            // backup old database
            backupFile = joinPath(backupDir(),
                L"postgresql-" + oldVer + L"-" + nowStamp() + L".sql");
            std::wstring berr;
            if (!pgBackup(c, backupFile, berr)) {
                err = L"备份旧数据库失败: " + berr;
                return false;
            }
        }

        // stop any running server (old or new)
        if (pgRunningVer(c, oldVer) || pgRunningVer(c, ver)) {
            // ensure ini points to whichever is actually running so compStop targets it
            iniSet(std::wstring(L"ver.postgresql"),
                   pgRunningVer(c, oldVer) ? oldVer : ver);
            std::wstring berr;
            if (!compStop(c, berr)) {
                err = L"停止数据库失败: " + berr;
                return false;
            }
            Sleep(1000);
        }

        // if target data dir already exists (from earlier migration), move it aside
        // so initdb sees a fresh dir; the old copy is preserved as safety
        if (dirExists(newData)) {
            std::wstring oldDir = newData + L".old-" + nowStamp();
            MoveFileW(newData.c_str(), oldDir.c_str());
        }

        // init target with fresh dir
        // (set version first so subsequent compStart targets the new version)
        iniSet(std::wstring(L"ver.postgresql"), ver);
        std::wstring berr;
        if (!pgInit(c, ver, pgUser(), pgPassword(), pgPort(), berr)) {
            err = L"初始化新版本失败: " + berr;
            return false;
        }

        // restore backup into new
        if (!backupFile.empty()) {
            if (!compStart(c, berr)) {
                err = L"启动新版本以恢复失败: " + berr;
                return false;
            }
            Sleep(2000);
            std::wstring psql;
            if (!findExe(Comp::Postgresql, ver, L"psql.exe", psql)) {
                err = L"未找到 psql.exe"; return false;
            }
            std::wstring cmd = L"-h 127.0.0.1 -p " + pgPort() + L" -U " + pgUser() +
                               L" -d postgres -f \"" + backupFile + L"\"";
            RunResult r = runProcessCapture(psql, cmd, compBinDirVer(c, ver), 120000,
                                            {{L"PGPASSWORD", pgPassword()}});
            if (!r.ok) {
                err = L"恢复备份失败: " + r.output;
                return false;
            }
        }
        return true;
    }

    // Other components: just update version and restart
    iniSet(std::wstring(L"ver.") + compName(c), ver);
    if (c == Comp::Nginx) {
        if (!genNginxConfig(ver)) { err = L"生成 nginx 配置失败"; return false; }
        std::wstring tOut;
        if (!nginxTestConfig(ver, tOut)) {
            err = L"nginx -t 校验失败: " + tOut;
            return false;
        }
        std::wstring serr;
        if (!compStart(c, serr)) { err = L"启动失败: " + serr; return false; }
    } else if (c == Comp::Redis) {
        if (!genRedisConfig(ver)) { err = L"生成 redis 配置失败"; return false; }
        std::wstring serr;
        if (!compStart(c, serr)) { err = L"启动失败: " + serr; return false; }
    } else if (c == Comp::Nodejs) {
        // restart pm2 with new node version
        std::wstring serr;
        if (!compStart(c, serr)) { err = L"启动失败: " + serr; return false; }
    }
    return true;
}

// ============================ Config generation ============================

static const wchar_t* DEFAULT_NGINX_CONF =
    L"worker_processes  1;\r\n"
    L"error_log  logs/error.log;\r\n"
    L"pid        logs/nginx.pid;\r\n"
    L"\r\n"
    L"events {\r\n"
    L"    worker_connections  1024;\r\n"
    L"}\r\n"
    L"\r\n"
    L"http {\r\n"
    L"    include       \"{{MIME}}\";\r\n"
    L"    default_type  application/octet-stream;\r\n"
    L"    sendfile        on;\r\n"
    L"    keepalive_timeout  65;\r\n"
    L"    access_log logs/access.log;\r\n"
    L"    error_log  logs/error.log;\r\n"
    L"\r\n"
    L"    server {\r\n"
    L"        listen       {{PORT}};\r\n"
    L"        server_name  localhost;\r\n"
    L"        location / {\r\n"
    L"            root   {{WWW_DIR}};\r\n"
    L"            index  index.html index.htm;\r\n"
    L"        }\r\n"
    L"    }\r\n"
    L"    include vhosts/*.conf;\r\n"
    L"}\r\n";

static const wchar_t* DEFAULT_REDIS_CONF =
    L"port {{PORT}}\r\n"
    L"daemonize no\r\n"
    L"pidfile {{PIDFILE}}\r\n"
    L"logfile {{LOGFILE}}\r\n"
    L"dir {{DIR}}\r\n"
    L"appendonly yes\r\n"
    L"save 900 1\r\n"
    L"save 300 10\r\n"
    L"save 60 10000\r\n";

static const wchar_t* DEFAULT_PG_APPEND =
    L"listen_addresses = '127.0.0.1'\r\n"
    L"port = {{PORT}}\r\n"
    L"max_connections = 100\r\n";

static const wchar_t* DEFAULT_VHOST =
    L"server {\r\n"
    L"    listen {{PORT}};\r\n"
    L"    server_name {{DOMAIN}};\r\n"
    L"    root {{ROOT}};\r\n"
    L"    index index.html index.htm;\r\n"
    L"    location / {\r\n"
    L"        try_files $uri $uri/ @nodejs;\r\n"
    L"    }\r\n"
    L"    location @nodejs {\r\n"
    L"        proxy_pass http://127.0.0.1:{{NODEJS_PORT}};\r\n"
    L"        proxy_set_header Host $host;\r\n"
    L"        proxy_set_header X-Real-IP $remote_addr;\r\n"
    L"        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;\r\n"
    L"        proxy_set_header X-Forwarded-Proto $scheme;\r\n"
    L"    }\r\n"
    L"}\r\n";

static const wchar_t* DEFAULT_VHOST_HTTPS =
    L"server {\r\n"
    L"    listen {{PORT}} ssl;\r\n"
    L"    server_name {{DOMAIN}};\r\n"
    L"    root {{ROOT}};\r\n"
    L"    index index.html index.htm;\r\n"
    L"    ssl_certificate     {{CERT}};\r\n"
    L"    ssl_certificate_key {{KEY}};\r\n"
    L"    ssl_protocols TLSv1.2 TLSv1.3;\r\n"
    L"    location / {\r\n"
    L"        try_files $uri $uri/ @nodejs;\r\n"
    L"    }\r\n"
    L"    location @nodejs {\r\n"
    L"        proxy_pass http://127.0.0.1:{{NODEJS_PORT}};\r\n"
    L"        proxy_set_header Host $host;\r\n"
    L"        proxy_set_header X-Real-IP $remote_addr;\r\n"
    L"        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;\r\n"
    L"        proxy_set_header X-Forwarded-Proto $scheme;\r\n"
    L"    }\r\n"
    L"}\r\n";

bool genNginxConfig(const std::wstring& ver) {
    std::wstring prefix = compDataVerDir(Comp::Nginx, ver);
    if (!makeDirs(joinPath(prefix, L"conf"))) return false;
    if (!makeDirs(joinPath(prefix, L"logs"))) return false;
    if (!makeDirs(logsDir())) return false;
    std::wstring vhostDir = joinPath(prefix, L"conf\\vhosts");
    if (!makeDirs(vhostDir)) return false;
    // nginx temp dirs (referenced by relative temp/* paths in default conf)
    if (!makeDirs(joinPath(prefix, L"temp"))) return false;
    for (auto d : {L"client_body_temp", L"proxy_temp", L"fastcgi_temp",
                   L"uwsgi_temp", L"scgi_temp"}) {
        if (!makeDirs(joinPath(prefix, L"temp\\" + std::wstring(d)))) return false;
    }
    // copy mime.types if missing
    std::wstring mimeSrc = joinPath(compBinDirVer(Comp::Nginx, ver), L"conf\\mime.types");
    std::wstring mimeDst = joinPath(prefix, L"conf\\mime.types");
    if (fileExists(mimeSrc) && !fileExists(mimeDst)) copyFileW2(mimeSrc, mimeDst);

    std::wstring tpl = readFileText(joinPath(compEtcDir(Comp::Nginx), L"nginx.conf.tpl"));
    if (tpl.empty()) tpl = DEFAULT_NGINX_CONF;

    std::map<std::wstring, std::wstring> kv;
    kv[L"PORT"] = nginxPort();
    // nginx runs with -p <data>\nginx\<ver>; use relative paths so the whole
    // folder tree is portable to another machine (mime.types is copied into
    // <prefix>\conf by genNginxConfig above, so it resolves relative to prefix)
    kv[L"WWW_DIR"] = L"../../../www";
    kv[L"MIME"] = L"mime.types";

    std::wstring conf = renderTemplate(tpl, kv);
    if (!writeFileText(joinPath(prefix, L"conf\\nginx.conf"), conf)) return false;

    // copy vhost configs from etc into runtime prefix (skip _template.conf)
    std::wstring srcVhost = joinPath(compEtcDir(Comp::Nginx), L"vhosts");
    // clear stale runtime vhosts first
    for (auto& f : listFiles(vhostDir, L"conf")) {
        DeleteFileW(joinPath(vhostDir, f).c_str());
    }
    for (auto& f : listFiles(srcVhost, L"conf")) {
        if (!f.empty() && f[0] == L'_') continue;
        copyFileW2(joinPath(srcVhost, f), joinPath(vhostDir, f));
    }
    return true;
}

bool genRedisConfig(const std::wstring& ver) {
    std::wstring dir = compDataVerDir(Comp::Redis, ver);
    if (!makeDirs(dir)) return false;
    if (!makeDirs(logsDir())) return false;
    std::wstring tpl = readFileText(joinPath(compEtcDir(Comp::Redis), L"redis.conf.tpl"));
    if (tpl.empty()) tpl = DEFAULT_REDIS_CONF;

    std::map<std::wstring, std::wstring> kv;
    kv[L"PORT"] = redisPort();
    // relative to the redis work dir (bin\redis\<ver>) so the folder tree is portable
    kv[L"PIDFILE"] = L"../../../data/redis/" + ver + L"/redis.pid";
    kv[L"LOGFILE"] = L"../../../logs/redis-" + ver + L".log";
    kv[L"DIR"] = L"../../../data/redis/" + ver;

    std::wstring conf = renderTemplate(tpl, kv);
    return writeFileText(joinPath(dir, L"redis.conf"), conf);
}

bool genPgConfig(const std::wstring& ver, const std::wstring& dataDir) {
    std::wstring tpl = readFileText(joinPath(compEtcDir(Comp::Postgresql), L"postgresql.conf.append"));
    if (tpl.empty()) tpl = DEFAULT_PG_APPEND;
    std::map<std::wstring, std::wstring> kv;
    kv[L"PORT"] = pgPort();
    std::wstring append = renderTemplate(tpl, kv);
    // Append to generated postgresql.conf (later values win in PG)
    std::wstring confPath = joinPath(dataDir, L"postgresql.conf");
    std::wstring existing = readFileText(confPath);
    existing += L"\r\n" + append;
    return writeFileText(confPath, existing);
}

// ============================ Pg connection settings ============================

std::wstring pgUser()     { return iniGet(L"pg.user", L"postgres"); }
std::wstring pgPassword() { return iniGet(L"pg.password", L"postgres"); }
std::wstring pgPort()     { return iniGet(L"pg.port", L"5432"); }
std::wstring nginxPort()  { return iniGet(L"nginx.port", L"80"); }
std::wstring redisPort()  { return iniGet(L"redis.port", L"6379"); }
std::wstring nodejsPort() { return iniGet(L"nodejs.port", L"3000"); }

// ============================ nginx ============================

bool nginxTestConfig(const std::wstring& ver, std::wstring& out) {
    std::wstring exe;
    if (!findExe(Comp::Nginx, ver, L"nginx.exe", exe)) { out = L"未找到 nginx.exe"; return false; }
    std::wstring prefix = compDataVerDir(Comp::Nginx, ver);
    // Pass the conf as an absolute path so nginx doesn't try to resolve it
    // relative to the prefix (newer nginx versions reject the joined path
    // in some setups, e.g. when the prefix contains a forward-slash
    // segment the parser doesn't recognize).
    std::wstring conf = joinPath(prefix, L"conf\\nginx.conf");
    std::wstring prefixArg = toForward(prefix);
    RunResult r = runProcessCapture(exe,
        L"-t -p \"" + prefixArg + L"\" -c \"" + toForward(conf) + L"\"",
        prefix, 15000);
    out = r.output;
    return r.ok;
}

bool nginxReload(std::wstring& err) {
    ComponentStatus st = compStatus(Comp::Nginx);
    if (!st.installed) { err = L"nginx 未安装"; return false; }
    if (!nginxRunningVer(st.currentVersion)) {
        // not running - just regenerate config
        if (!genNginxConfig(st.currentVersion)) { err = L"生成配置失败"; return false; }
        std::wstring tOut;
        if (!nginxTestConfig(st.currentVersion, tOut)) { err = L"nginx -t 失败: " + tOut; return false; }
        return true;
    }
    std::wstring exe;
    if (!findExe(Comp::Nginx, st.currentVersion, L"nginx.exe", exe)) { err = L"未找到 nginx.exe"; return false; }
    std::wstring prefix = compDataVerDir(Comp::Nginx, st.currentVersion);
    if (!genNginxConfig(st.currentVersion)) { err = L"生成配置失败"; return false; }
    RunResult r = runProcessCapture(exe, L"-s reload -p \"" + toForward(prefix) + L"\"", prefix, 10000);
    if (!r.ok) err = r.output;
    return r.ok;
}

std::vector<VHost> nginxListVHosts() {
    std::vector<VHost> result;
    std::wstring dir = joinPath(compEtcDir(Comp::Nginx), L"vhosts");
    for (auto& f : listFiles(dir, L"conf")) {
        if (!f.empty() && f[0] == L'_') continue;
        VHost v;
        v.name = f.substr(0, f.size() - 5); // strip .conf
        std::wstring content = readFileText(joinPath(dir, f));
        // parse server_name, listen, root
        size_t p1 = content.find(L"server_name");
        if (p1 != std::wstring::npos) {
            size_t b = content.find(L" ", p1 + 11); 
            size_t e = content.find(L";", p1);
            if (b != std::wstring::npos && e != std::wstring::npos && b < e)
                v.domain = trimStr(content.substr(b + 1, e - b - 1));
        }
        p1 = content.find(L"listen");
        if (p1 != std::wstring::npos) {
            size_t e = content.find(L";", p1);
            if (e != std::wstring::npos) {
                std::wstring line = trimStr(content.substr(p1 + 6, e - p1 - 6));
                // listen may be "80;" or "80 default_server;"
                std::wstringstream ss(line);
                ss >> v.port;
            }
        }
        p1 = content.find(L"root");
        if (p1 != std::wstring::npos) {
            size_t b = content.find(L" ", p1 + 4);
            size_t e = content.find(L";", p1);
            if (b != std::wstring::npos && e != std::wstring::npos && b < e)
                v.root = trimStr(content.substr(b + 1, e - b - 1));
        }
        result.push_back(v);
    }
    return result;
}

bool nginxAddVHostEx(const std::wstring& name, const std::wstring& domain,
                     const std::wstring& port, bool ssl,
                     const std::wstring& certPath, const std::wstring& keyPath,
                     const std::wstring& root, std::wstring& err) {
    if (name.empty() || domain.empty()) { err = L"站点名和域名不能为空"; return false; }
    // validate name (no path chars)
    if (name.find_first_of(L"\\/:. *?\"<>|") != std::wstring::npos) {
        err = L"站点名含非法字符"; return false;
    }
    if (ssl) {
        if (certPath.empty() || keyPath.empty()) { err = L"HTTPS 站点需要提供证书和 key 文件"; return false; }
        if (!fileExists(certPath)) { err = L"证书文件不存在: " + certPath; return false; }
        if (!fileExists(keyPath)) { err = L"key 文件不存在: " + keyPath; return false; }
    }
    // port conflict check
    ComponentStatus st = compStatus(Comp::Nginx);
    if (st.installed) {
        for (auto& v : nginxListVHosts()) {
            if (v.port == port && v.name != name) {
                err = L"端口 " + port + L" 已被站点 " + v.name + L" 占用";
                return false;
            }
        }
    }
    std::wstring vhostDir = joinPath(compEtcDir(Comp::Nginx), L"vhosts");
    if (!makeDirs(vhostDir)) { err = L"无法创建 vhosts 目录"; return false; }

    // site root: user-provided path, or the app-managed www\<name>
    bool appManaged = root.empty();
    std::wstring siteRoot = appManaged ? joinPath(wwwDir(), name) : root;
    if (siteRoot.empty()) { err = L"站点根目录为空"; return false; }
    // user-provided root must already exist; the app-managed one is created below
    if (!appManaged && !dirExists(siteRoot)) { err = L"根目录不存在: " + siteRoot; return false; }

    std::map<std::wstring, std::wstring> kv;
    kv[L"PORT"] = port.empty() ? nginxPort() : port;
    kv[L"DOMAIN"] = domain;
    // app-managed roots use a relative path (portable); user-picked roots stay absolute
    kv[L"ROOT"] = appManaged ? (L"../../../www/" + name) : toForward(siteRoot);
    kv[L"NODEJS_PORT"] = nodejsPort();
    if (ssl) {
        kv[L"CERT"] = toForward(certPath);
        kv[L"KEY"] = toForward(keyPath);
    }

    std::wstring tplFile = joinPath(vhostDir, ssl ? L"_template_https.conf" : L"_template.conf");
    std::wstring tpl = readFileText(tplFile);
    if (tpl.empty()) tpl = ssl ? DEFAULT_VHOST_HTTPS : DEFAULT_VHOST;
    std::wstring conf = renderTemplate(tpl, kv);

    std::wstring file = joinPath(vhostDir, name + L".conf");
    if (!writeFileText(file, conf)) { err = L"写入站点配置失败"; return false; }

    if (appManaged) {
        // app-managed site: create default page + ssl folder for certificates
        makeDirs(siteRoot);
        makeDirs(joinPath(siteRoot, L"ssl"));
        std::wstring idx = joinPath(siteRoot, L"index.html");
        if (!fileExists(idx)) {
            writeFileText(idx, L"<html><head><title>" + name + L"</title></head><body><h1>" + name + L"</h1></body></html>");
        }
    }

    if (!nginxReload(err)) return false;
    return true;
}

bool nginxAddVHost(const std::wstring& name, const std::wstring& domain,
                   const std::wstring& port, std::wstring& err) {
    return nginxAddVHostEx(name, domain, port, false, L"", L"", L"", err);
}

bool nginxRemoveVHost(const std::wstring& name, std::wstring& err) {
    if (name.empty()) { err = L"站点名为空"; return false; }
    std::wstring file = joinPath(joinPath(compEtcDir(Comp::Nginx), L"vhosts"), name + L".conf");
    if (!fileExists(file)) { err = L"站点不存在: " + name; return false; }
    if (!DeleteFileW(file.c_str())) { err = L"删除文件失败"; return false; }
    if (!nginxReload(err)) return false;
    return true;
}

// ============================ postgresql ============================

bool pgDataInitialized(const std::wstring& ver) {
    return fileExists(joinPath(pgDataDir(Comp::Postgresql, ver), L"PG_VERSION"));
}

bool pgInit(Comp c, const std::wstring& ver, const std::wstring& user,
            const std::wstring& password, const std::wstring& port, std::wstring& err) {
    std::wstring dataDir = pgDataDir(c, ver);
    if (pgDataInitialized(ver)) { err = L"数据库已初始化"; return false; }
    if (!makeDirs(dataDir)) { err = L"无法创建数据目录"; return false; }

    std::wstring initdb;
    if (!findExe(Comp::Postgresql, ver, L"initdb.exe", initdb)) {
        err = L"未找到 initdb.exe"; return false;
    }

    // write temp pw file to system temp (outside data dir so initdb sees empty dir)
    wchar_t tmpDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpDir);
    std::wstring pwFile = joinPath(tmpDir, L"lnpp_pwfile.tmp");
    writeFileText(pwFile, password);

    std::wstring args = L"-D \"" + dataDir + L"\" -U " + user +
                        L" -E UTF8 --locale=C -A scram-sha-256 --pwfile=\"" + pwFile + L"\"";

    RunResult r = runProcessCapture(initdb, args, compBinDirVer(c, ver), 120000);
    DeleteFileW(pwFile.c_str());
    if (!r.ok) {
        err = L"initdb 失败: " + r.output;
        return false;
    }

    // persist settings
    iniSet(L"pg.user", user);
    iniSet(L"pg.password", password);
    iniSet(L"pg.port", port.empty() ? L"5432" : port);

    if (!genPgConfig(ver, dataDir)) { err = L"生成配置失败"; return false; }
    return true;
}

bool pgChangePassword(Comp c, const std::wstring& user, const std::wstring& password, std::wstring& err) {
    std::wstring ver = iniGet(L"ver.postgresql", L"");
    if (ver.empty()) { err = L"未选择 PostgreSQL 版本"; return false; }
    std::wstring psql;
    if (!findExe(Comp::Postgresql, ver, L"psql.exe", psql)) { err = L"未找到 psql.exe"; return false; }
    std::wstring cmd = L"-h 127.0.0.1 -p " + pgPort() + L" -U " + pgUser() +
                       L" -d postgres -c \"ALTER USER " + user + L" WITH PASSWORD '" + password + L"';\"";

    RunResult r = runProcessCapture(psql, cmd, compBinDirVer(c, ver), 15000,
                                    {{L"PGPASSWORD", pgPassword()}});
    if (!r.ok) { err = r.output.empty() ? L"修改密码失败" : r.output; return false; }
    if (lowerStr(user) == lowerStr(pgUser())) iniSet(L"pg.password", password);
    return true;
}

bool pgCreateUser(Comp c, const std::wstring& user, const std::wstring& password, std::wstring& err) {
    std::wstring ver = iniGet(L"ver.postgresql", L"");
    if (ver.empty()) { err = L"未选择 PostgreSQL 版本"; return false; }
    std::wstring psql;
    if (!findExe(Comp::Postgresql, ver, L"psql.exe", psql)) { err = L"未找到 psql.exe"; return false; }
    std::wstring cmd = L"-h 127.0.0.1 -p " + pgPort() + L" -U " + pgUser() +
                       L" -d postgres -c \"CREATE USER " + user + L" WITH PASSWORD '" + password + L"';\"";

    RunResult r = runProcessCapture(psql, cmd, compBinDirVer(c, ver), 15000,
                                    {{L"PGPASSWORD", pgPassword()}});
    if (!r.ok) { err = r.output.empty() ? L"创建用户失败" : r.output; return false; }
    return true;
}

bool pgDropUser(Comp c, const std::wstring& user, std::wstring& err) {
    std::wstring ver = iniGet(L"ver.postgresql", L"");
    if (ver.empty()) { err = L"未选择 PostgreSQL 版本"; return false; }
    std::wstring psql;
    if (!findExe(Comp::Postgresql, ver, L"psql.exe", psql)) { err = L"未找到 psql.exe"; return false; }
    std::wstring cmd = L"-h 127.0.0.1 -p " + pgPort() + L" -U " + pgUser() +
                       L" -d postgres -c \"DROP USER IF EXISTS " + user + L";\"";
    RunResult r = runProcessCapture(psql, cmd, compBinDirVer(c, ver), 15000,
                                    {{L"PGPASSWORD", pgPassword()}});
    if (!r.ok) { err = r.output.empty() ? L"删除用户失败" : r.output; return false; }
    return true;
}

bool pgListUsers(std::vector<std::wstring>& users, std::wstring& err) {
    users.clear();
    std::wstring ver = iniGet(L"ver.postgresql", L"");
    if (ver.empty()) { err = L"未选择 PostgreSQL 版本"; return false; }
    if (!pgRunningVer(Comp::Postgresql, ver)) { err = L"PostgreSQL 未运行"; return false; }
    std::wstring psql;
    if (!findExe(Comp::Postgresql, ver, L"psql.exe", psql)) { err = L"未找到 psql.exe"; return false; }
    std::wstring cmd = L"-h 127.0.0.1 -p " + pgPort() + L" -U " + pgUser() +
                       L" -d postgres -t -A -c \"SELECT rolname FROM pg_roles WHERE rolname NOT LIKE 'pg\\_%' ORDER BY 1\"";
    RunResult r = runProcessCapture(psql, cmd, compBinDirVer(Comp::Postgresql, ver), 15000,
                                    {{L"PGPASSWORD", pgPassword()}});
    if (!r.ok) { err = r.output; return false; }
    std::wstringstream ss(r.output);
    std::wstring line;
    while (std::getline(ss, line)) {
        line = trimStr(line);
        if (!line.empty()) users.push_back(line);
    }
    return true;
}

bool pgBackup(Comp c, std::wstring& backupFile, std::wstring& err) {
    std::wstring ver = iniGet(L"ver.postgresql", L"");
    if (ver.empty()) { err = L"未选择 PostgreSQL 版本"; return false; }
    // must be running to dump
    if (!pgRunningVer(c, ver)) {
        err = L"PostgreSQL 未运行，无法备份";
        return false;
    }
    std::wstring pgDumpall;
    if (!findExe(Comp::Postgresql, ver, L"pg_dumpall.exe", pgDumpall)) {
        err = L"未找到 pg_dumpall.exe"; return false;
    }
    makeDirs(backupDir());
    if (backupFile.empty())
        backupFile = joinPath(backupDir(), L"postgresql-" + ver + L"-" + nowStamp() + L".sql");
    std::wstring cmd = L"-h 127.0.0.1 -p " + pgPort() + L" -U " + pgUser() +
                       L" -f \"" + backupFile + L"\"";
    RunResult r = runProcessCapture(pgDumpall, cmd, compBinDirVer(c, ver), 120000,
                                    {{L"PGPASSWORD", pgPassword()}});
    if (!r.ok) {
        err = L"pg_dumpall 失败: " + r.output;
        return false;
    }
    return true;
}

// ============================ redis ============================

bool redisTestConfig(const std::wstring& ver, std::wstring& out) {
    // redis-server --test-memory is overkill; just verify config parses by launching briefly
    std::wstring exe;
    if (!findExe(Comp::Redis, ver, L"redis-server.exe", exe)) { out = L"未找到 redis-server.exe"; return false; }
    std::wstring conf = joinPath(compDataVerDir(Comp::Redis, ver), L"redis.conf");
    ProcInfo pi;
    if (!startProcessDetached(exe, L"\"" + conf + L"\"", compBinDirVer(Comp::Redis, ver), pi)) {
        out = L"启动失败";
        return false;
    }
    WaitForSingleObject(pi.hProcess, 2000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return code == STILL_ACTIVE || redisRunningVer(ver);
}

// ============================ nodejs / pm2 ============================

static std::vector<PM2App> parsePm2List(const std::wstring& json) {
    std::vector<PM2App> apps;
    // jlist returns array of objects; split by top-level objects
    size_t pos = 0;
    while (pos < json.size()) {
        // find next '{'
        size_t open = json.find(L'{', pos);
        if (open == std::wstring::npos) break;
        // find matching '}' accounting for nested braces and strings
        int depth = 0;
        bool inStr = false;
        wchar_t strQuote = 0;
        size_t end = open;
        for (; end < json.size(); ++end) {
            wchar_t c = json[end];
            if (inStr) {
                if (c == L'\\') { ++end; continue; }
                if (c == strQuote) inStr = false;
                continue;
            }
            if (c == L'"') { inStr = true; strQuote = c; continue; }
            if (c == L'{') ++depth;
            else if (c == L'}') { --depth; if (depth == 0) break; }
        }
        if (end >= json.size()) break;
        std::wstring obj = json.substr(open, end - open + 1);

        PM2App app;
        // extract pm_id
        size_t k = obj.find(L"\"pm_id\"");
        if (k != std::wstring::npos) {
            size_t colon = obj.find(L':', k);
            size_t s = obj.find_first_of(L"0123456789", colon);
            size_t e2 = s;
            while (e2 < obj.size() && iswdigit(obj[e2])) e2++;
            app.id = (s != std::wstring::npos) ? _wtoi(obj.substr(s, e2 - s).c_str()) : -1;
        }
        // extract name
        k = obj.find(L"\"name\"");
        if (k != std::wstring::npos) {
            size_t colon = obj.find(L':', k);
            size_t q1 = obj.find(L'"', colon);
            size_t q2 = obj.find(L'"', q1 + 1);
            if (q1 != std::wstring::npos && q2 != std::wstring::npos)
                app.name = obj.substr(q1 + 1, q2 - q1 - 1);
        }
        // extract status
        k = obj.find(L"\"status\"");
        if (k != std::wstring::npos) {
            size_t colon = obj.find(L':', k);
            size_t q1 = obj.find(L'"', colon);
            size_t q2 = obj.find(L'"', q1 + 1);
            if (q1 != std::wstring::npos && q2 != std::wstring::npos)
                app.status = obj.substr(q1 + 1, q2 - q1 - 1);
        }
        // exec_interpreter (nested under pm2_env)
        k = obj.find(L"\"exec_interpreter\"");
        if (k != std::wstring::npos) {
            size_t colon = obj.find(L':', k);
            size_t q1 = obj.find(L'"', colon);
            size_t q2 = obj.find(L'"', q1 + 1);
            if (q1 != std::wstring::npos && q2 != std::wstring::npos)
                app.interpreter = obj.substr(q1 + 1, q2 - q1 - 1);
        }
        // restarts
        k = obj.find(L"\"restart_time\"");
        if (k == std::wstring::npos) k = obj.find(L"\"restart_time\":");
        if (k != std::wstring::npos) {
            size_t colon = obj.find(L':', k);
            size_t s = obj.find_first_of(L"0123456789", colon);
            size_t e2 = s;
            while (e2 < obj.size() && iswdigit(obj[e2])) e2++;
            app.restarts = (s != std::wstring::npos) ? _wtoi(obj.substr(s, e2 - s).c_str()) : 0;
        }
        if (app.id >= 0 && !app.name.empty())
            apps.push_back(app);
        pos = end + 1;
    }
    return apps;
}

std::vector<PM2App> nodePm2List() {
    // don't let pm2 jlist re-spawn a stopped daemon
    if (!nodeDaemonRunning()) return {};
    RunResult r = runPm2({L"jlist"});
    if (!r.ok) return {};
    return parsePm2List(r.output);
}

bool nodePm2Restart(int id, std::wstring& err) {
    RunResult r = runPm2({L"restart", std::to_wstring(id)});
    if (!r.ok) err = r.output.empty() ? L"pm2 restart 失败" : r.output;
    return r.ok;
}

bool nodePm2RestartAll(std::wstring& err) {
    RunResult r = runPm2({L"restart", L"all"});
    if (!r.ok) err = r.output.empty() ? L"pm2 restart all 失败" : r.output;
    return r.ok;
}

bool nodePm2Delete(int id, std::wstring& err) {
    RunResult r = runPm2({L"delete", std::to_wstring(id)});
    if (!r.ok) err = r.output.empty() ? L"pm2 delete 失败" : r.output;
    return r.ok;
}

bool nodePm2Stop(int id, std::wstring& err) {
    RunResult r = runPm2({L"stop", std::to_wstring(id)});
    if (!r.ok) err = r.output.empty() ? L"pm2 stop 失败" : r.output;
    return r.ok;
}

bool nodePm2Resurrect(std::wstring& err) {
    RunResult r = runPm2({L"resurrect"});
    // resurrect returns non-zero if no dump; that's ok
    if (!r.ok && r.output.find(L"File not found") == std::wstring::npos &&
        r.output.find(L"error") == std::wstring::npos) {
        err = r.output.empty() ? L"pm2 resurrect 失败" : r.output;
        return false;
    }
    // ensure every restored app runs on the bundled node (a bare "node"
    // interpreter resolves via pm2's own lookup, which ignores PATH and can
    // point at a removed system install -> apps start but never listen)
    std::wstring ver = iniGet(L"ver.nodejs", L"");
    std::wstring node = joinPath(compBinDirVer(Comp::Nodejs, ver), L"node.exe");
    if (!node.empty() && fileExists(node)) {
        std::wstring ln = lowerStr(node);
        for (auto& app : nodePm2List()) {
            if (!app.interpreter.empty() && lowerStr(app.interpreter) != ln) {
                runPm2({L"restart", std::to_wstring(app.id), L"--interpreter", node}, 20000);
            }
        }
    }
    return true;
}
