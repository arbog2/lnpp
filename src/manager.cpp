#include "manager.h"
#include "downloader.h"
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

// The directory the "配置" button opens. Under the etc=templates / data=runtime
// split, the live hand-editable configuration is always under data\, so opening
// etc\ would show template files that are never read at runtime — and for
// Node.js it showed a directory that does not exist at all.
std::wstring compConfigDir(Comp c) {
    // The version actually in use, or the component root when there is none.
    auto liveVerDir = [&](Comp comp) {
        std::wstring ver = iniGet(std::wstring(L"ver.") + compName(comp), L"");
        if (!ver.empty() && compVersionUsable(comp, ver)) return compDataVerDir(comp, ver);
        return compDataDir(comp);
    };
    switch (c) {
        // Site configs are the one thing hand-written per site. They are the
        // source of truth — do not point at the generated per-version copies
        // under data\nginx\<ver>\conf\vhosts, which are overwritten on start.
        case Comp::Nginx:      return nginxVhostSourceDir();
        case Comp::Postgresql: return liveVerDir(Comp::Postgresql);
        case Comp::Redis:      return liveVerDir(Comp::Redis);
        case Comp::Nodejs:     return compDataDir(Comp::Nodejs);
        default:               return compDataDir(c);
    }
}
std::wstring compBinDirVer(Comp c, const std::wstring& ver) {
    return joinPath(compBinDir(c), ver);
}

// Forward declarations: the SQL / command-line validators are defined in the
// postgresql section, but the version-switch migration code above it also
// builds a psql command line and needs them first.
static bool pgValidIdent(const std::wstring& s);
// Port whitelist shared by every command line and config value we build
// (psql / pg_dumpall / redis-cli / the nginx listen directive).
static bool validPort(const std::wstring& s);

// ============================ Operation locks ============================
// The message returned when an operation is refused because another thread is
// already driving the same component. Exposed as a predicate so callers can tell
// "someone else is already on it" — benign, and in the Node.js path the expected
// outcome — apart from a genuine failure, instead of reading the same string two
// different ways in two different places.
static const wchar_t* kCompBusyMsg = L"该组件正在执行其他操作，请稍候";

bool compIsBusy(const std::wstring& err) { return err == kCompBusyMsg; }

// One lock per component. autoStartComponents() fires compStart for all four
// components from four parallel threads, and compStart(Nodejs) additionally
// starts Redis as a side effect — so the same component really can be driven
// by two threads at once (two servers fighting over one port / data dir, or
// two config generators racing). try_lock makes the loser fail fast with a
// readable reason instead of corrupting state; recursive because
// compSwitchVersion holds the lock while calling compStop/compStart.
static std::recursive_mutex g_compOpMtx[(int)Comp::Count];

// Serialises config generation. Each gen*() function rewrites a file that other
// threads read (nginx -t, redis-server, postgres) and can append to
// postgresql.conf, so two overlapping runs are never what we want.
static std::mutex g_genCfgMtx;

// ============================ Runtime layout ============================
// The layout rule: etc\ carries templates only, data\ carries everything the
// program actually runs on. Called once from WinMain before any window exists.

// v1.5.3 moved the per-site vhost files out of etc\ (which now holds templates
// only) into data\. Migrate an existing set across so nobody silently loses
// their sites after upgrading.
static int migrateVhostSourceToData() {
    std::wstring from = joinPath(compEtcDir(Comp::Nginx), L"vhosts");
    std::wstring to = nginxVhostSourceDir();
    std::vector<std::wstring> stale;
    for (auto& f : listFiles(from, L"conf")) {
        if (f.rfind(L"_template", 0) == 0) continue;   // templates belong in etc
        stale.push_back(f);
    }
    if (stale.empty()) return 0;
    if (!makeDirs(to)) {
        logMsg(L"init", L"无法创建站点配置目录: " + to);
        return 0;
    }
    int moved = 0;
    for (auto& f : stale) {
        std::wstring src = joinPath(from, f);
        std::wstring dst = joinPath(to, f);
        if (fileExists(dst)) continue;                 // never clobber
        if (!copyFileW2(src, dst)) {
            logMsg(L"init", L"迁移站点配置失败（已保留原文件）: " + src);
            continue;
        }
        // Only remove the original once the copy is confirmed byte-for-byte
        // readable, so etc\ ends up holding templates only as promised.
        if (readFileText(dst).empty() && !readFileText(src).empty()) {
            logMsg(L"init", L"站点配置复制后校验失败（已保留原文件）: " + src);
            continue;
        }
        DeleteFileW(src.c_str());
        ++moved;
    }
    return moved;
}

void prepareRuntimeLayout() {
    // data\ is created if missing - everything the program runs on lives there.
    if (!makeDirs(dataDir())) {
        logMsg(L"init", L"无法创建数据目录: " + dataDir());
    }
    // Download list: data\packages.conf seeded from etc\packages.conf.tpl.
    prepareDownloadList();
    // Site configs: move etc\nginx\vhosts\*.conf into data\nginx\vhosts\.
    if (!makeDirs(nginxVhostSourceDir()))
        logMsg(L"init", L"无法创建站点配置目录: " + nginxVhostSourceDir());
    int moved = migrateVhostSourceToData();
    if (moved > 0) {
        logMsg(L"init", L"已把 " + std::to_wstring(moved) + L" 个站点配置从 etc\\nginx\\vhosts 迁移到 " +
                         nginxVhostSourceDir());
    }
}

// ============================ Component discovery ============================

// Natural (version-aware) descending comparison: "1.30" > "1.9", "2.0" > "1.99".
// Public so unit tests can verify the sort order without touching disk.
bool naturalGt(const std::wstring& a, const std::wstring& b) {
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
    auto pa = parts(a), pb = parts(b);
    size_t n = pa.size() > pb.size() ? pa.size() : pb.size();
    for (size_t i = 0; i < n; ++i) {
        auto va = i < pa.size() ? pa[i] : 0;
        auto vb = i < pb.size() ? pb[i] : 0;
        if (va != vb) return va > vb;
    }
    return lowerStr(a) > lowerStr(b);
}

std::vector<std::wstring> compVersions(Comp c) {
    std::vector<std::wstring> versions;
    for (const auto& ver : listSubDirs(compBinDir(c))) {
        if (compVersionUsable(c, ver)) versions.push_back(ver);
    }
    std::sort(versions.begin(), versions.end(), naturalGt);
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

// True only when `pid`'s executable image is exactly exePath. Checking the
// image before trusting a pidfile prevents a stale PID (reused by an
// unrelated process) from making the manager refuse to start, or worse from
// making Stop terminate the wrong process.
static bool processImageIs(DWORD pid, const std::wstring& exePath) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(h, 0, path, &size);
    CloseHandle(h);
    if (!ok) return false;
    return lowerStr(toForward(path)) == lowerStr(toForward(exePath));
}

// True when `pid` is an nginx.exe whose image lives under our own
// bin\nginx\<ver> directory. Matching by process name alone (the old code)
// made the manager "adopt" any nginx.exe on the system — its master pid then
// got written into our pidfile, and Stop/Reload would TerminateProcess a
// server we don't own.
static bool isOurNginx(DWORD pid, const std::wstring& ver) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(h, 0, path, &size);
    CloseHandle(h);
    if (!ok) return false;
    std::wstring exeDir = toForward(dirOf(path));
    std::wstring ours = toForward(compBinDirVer(Comp::Nginx, ver));
    std::wstring e = lowerStr(exeDir), o = lowerStr(ours);
    // Exact dir means nginx.exe lives in bin\nginx\<ver>. Also accept a
    // subdirectory under that version dir, while the separator check keeps
    // e.g. bin\nginx\1.30x from matching version 1.30.
    if (e.size() < o.size()) return false;
    if (e == o) return true;
    return e.compare(0, o.size(), o) == 0 && e[o.size()] == L'/';
}

static bool anyNginxRunning(const std::wstring& ver) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {0};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"nginx.exe") == 0 && isOurNginx(pe.th32ProcessID, ver)) {
                found = true; break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// nginx master = an nginx.exe process we own whose parent is not another
// nginx.exe (workers are children of the master; only the master answers
// -s quit / -s reload).
static DWORD nginxMasterPid(const std::wstring& ver) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {0};
    pe.dwSize = sizeof(pe);
    std::vector<DWORD> pids, ppids;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"nginx.exe") == 0 && isOurNginx(pe.th32ProcessID, ver)) {
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

// Image lives anywhere under our own bin\nginx\ tree (any version).
// Used to spot stray masters (e.g. started by double-click with the bin
// dir as prefix, or left behind by a previous version) that keep holding
// the listen ports and would otherwise wedge the next start / switch.
static bool isAnyOurNginx(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(h, 0, path, &size);
    CloseHandle(h);
    if (!ok) return false;
    std::wstring exeDir = toForward(dirOf(path));
    std::wstring ours = toForward(compBinDir(Comp::Nginx));
    std::wstring e = lowerStr(exeDir), o = lowerStr(ours);
    if (e.size() < o.size() + 1) return false;
    return e.compare(0, o.size(), o) == 0 && e[o.size()] == L'/';
}

static std::vector<DWORD> ourNginxPidsVer(const std::wstring& ver) {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe = {0};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"nginx.exe") == 0 && isOurNginx(pe.th32ProcessID, ver))
                out.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

static std::vector<DWORD> allOurNginxPids() {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe = {0};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"nginx.exe") == 0 && isAnyOurNginx(pe.th32ProcessID))
                out.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

// Graceful-quit every installed nginx version via both its data prefix
// (manager-started) and its bin prefix (stray: double-click / script),
// then force-kill leftovers. Runs before (re)start so a leftover master
// holding the listen ports cannot wedge the new instance.
static void stopOurNginxAll() {
    if (allOurNginxPids().empty()) return;
    for (auto& ver : compVersions(Comp::Nginx)) {
        if (!compVersionUsable(Comp::Nginx, ver)) continue;
        std::wstring exe;
        if (!findExe(Comp::Nginx, ver, L"nginx.exe", exe)) continue;
        std::wstring dataPrefix = compDataVerDir(Comp::Nginx, ver);
        std::wstring binPrefix = compBinDirVer(Comp::Nginx, ver);
        runProcessCapture(exe, L"-s quit -p \"" + toForward(dataPrefix) + L"\"", dataPrefix, 3000);
        runProcessCapture(exe, L"-s quit -p \"" + toForward(binPrefix) + L"\"", binPrefix, 3000);
    }
    for (int i = 0; i < 30; ++i) {
        if (allOurNginxPids().empty()) return;
        Sleep(100);
    }
    for (DWORD pid : allOurNginxPids()) killProcessByPid(pid);
    for (int i = 0; i < 30; ++i) {
        if (allOurNginxPids().empty()) return;
        Sleep(100);
    }
}

// `repairPidFile` lets the caller opt into rewriting a stale pidfile. The
// background status pollers must pass false: they run every 2-3s on two
// different threads, and a status *query* that writes the pidfile makes those
// threads race on the same nginx.pid.tmp (one MoveFileEx then fails).
static bool nginxRunningVer(const std::wstring& ver, bool repairPidFile) {
    DWORD pid = readPid(nginxPidFile(ver));
    if (pid && isPidAlive(pid) && isOurNginx(pid, ver)) return true;
    if (!anyNginxRunning(ver)) return false;
    if (repairPidFile) {
        // stale pidfile (e.g. nginx started outside the manager or the pidfile
        // was wiped): repoint it at the live master so `-s reload` / `-s quit`
        // address the right process
        DWORD mp = nginxMasterPid(ver);
        if (mp) writeFileText(nginxPidFile(ver), std::to_wstring(mp));
    }
    return true;
}

// A nginx that was started from bin\nginx\<ver> (double-click or a manual
// command) runs with the stock prefix and ignores the manager's data-prefix
// configuration. It writes its own bin-side pidfile, and nginxRunningVer may
// already have mirrored that PID into the data pidfile. Treat it as a stray
// so Start can clean it up instead of reporting "已在运行".
static bool nginxStrayRunning(const std::wstring& ver) {
    DWORD dataPid = readPid(nginxPidFile(ver));
    std::wstring binPidPath =
        joinPath(joinPath(compBinDirVer(Comp::Nginx, ver), L"logs"), L"nginx.pid");
    DWORD binPid = readPid(binPidPath);
    if (!binPid || !isPidAlive(binPid) || !isOurNginx(binPid, ver)) return false;
    return dataPid == 0 || dataPid == binPid;
}

// ---- postgresql ----
static std::wstring pgDataDir(Comp c, const std::wstring& ver) {
    return compDataVerDir(c, ver);
}

static bool pgRunningVer(Comp c, const std::wstring& ver) {
    std::wstring pidFile = joinPath(pgDataDir(c, ver), L"postmaster.pid");
    DWORD pid = readPid(pidFile);
    return pid && isPidAlive(pid) &&
           processImageIs(pid, compExe(Comp::Postgresql, ver, L"postgres.exe"));
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
    return pid && isPidAlive(pid) &&
           processImageIs(pid, compExe(Comp::Redis, ver, L"redis-server.exe"));
}

// ---- nodejs / pm2 ----

static std::wstring pm2HomeDir() {
    wchar_t home[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"PM2_HOME", home, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return L"";
        return joinPath(home, L".pm2");
    }
    return home;
}

// True when the process image's file name matches exeName, whatever directory
// it lives in. Needed for the pm2 daemon: PM2_HOME is shared per user
// (C:\Users\<user>\.pm2), so a perfectly live daemon may have been started by a
// different node.exe — a system-wide install or another tool. Demanding *our*
// bundled node here reported "not running" for a daemon that was up.
// Matching just the image name still blocks the real hazard, a stale pm2.pid
// whose PID was reused by an unrelated program.
static bool processImageNameIs(DWORD pid, const std::wstring& exeName) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(h, 0, path, &size);
    CloseHandle(h);
    if (!ok) return false;
    std::wstring name = path;
    size_t pos = name.find_last_of(L"\\/");
    if (pos != std::wstring::npos) name = name.substr(pos + 1);
    return lowerStr(name) == lowerStr(exeName);
}

// The daemon's RPC endpoint. pm2 for Windows uses a fixed pipe name (its own
// daemon log prints "RPC socket file: \\.\pipe\rpc.sock"), independent of
// PM2_HOME.
static const wchar_t* PM2_RPC_PIPE = L"\\\\.\\pipe\\rpc.sock";

// True when the daemon's RPC endpoint actually accepts connections.
//
// This — not pm2.pid — is what decides whether a pm2 command may run. A stale
// pm2.pid can point at a live node.exe while no pipe exists, and every command
// issued in that state makes the CLI spawn another daemon; that is exactly how
// a storm of hundreds of daemons built up on this machine (each spawn also
// failed, because the endpoint the new daemon tried to create never became
// connectable).
//
// Opening and immediately closing the pipe is a plain connect/disconnect, which
// the daemon already handles for every CLI that exits, so probing is safe.
static bool pm2PipeReachable() {
    HANDLE h = CreateFileW(PM2_RPC_PIPE, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// Circuit breaker. When the daemon cannot be brought up, hammering it on every
// poll is what turns a broken daemon into a spawn storm: each failed command
// starts another daemon. After a failure the manager stops touching pm2 for a
// while instead.
static const DWORD PM2_COOLDOWN_MS = 30000;
// Touched by the pm2 poller and by whatever operation thread is calling into
// pm2, so it has to be an atomic rather than a bare DWORD.
static std::atomic<DWORD> g_pm2DownUntil{0};

static bool pm2InCooldown() {
    // Wrap-safe comparison: true while now is still before the deadline.
    DWORD until = g_pm2DownUntil.load();
    return until != 0 && (long)(GetTickCount() - until) < 0;
}

static void pm2MarkDown() {
    g_pm2DownUntil.store(GetTickCount() + PM2_COOLDOWN_MS);
}

// True iff the pm2 God daemon is actually usable: its pidfile points at a live
// node process AND its RPC endpoint answers. Checked without invoking the CLI,
// because `pm2 ping` / `pm2 pid` / `pm2 jlist` all re-spawn the daemon when it
// is stopped.
static bool nodeDaemonRunning() {
    std::wstring homeDir = pm2HomeDir();
    if (homeDir.empty()) return false;
    DWORD pid = readPid(joinPath(homeDir, L"pm2.pid"));
    if (pid == 0 || !isPidAlive(pid)) return false;
    if (!processImageNameIs(pid, L"node.exe")) return false;   // stale pid, reused
    if (!pm2PipeReachable()) return false;                     // endpoint gone
    return true;
}

// Locate a usable pm2 and describe how to invoke it. A pm2.cmd is run
// directly; a pm2.js has to be handed to the bundled node.exe, because handing
// a .js file to CreateProcess always fails with ERROR_BAD_EXE_FORMAT — that
// fallback used to look implemented but could never run.
static bool nodePm2Cmd(const std::wstring& ver, std::wstring& exe, std::wstring& argPrefix) {
    std::wstring verDir = compBinDirVer(Comp::Nodejs, ver);
    std::wstring cmd = joinPath(verDir, L"pm2.cmd");
    if (fileExists(cmd)) { exe = cmd; argPrefix.clear(); return true; }
    std::wstring js = joinPath(verDir, L"node_modules\\pm2\\bin\\pm2.js");
    if (fileExists(js)) {
        exe = joinPath(verDir, L"node.exe");
        if (!fileExists(exe)) return false;
        argPrefix = L"\"" + toForward(js) + L"\" ";
        return true;
    }
    // fall back to pm2 in system PATH (global npm install)
    wchar_t buf[4096];
    DWORD n = GetEnvironmentVariableW(L"PATH", buf, 4096);
    if (n > 0) {
        std::wstring path(buf, n);
        std::wstringstream ss(path);
        std::wstring dir;
        while (std::getline(ss, dir, L';')) {
            std::wstring cand = joinPath(trimStr(dir), L"pm2.cmd");
            if (fileExists(cand)) { exe = cand; argPrefix.clear(); return true; }
        }
    }
    exe.clear();
    argPrefix.clear();
    return false;
}

bool nodePm2Installed() {
    ComponentStatus st = compStatus(Comp::Nodejs);
    if (!st.installed) return false;
    std::wstring pm2, prefix;
    return nodePm2Cmd(st.currentVersion, pm2, prefix);
}

// pm2's Windows daemon talks over the named pipe \\.\pipe\rpc.sock. When the
// daemon dies (or was force-killed, leaving pm2.pid behind) the CLI spawns a
// fresh daemon and tries to connect in the same breath; Windows answers that
// connect with EPERM instead of ENOENT, and the user gets a 30-line stack:
//     Error: connect EPERM \\.\pipe\rpc.sock
//     [PM2] Spawning PM2 daemon with pm2_home=...
// Recognize it so callers can retry once and report something actionable.
static bool pm2DaemonHandshakeError(const std::wstring& output) {
    return output.find(L"EPERM") != std::wstring::npos ||
           output.find(L"EPIPE") != std::wstring::npos ||
           output.find(L"rpc.sock") != std::wstring::npos ||
           output.find(L"Spawning PM2 daemon") != std::wstring::npos;
}

// Commands that only read state must never be the reason a daemon gets spawned:
// if the daemon is down they have nothing to report anyway.
static bool pm2CmdIsReadOnly(const std::vector<std::wstring>& args) {
    if (args.empty()) return false;
    const std::wstring& a = args[0];
    return a == L"jlist" || a == L"list" || a == L"ls" || a == L"status" ||
           a == L"ping" || a == L"pid";
}

// Wait for a daemon to become reachable. Polls the pipe (never the CLI), so
// waiting cannot itself spawn anything.
static bool waitForPm2Daemon(int timeoutMs) {
    for (int waited = 0; waited <= timeoutMs; waited += 250) {
        if (nodeDaemonRunning()) return true;
        Sleep(250);
    }
    return nodeDaemonRunning();
}

static RunResult runPm2(const std::vector<std::wstring>& args, int timeoutMs = 15000) {
    std::wstring ver = iniGet(L"ver.nodejs", L"");
    if (ver.empty()) return {};
    RunResult empty;
    if (!dirExists(compBinDirVer(Comp::Nodejs, ver))) return empty;

    // Circuit breaker: while the daemon is known-broken, do not touch pm2 at
    // all. Every failed pm2 command spawns another daemon, so retrying on a
    // broken install is what escalates a single failure into a storm.
    if (pm2InCooldown()) return empty;

    std::wstring pm2, argPrefix;
    if (nodePm2Cmd(ver, pm2, argPrefix)) {
        std::wstring cmdArgs = argPrefix;
        for (auto& a : args) {
            if (!cmdArgs.empty() && cmdArgs.back() != L' ') cmdArgs += L" ";
            cmdArgs += a;
        }
        std::wstring wd = compBinDirVer(Comp::Nodejs, ver);
        // prepend the portable node dir to PATH so `node` (used by pm2 daemon and
        // forked apps) resolves to LNPP's bundled node, not a system-wide install
        wchar_t pathBuf[32768];
        DWORD n = GetEnvironmentVariableW(L"PATH", pathBuf, 32768);
        std::wstring path = wd + L";" + (n > 0 ? std::wstring(pathBuf, n) : L"");
        std::map<std::wstring, std::wstring> env = {{L"PATH", path}};

        // Only run a command when the daemon's RPC endpoint is reachable. A
        // read-only command has nothing to report otherwise, and a write command
        // must not be what spawns a daemon: that spawn races the socket and is
        // the documented source of the rpc.sock EPERM failure.
        if (!nodeDaemonRunning()) {
            if (pm2CmdIsReadOnly(args)) return empty;
            // Give an operator-requested start exactly one chance to bring the
            // daemon up, then require the endpoint to actually answer.
            runProcessCapture(pm2, argPrefix + L"ping", wd, 20000, env);
            if (!waitForPm2Daemon(5000)) {
                logMsg(L"pm2", L"守护进程无法启动，进入 30s 冷却: " + args[0]);
                pm2MarkDown();
                empty.output = L"pm2 守护进程不可用（\\\\.\\pipe\\rpc.sock 不存在）。"
                               L"已暂停 30 秒内的后续调用以避免反复拉起守护进程，"
                               L"请稍后重试或在任务管理器结束残留的 pm2 守护进程。";
                return empty;
            }
        }

        RunResult r = runProcessCapture(pm2, cmdArgs, wd, timeoutMs, env);

        // A working command proves the daemon is healthy again: clear any
        // cooldown so normal polling resumes immediately.
        if (r.ok) g_pm2DownUntil.store(0);

        // A handshake failure means the endpoint died under us. Do NOT retry:
        // the retry is what doubled the spawn rate during the storm. Cool down
        // and tell the caller what happened.
        if (!r.ok && pm2DaemonHandshakeError(r.output)) {
            logMsg(L"pm2", L"守护进程握手失败，进入 30s 冷却: " + args[0] + L" / " + r.output);
            pm2MarkDown();
            r.output = L"pm2 守护进程状态异常（连接 \\\\.\\pipe\\rpc.sock 失败）。"
                       L"已在 30 秒内停止后续 pm2 调用，避免不断拉起新的守护进程；"
                       L"请稍后重试。\r\n--- pm2 输出 ---\r\n" + r.output;
        }
        return r;
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
        case Comp::Nginx:      return nginxRunningVer(st.currentVersion, /*repairPidFile=*/true);
        case Comp::Postgresql: return pgRunningVer(c, st.currentVersion);
        case Comp::Redis:      return redisRunningVer(st.currentVersion);
        case Comp::Nodejs:
            return nodeDaemonRunning();
        default: return false;
    }
}

// Lightweight liveness probe for background polling: identical to
// compIsRunning except nginx never rewrites the pidfile and Redis checks the
// pidfile only (no redis-cli spawn). Both pollers run on worker threads at the
// same time, so this path must stay free of side effects.
bool compRunningQuick(Comp c) {
    std::wstring ver = iniGet(std::wstring(L"ver.") + compName(c), L"");
    if (ver.empty()) return false;
    if (!dirExists(compBinDirVer(c, ver))) return false;
    switch (c) {
        case Comp::Nginx:      return nginxRunningVer(ver, /*repairPidFile=*/false);
        case Comp::Postgresql: return pgRunningVer(c, ver);
        case Comp::Redis: {
            DWORD pid = readPid(redisPidFile(ver));
            return isPidAlive(pid);
        }
        case Comp::Nodejs:     return nodeDaemonRunning();
        default: return false;
    }
}

// ---- Component log rotation ----
// nginx / redis / postgresql append to their log files for as long as they run,
// so a portable stack that is used daily accumulates logs without bound.
// Rotation can only happen while the component is *down*: on Windows a file a
// running server holds open cannot be renamed or deleted (the servers do not
// open their logs with FILE_SHARE_DELETE), and truncating a live log would just
// leave the writer appending at its old offset. compStart() therefore calls
// this right before spawning.
static const ULONGLONG COMP_LOG_MAX_BYTES = 8ull * 1024 * 1024;
static const int COMP_LOG_KEEP = 2;   // keep x.log.1 .. x.log.N

static void rotateOneLog(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;   // missing, or still held by a server
    LARGE_INTEGER size = {};
    BOOL gotSize = GetFileSizeEx(h, &size);
    CloseHandle(h);
    if (!gotSize || (ULONGLONG)size.QuadPart <= COMP_LOG_MAX_BYTES) return;

    DeleteFileW((path + L"." + std::to_wstring(COMP_LOG_KEEP)).c_str());
    for (int i = COMP_LOG_KEEP - 1; i >= 1; --i) {
        MoveFileExW((path + L"." + std::to_wstring(i)).c_str(),
                    (path + L"." + std::to_wstring(i + 1)).c_str(),
                    MOVEFILE_REPLACE_EXISTING);
    }
    if (MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING)) {
        logMsg(L"log", L"日志已轮转: " + path);
    }
}

// See the note above: the caller guarantees the component is not running.
void rotateCompLogs(Comp c, const std::wstring& ver) {
    if (ver.empty()) return;
    switch (c) {
        case Comp::Nginx: {
            std::wstring dir = joinPath(compDataVerDir(Comp::Nginx, ver), L"logs");
            rotateOneLog(joinPath(dir, L"access.log"));
            rotateOneLog(joinPath(dir, L"error.log"));
            return;
        }
        case Comp::Postgresql:
            rotateOneLog(joinPath(logsDir(), L"postgresql-" + ver + L".log"));
            return;
        case Comp::Redis:
            rotateOneLog(joinPath(logsDir(), L"redis-" + ver + L".log"));
            return;
        default:
            // nodejs: pm2 keeps its own logs under %USERPROFILE%\.pm2\logs
            return;
    }
}

// ============================ Housekeeping ============================
// Every PostgreSQL version switch leaves two artefacts behind: a full copy of
// the old cluster (data\postgresql\<ver>.old-<stamp>, ~120 MB) and a full
// pg_dumpall in backup\. Nothing pruned them, so on this machine the directory
// had reached 32 copies / 3.9 GB. Both are now trimmed, and the dropped ones go
// to the Recycle Bin rather than straight to the void: they are the only way
// back if a restore turns out to have been lossy.

static int keepCount(const wchar_t* key, int def) {
    std::wstring v = iniGet(key, L"");
    if (v.empty()) return def;
    int n = _wtoi(v.c_str());
    return n >= 0 ? n : def;
}

static ULONGLONG treeBytes(const std::wstring& path) {
    if (fileExists(path)) {
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d))
            return ((ULONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow;
        return 0;
    }
    ULONGLONG total = 0;
    for (auto& sub : listSubDirs(path)) total += treeBytes(joinPath(path, sub));
    for (auto& f : listFiles(path, L"*")) total += treeBytes(joinPath(path, f));
    return total;
}

// Recycle Bin delete, so pruning stays reversible.
static bool recycleTree(const std::wstring& path) {
    if (!dirExists(path) && !fileExists(path)) return true;
    std::wstring p = path;
    while (p.size() > 1 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    p.push_back(L'\0');
    p.push_back(L'\0');
    SHFILEOPSTRUCTW op = {0};
    op.wFunc = FO_DELETE;
    op.pFrom = p.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

// The trailing ".old-20260922_065605" / ".failed-..." stamp sorts correctly as
// text, so newest-first is a plain string comparison on that suffix.
static bool stampOf(const std::wstring& name, const wchar_t* marker, std::wstring& stamp) {
    size_t p = name.rfind(marker);
    if (p == std::wstring::npos) return false;
    stamp = name.substr(p + wcslen(marker));
    return !stamp.empty();
}

static void pruneStaleDataCopies(Comp c) {
    int keep = keepCount(L"keep.datacopies", 2);
    std::wstring root = compDataDir(c);
    struct Entry { std::wstring path, stamp; };
    std::vector<Entry> stale;
    for (auto& name : listSubDirs(root)) {
        std::wstring stamp;
        // ".before-restore-" belongs here too: pgRestoreBackup() parks the
        // pre-restore cluster under that name, and without it every restore
        // leaked another full copy that this cleanup could never see.
        if (stampOf(name, L".before-restore-", stamp) ||
            stampOf(name, L".old-", stamp) ||
            stampOf(name, L".failed-", stamp))
            stale.push_back({ joinPath(root, name), stamp });
    }
    if ((int)stale.size() <= keep) return;
    std::sort(stale.begin(), stale.end(),
              [](const Entry& a, const Entry& b) { return a.stamp > b.stamp; });
    ULONGLONG freed = 0;
    int dropped = 0;
    for (size_t i = (size_t)keep; i < stale.size(); ++i) {
        ULONGLONG sz = treeBytes(stale[i].path);
        if (recycleTree(stale[i].path)) { freed += sz; ++dropped; }
    }
    if (dropped > 0) {
        logMsg(L"maint", L"清理旧数据副本 " + std::to_wstring(dropped) + L" 个 (" +
                         std::to_wstring(freed / (1024 * 1024)) + L" MB)，已移入回收站");
    }
}

static void pruneOldBackups() {
    int keep = keepCount(L"keep.backups", 10);
    std::wstring dir = backupDir();
    auto files = listFiles(dir, L"sql");
    if ((int)files.size() <= keep) return;
    std::sort(files.begin(), files.end(), std::greater<std::wstring>());
    ULONGLONG freed = 0;
    int dropped = 0;
    for (size_t i = (size_t)keep; i < files.size(); ++i) {
        ULONGLONG sz = treeBytes(joinPath(dir, files[i]));
        if (recycleTree(joinPath(dir, files[i]))) { freed += sz; ++dropped; }
    }
    if (dropped > 0) {
        logMsg(L"maint", L"清理旧备份 " + std::to_wstring(dropped) + L" 个 (" +
                         std::to_wstring(freed / (1024 * 1024)) + L" MB)，已移入回收站");
    }
}

bool compStart(Comp c, std::wstring& err) {
    std::unique_lock<std::recursive_mutex> lk(g_compOpMtx[(int)c], std::try_to_lock);
    if (!lk.owns_lock()) { err = kCompBusyMsg; return false; }
    ComponentStatus st = compStatus(c);
    if (!st.installed) { err = L"组件未安装（bin 下无版本目录）"; return false; }
    bool alreadyRunning =
        st.running && (c != Comp::Nginx || !nginxStrayRunning(st.currentVersion));
    // A live pm2 daemon with an empty app list is NOT "already running": the
    // apps are simply gone (pm2 kill, a crash, or a resurrect that never ran).
    // Returning 已在运行 here is what left the Node page permanently empty while
    // the daemon happily answered every poll.
    if (alreadyRunning && c == Comp::Nodejs && nodePm2List().empty()) {
        alreadyRunning = false;
    }
    if (alreadyRunning) {
        err = L"已在运行";
        return false;
    }
    std::wstring ver = st.currentVersion;
    // We are down here, which is the only window in which the component's logs
    // can be rotated on Windows (see rotateCompLogs).
    rotateCompLogs(c, ver);
    // Starting a component is a natural maintenance window: the PostgreSQL
    // version switch leaves a full copy of the old cluster behind every time,
    // and the backup folder grows by one full dump per migration.
    if (c == Comp::Postgresql) {
        pruneStaleDataCopies(Comp::Postgresql);
        pruneOldBackups();
    }

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
            // Clear leftover masters (other version / stray prefix) still
            // holding the listen ports; otherwise the new master exits on
            // bind conflict and start fails with "no process detected".
            stopOurNginxAll();
            std::wstring prefix = compDataVerDir(Comp::Nginx, ver);
            ProcInfo pi;
            if (!startProcessDetached(exe, L"-p \"" + toForward(prefix) + L"\"", prefix, pi)) {
                err = L"启动 nginx 失败"; return false;
            }
            // Give it a moment to write pid file
            bool up = false;
            for (int i = 0; i < 20 && !up; ++i) {
                if (nginxRunningVer(ver, /*repairPidFile=*/true)) up = true;
                else Sleep(100);
            }
            // startProcessDetached() hands the caller the process handle; not
            // closing it leaked one handle per start (a tray-resident manager
            // that is started/stopped all day would creep towards the limit).
            CloseHandle(pi.hProcess);
            if (!up) { err = L"nginx 启动后未检测到进程"; return false; }
            return true;
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
            CloseHandle(pi.hProcess);
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
            bool up = false;
            for (int i = 0; i < 20 && !up; ++i) {
                if (redisRunningVer(ver)) up = true;
                else Sleep(100);
            }
            CloseHandle(pi.hProcess);
            if (!up) { err = L"Redis 启动后未检测到进程"; return false; }
            return true;
        }
        case Comp::Nodejs: {
            // pm2 apps commonly depend on Redis (a refused 6379 connection makes
            // them exit right after start, which pm2 then reports as a crashed
            // app). Bring Redis up first unless the user is starting it itself.
            // This path does NOT hold the Redis component lock, so if the user's
            // own Redis start is already in flight this fails fast on purpose —
            // two servers on one port would be worse.
            ComponentStatus rs = compStatus(Comp::Redis);
            if (rs.installed && !rs.running) {
                std::wstring rerr;
                if (!compStart(Comp::Redis, rerr)) {
                    // Not fatal either way: the app may not need Redis at all.
                    // Distinguish "another operation already owns Redis" — the
                    // expected outcome under autostart, which fires all four
                    // components in parallel — from a real failure, so the log
                    // says what happened instead of implying a fault.
                    if (compIsBusy(rerr))
                        logMsg(L"pm2", L"启动 Node.js 前跳过自动启动 Redis：另一个操作正在启动它");
                    else
                        logMsg(L"pm2", L"启动 Node.js 前自动启动 Redis 失败: " + rerr);
                }
            }
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
    std::unique_lock<std::recursive_mutex> lk(g_compOpMtx[(int)c], std::try_to_lock);
    if (!lk.owns_lock()) { err = kCompBusyMsg; return false; }
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
            // Graceful quit first: `-s quit` is *asynchronous* — the master
            // waits for workers to finish in-flight requests before exiting.
            // The old code checked the pidfile immediately after sending the
            // signal (still alive, of course) and force-killed, so the
            // graceful shutdown never actually happened. Poll up to 3s first.
            runProcessCapture(exe, L"-s quit -p \"" + toForward(prefix) + L"\"",
                              prefix, 10000);
            // Strays started with the bin dir as prefix (double-click etc.)
            // ignore signals sent to the data prefix; try that prefix too.
            std::wstring binPrefix = compBinDirVer(Comp::Nginx, ver);
            runProcessCapture(exe, L"-s quit -p \"" + toForward(binPrefix) + L"\"",
                              binPrefix, 5000);
            for (int i = 0; i < 30; ++i) {
                if (!nginxRunningVer(ver, /*repairPidFile=*/true)) return true;
                Sleep(100);
            }
            // grace period exhausted: force kill every process of this
            // version (the pidfile alone misses twin masters started
            // outside the manager - exactly how the ports stay wedged)
            for (DWORD pid : ourNginxPidsVer(ver)) killProcessByPid(pid);
            for (int i = 0; i < 30; ++i) {
                if (!nginxRunningVer(ver, /*repairPidFile=*/true)) return true;
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
                if (processImageIs(pid, compExe(Comp::Postgresql, ver, L"postgres.exe")))
                    killProcessByPid(pid);
            }
            if (pgRunningVer(c, ver)) {
                // Never return false with an empty err: callers prefix it
                // ("停止数据库失败: " + err) and an empty reason is useless.
                err = r.output.empty() ? L"pg_ctl stop 未能停止 postmaster" : r.output;
                return false;
            }
            return true;
        }
        case Comp::Redis: {
            // try shutdown via redis-cli
            std::wstring cli;
            std::wstring rport = redisPort();
            if (!validPort(rport)) rport = L"6379";
            if (findExe(Comp::Redis, ver, L"redis-cli.exe", cli)) {
                runProcessCapture(cli, L"-p " + rport + L" shutdown nosave",
                                  compBinDirVer(c, ver), 5000);
            }
            DWORD pid = readPid(redisPidFile(ver));
            if (processImageIs(pid, compExe(Comp::Redis, ver, L"redis-server.exe")))
                killProcessByPid(pid);
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
    // Recursive lock: compStop/compStart below re-acquire it on this thread.
    std::unique_lock<std::recursive_mutex> lk(g_compOpMtx[(int)c], std::try_to_lock);
    if (!lk.owns_lock()) { err = kCompBusyMsg; return false; }
    ComponentStatus st = compStatus(c);
    if (!st.installed) { err = L"组件未安装"; return false; }
    if (st.currentVersion == ver) { err = L"已是当前版本"; return false; }
    // verify new version dir exists
    if (!compVersionUsable(c, ver)) { err = L"版本不可用或目录不存在: " + ver; return false; }

    // Clear stray masters of any version before selecting the target. Without
    // this, a bin-prefix process of the target version is reported as
    // "already running" and the switch keeps its stale config instead of
    // restarting under the managed data prefix.
    if (c == Comp::Nginx) stopOurNginxAll();

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
            // poll for readiness instead of a fixed sleep (fast machines are
            // ready sooner; slow ones would otherwise race the dump)
            for (int i = 0; i < 60 && !pgRunningVer(c, oldVer); ++i) Sleep(200);
            if (!pgRunningVer(c, oldVer)) {
                err = L"旧版本启动后未就绪: " + oldVer;
                return false;
            }
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
        bool newDataMoved = false;
        std::wstring oldNewData;
        if (dirExists(newData)) {
            oldNewData = newData + L".old-" + nowStamp();
            if (!MoveFileW(newData.c_str(), oldNewData.c_str())) {
                err = L"无法移动旧数据目录（可能被占用）: " + newData;
                return false;
            }
            newDataMoved = true;
        }

        // Any failure after this point must put the previous version back.
        auto rollbackAfterFailure = [&](const std::wstring& reason,
                                        const std::wstring& backupHint,
                                        std::wstring& errOut) {
            std::wstring detail;
            if (pgRunningVer(c, ver)) {
                std::wstring serr;
                if (!compStop(c, serr))
                    detail += L"停止新版本失败: " + serr + L"；";
            }
            if (dirExists(newData)) {
                std::wstring failedDir = newData + L".failed-" + nowStamp();
                if (!MoveFileW(newData.c_str(), failedDir.c_str()))
                    detail += L"保留失败数据目录失败: " + failedDir + L"；";
            }
            if (newDataMoved && dirExists(oldNewData)) {
                if (MoveFileW(oldNewData.c_str(), newData.c_str()))
                    newDataMoved = false;
                else
                    detail += L"恢复原数据目录失败: " + oldNewData + L"；";
            }
            iniSet(std::wstring(L"ver.postgresql"), oldVer);
            if (oldWasRunning && !pgRunningVer(c, oldVer)) {
                std::wstring serr;
                if (!compStart(c, serr))
                    detail += L"重新启动旧版本失败: " + serr + L"；";
            }
            errOut = reason;
            if (!backupHint.empty()) errOut += L"（" + backupHint + L"）";
            if (!detail.empty()) errOut += L"；回滚提示: " + detail;
        };

        // init target with fresh dir
        // (set version first so subsequent compStart targets the new version)
        iniSet(std::wstring(L"ver.postgresql"), ver);
        std::wstring berr;
        if (!pgInit(c, ver, pgUser(), pgPassword(), pgPort(), berr)) {
            rollbackAfterFailure(L"初始化新版本失败: " + berr, L"", err);
            return false;
        }

        // restore backup into new
        if (!backupFile.empty()) {
            if (!compStart(c, berr)) {
                rollbackAfterFailure(L"启动新版本以恢复失败: " + berr, L"", err);
                return false;
            }
            for (int i = 0; i < 60 && !pgRunningVer(c, ver); ++i) Sleep(200);
            if (!pgRunningVer(c, ver)) {
                rollbackAfterFailure(L"新版本启动后未就绪: " + ver, L"", err);
                return false;
            }
            std::wstring psql;
            if (!findExe(Comp::Postgresql, ver, L"psql.exe", psql)) {
                rollbackAfterFailure(L"未找到 psql.exe", L"", err);
                return false;
            }
            // port/user come from settings.ini and land on the command line
            std::wstring port = pgPort();
            std::wstring user = pgUser();
            if (!validPort(port)) {
                rollbackAfterFailure(L"端口号无效: " + port, L"", err);
                return false;
            }
            if (!pgValidIdent(user)) {
                rollbackAfterFailure(L"用户名含非法字符: " + user, L"", err);
                return false;
            }
            std::wstring cmd = L"-h 127.0.0.1 -p " + port + L" -U " + user +
                               L" -d postgres -f \"" + backupFile + L"\"";
            RunResult r = runProcessCapture(psql, cmd, compBinDirVer(c, ver), 120000,
                                            {{L"PGPASSWORD", pgPassword()}});
            if (!r.ok) {
                rollbackAfterFailure(L"恢复备份失败: " + r.output,
                                     L"备份文件: " + backupFile, err);
                return false;
            }
        }
        // The new cluster is up and the dump was replayed: the pre-migration
        // copies and this run's intermediate backups are now surplus. Anything
        // dropped goes to the Recycle Bin, so a bad restore is still undoable.
        pruneStaleDataCopies(Comp::Postgresql);
        pruneOldBackups();
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
    L"{{DENY_UNKNOWN}}\r\n"
    L"    include vhosts/*.conf;\r\n"
    L"}\r\n";

static const wchar_t* DEFAULT_REDIS_CONF =
    // bind before the rest: without it Redis 5 listens on every interface, and
    // this template ships no requirepass, which is an unauthenticated Redis
    // exposed to the whole LAN. Keep it in sync with etc\redis\redis.conf.tpl.
    L"bind 127.0.0.1\r\n"
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

// The catch-all server blocks that live where the {{DENY_UNKNOWN}} placeholder
// is, i.e. inside http{} and before "include vhosts/*.conf".
//
// blockUnknown = true: every request whose Host / SNI matches no site is
// refused, so a direct-IP visit or a typo'd domain no longer lands on whatever
// happens to be first.
//   - :80  -> a plain HTTP 500.
//   - :443 -> refused during the TLS handshake (ssl_reject_handshake, nginx
//     >= 1.19.4). This is deliberately NOT `return 500` with a certificate:
//     a catch-all presenting a real cert only produces a certificate-mismatch
//     warning in front of the 500, which is worse for a human than a clean
//     handshake failure, and it would hand the block a key to protect.
// blockUnknown = false: the previous behaviour (a localhost-only site), kept as
// the escape hatch — nginx.block_unknown_host in settings.ini turns the whole
// thing off with one ini edit.
//
// Public so the unit tests can assert both renderings.
std::wstring nginxDenyBlock(bool blockUnknown) {
    if (!blockUnknown) {
        return
            L"    # nginx.block_unknown_host = false：只保留本机 localhost 站点。\r\n"
            L"    # 注意 :80 上第一个 server 块仍是隐式兜底，未匹配域名会命中这里。\r\n"
            L"    server {\r\n"
            L"        listen       80;\r\n"
            L"        server_name  localhost;\r\n"
            L"        location / {\r\n"
            L"            root   ../../../www;\r\n"
            L"            index  index.html index.htm;\r\n"
            L"        }\r\n"
            L"    }\r\n";
    }
    return
        L"    # ---- 拒绝 IP 直连与未配置的域名（nginx.block_unknown_host=true）----\r\n"
        L"    server {\r\n"
        L"        listen       80 default_server;\r\n"
        L"        server_name  _;\r\n"
        L"        return 500;\r\n"
        L"    }\r\n"
        L"    server {\r\n"
        L"        listen       443 ssl default_server;\r\n"
        L"        server_name  _;\r\n"
        L"        # 握手阶段就拒绝：不需要证书，也不暴露任何站点信息\r\n"
        L"        ssl_reject_handshake on;\r\n"
        L"    }\r\n";
}

// ---- nginx vhost source ----
// etc\ holds templates only. The per-site vhost files a user creates and edits
// are runtime configuration, so their source of truth now lives under data\
// next to the rest of nginx's state. genNginxConfig copies them into each
// version's runtime prefix (data\nginx\<ver>\conf\vhosts) because the
// "include vhosts/*.conf" in nginx.conf resolves relative to that prefix.
// (Declared in manager.h because prepareRuntimeLayout() needs it early.)
std::wstring nginxVhostSourceDir() {
    return joinPath(compDataDir(Comp::Nginx), L"vhosts");
}

// A site that claims default_server becomes the implicit catch-all for its
// port, which is exactly what this feature removes. It also collides with the
// blocks above ("a duplicate default server for 0.0.0.0:443" and nginx then
// refuses to start). nginx -t only names the port, never the file, so name it.
static void reportVhostDefaultServers() {
    static const wchar_t* kMarker = L"default_server";
    std::wstring dir = nginxVhostSourceDir();
    for (auto& f : listFiles(dir, L"conf")) {
        if (f.rfind(L"_template", 0) == 0) continue;
        std::wstring content = readFileText(joinPath(dir, f));
        size_t pos = 0;
        while ((pos = content.find(kMarker, pos)) != std::wstring::npos) {
            size_t lineNo = 1;
            for (size_t i = 0; i < pos && i < content.size(); ++i)
                if (content[i] == L'\n') ++lineNo;
            logMsg(L"nginx", L"站点配置 " + dir + L"\\" + f + L" 第 " +
                             std::to_wstring(lineNo) + L" 行含 default_server：它会让 IP/未配置"
                             L"域名命中该站点，并与兜底 server 冲突导致 nginx 启动失败，请删除该关键字");
            pos += wcslen(kMarker);
        }
    }
}

static const wchar_t* DEFAULT_VHOST =
    L"server {\r\n"
    L"    listen {{PORT}};\r\n"
    L"    server_name {{DOMAIN}};\r\n"
    L"    root \"{{ROOT}}\";\r\n"
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
    L"    root \"{{ROOT}}\";\r\n"
    L"    index index.html index.htm;\r\n"
    L"    ssl_certificate     \"{{CERT}}\";\r\n"
    L"    ssl_certificate_key \"{{KEY}}\";\r\n"
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
    std::lock_guard<std::mutex> lk(g_genCfgMtx);
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

    // Unknown domains / direct-IP access are refused unless the user turned the
    // block off. Read the toggle the same way every other "true" flag is read.
    bool blockUnknown = lowerStr(iniGet(L"nginx.block_unknown_host", L"true")) != L"false";

    std::map<std::wstring, std::wstring> kv;
    kv[L"PORT"] = nginxPort();
    // nginx runs with -p <data>\nginx\<ver>; use relative paths so the whole
    // folder tree is portable to another machine (mime.types is copied into
    // <prefix>\conf by genNginxConfig above, so it resolves relative to prefix)
    kv[L"WWW_DIR"] = L"../../../www";
    kv[L"MIME"] = L"mime.types";
    kv[L"DENY_UNKNOWN"] = nginxDenyBlock(blockUnknown);

    std::wstring conf = renderTemplate(tpl, kv);
    // A template saved before this feature has no {{DENY_UNKNOWN}} placeholder,
    // and the catch-all would then silently never appear — the server would stay
    // wide open with no error anywhere. nginx -t cannot catch that, so say so.
    if (blockUnknown && conf.find(L"default_server") == std::wstring::npos) {
        logMsg(L"nginx", L"etc\\nginx\\nginx.conf.tpl 里没有 {{DENY_UNKNOWN}} 占位符，"
                         L"拒绝 IP 直连/未配置域名的兜底 server 块没有生成，nginx 仍会对"
                         L"未知域名开放。请在该模板的 http { } 内、include vhosts/*.conf 之前"
                         L"加一行 {{DENY_UNKNOWN}}");
    }
    reportVhostDefaultServers();
    if (!writeFileText(joinPath(prefix, L"conf\\nginx.conf"), conf)) return false;

    // copy the user's site configs into the runtime prefix. The source of truth
    // is data\nginx\vhosts (etc\ keeps templates only); each nginx version has
    // its own runtime prefix because "include vhosts/*.conf" resolves relative
    // to the -p prefix.
    std::wstring srcVhost = nginxVhostSourceDir();
    if (!makeDirs(srcVhost)) {
        logMsg(L"nginx", L"无法创建站点配置目录: " + srcVhost);
    }
    // clear stale runtime vhosts first
    for (auto& f : listFiles(vhostDir, L"conf")) {
        DeleteFileW(joinPath(vhostDir, f).c_str());
    }
    for (auto& f : listFiles(srcVhost, L"conf")) {
        if (f.rfind(L"_template", 0) == 0) continue;
        if (!copyFileW2(joinPath(srcVhost, f), joinPath(vhostDir, f)))
            logMsg(L"nginx", L"复制站点配置失败: " + f);
    }
    return true;
}

bool genRedisConfig(const std::wstring& ver) {
    std::lock_guard<std::mutex> lk(g_genCfgMtx);
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
    (void)ver;   // template only needs dataDir; keep the signature uniform with genNginxConfig
    std::lock_guard<std::mutex> lk(g_genCfgMtx);
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

// Store the password encrypted and drop any plaintext copy. If DPAPI fails
// (rare), keep the plaintext path so the manager still works.
static void pgStorePassword(const std::wstring& password) {
    std::wstring enc = dpProtect(password);
    if (!enc.empty()) {
        iniSet(L"pg.password.enc", enc);
        iniDelete(L"pg.password");
    } else {
        iniSet(L"pg.password", password);
    }
}

// Password is stored DPAPI-encrypted under pg.password.enc. pg.password is the
// legacy plaintext key: it is still read for installs created before the
// encryption change, but is migrated to the encrypted key on first read — the
// old code only migrated when the user re-initialised the cluster or changed
// the password, so an existing settings.ini kept the secret in cleartext
// forever.
std::wstring pgPassword() {
    std::wstring enc = iniGet(L"pg.password.enc", L"");
    if (!enc.empty()) {
        std::wstring p = dpUnprotect(enc);
        if (!p.empty()) return p;
    }
    std::wstring plain = iniGet(L"pg.password", L"");
    if (!plain.empty()) {
        pgStorePassword(plain);   // writes .enc and deletes the plaintext key
        return plain;
    }
    return L"postgres";
}
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
    if (!nginxRunningVer(st.currentVersion, /*repairPidFile=*/true)) {
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

// Every port a vhost file listens on. An HTTPS site has two server blocks (the
// :80 redirect plus the TLS one), so reading only the first "listen" made every
// HTTPS site look like a plain :80 site: a real 443 collision went unnoticed,
// while two coexisting :80 sites looked like a conflict.
static std::vector<std::wstring> vhostPorts(const std::wstring& content) {
    std::vector<std::wstring> ports;
    size_t pos = 0;
    while ((pos = content.find(L"listen", pos)) != std::wstring::npos) {
        if (pos > 0 && (iswalnum(content[pos - 1]) || content[pos - 1] == L'_')) {
            pos += 6;                                    // part of a longer word
            continue;
        }
        size_t e = content.find(L";", pos);
        if (e == std::wstring::npos) break;
        // "80", "443 ssl", "80 default_server", "[::]:443" -> bare port number
        std::wstringstream ss(trimStr(content.substr(pos + 6, e - pos - 6)));
        std::wstring tok;
        if (ss >> tok) {
            size_t br = tok.rfind(L']');
            if (br != std::wstring::npos) tok = tok.substr(br + 1);
            size_t colon = tok.rfind(L':');
            if (colon != std::wstring::npos) tok = tok.substr(colon + 1);
            tok = trimStr(tok);
            if (!tok.empty() && tok.find_first_not_of(L"0123456789") == std::wstring::npos &&
                std::find(ports.begin(), ports.end(), tok) == ports.end())
                ports.push_back(tok);
        }
        pos = e + 1;
    }
    return ports;
}

std::vector<VHost> nginxListVHosts() {
    std::vector<VHost> result;
    std::wstring dir = nginxVhostSourceDir();
    for (auto& f : listFiles(dir, L"conf")) {
        if (f == L"_template.conf" || f == L"_template_https.conf") continue;
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
        v.ports = vhostPorts(content);
        for (size_t i = 0; i < v.ports.size(); ++i) {
            if (i) v.port += L",";
            v.port += v.ports[i];
        }
        p1 = content.find(L"root");
        if (p1 != std::wstring::npos) {
            // "root" also occurs inside "document_root" / "$root"; only a
            // standalone directive counts.
            if (p1 > 0 && !iswspace(content[p1 - 1])) p1 = content.find(L"root", p1 + 1);
        }
        if (p1 != std::wstring::npos) {
            size_t b = content.find(L" ", p1 + 4);
            size_t e = content.find(L";", p1);
            if (b != std::wstring::npos && e != std::wstring::npos && b < e)
                v.root = trimStr(content.substr(b + 1, e - b - 1));
            if (v.root.size() >= 2 && v.root.front() == L'"' && v.root.back() == L'"')
                v.root = v.root.substr(1, v.root.size() - 2);
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
    // _template* files are reserved for config templates, so a site name must
    // not collide with that convention (previously _foo was written but never
    // copied into the runtime vhost dir, i.e. silently disabled).
    if (name[0] == L'_') {
        err = L"站点名不能以下划线开头";
        return false;
    }
    // validate name (no path chars)
    if (name.find_first_of(L"\\/:. *?\"<>|") != std::wstring::npos) {
        err = L"站点名含非法字符"; return false;
    }
    if (domain.size() > 253) {
        err = L"域名过长";
        return false;
    }
    for (wchar_t c : domain) {
        bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                  (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' ||
                  c == L'_' || c == L'*';
        if (!ok) {
            err = L"域名含非法字符（只允许字母、数字、. - _ *）";
            return false;
        }
    }
    if (ssl) {
        if (certPath.empty() || keyPath.empty()) { err = L"HTTPS 站点需要提供证书和 key 文件"; return false; }
        if (!fileExists(certPath)) { err = L"证书文件不存在: " + certPath; return false; }
        if (!fileExists(keyPath)) { err = L"key 文件不存在: " + keyPath; return false; }
    }
    // Validate the listen port before it lands in the generated config. It used
    // to be written verbatim into "listen <port>;", so a stray character made
    // nginx -t fail *after* the file was already on disk (see the rollback
    // below).
    std::wstring effPort = port.empty() ? nginxPort() : port;
    if (!validPort(effPort)) {
        err = L"端口号无效（应为 1-65535 的数字）: " + port;
        return false;
    }
    // Conflict check. Sharing a listen port is legal in nginx as long as the
    // server_name differs - that is exactly the multi-domain layout the
    // catch-all blocks exist for. Only the same domain on the same port is a
    // real conflict; a bare shared port deserves a note, not a refusal. The old
    // check compared one port per file, so it both missed genuine 443 clashes
    // and rejected two :80 sites that nginx would have served happily.
    ComponentStatus st = compStatus(Comp::Nginx);
    if (st.installed) {
        for (auto& v : nginxListVHosts()) {
            if (v.name == name) continue;
            if (std::find(v.ports.begin(), v.ports.end(), effPort) == v.ports.end()) continue;
            if (lowerStr(v.domain) == lowerStr(domain)) {
                err = L"域名 " + domain + L" 与站点 " + v.name + L" 冲突：两者都监听 " + effPort;
                return false;
            }
            logMsg(L"nginx", L"站点 " + name + L" 与 " + v.name + L" 共用端口 " + effPort +
                             L"（域名不同，nginx 可正常区分）");
        }
    }
    std::wstring vhostDir = nginxVhostSourceDir();
    if (!makeDirs(vhostDir)) { err = L"无法创建 vhosts 目录"; return false; }

    // site root: user-provided path, or the app-managed www\<name>
    bool appManaged = root.empty();
    std::wstring siteRoot = appManaged ? joinPath(wwwDir(), name) : root;
    if (siteRoot.empty()) { err = L"站点根目录为空"; return false; }
    // user-provided root must already exist; the app-managed one is created below
    if (!appManaged && !dirExists(siteRoot)) { err = L"根目录不存在: " + siteRoot; return false; }

    std::map<std::wstring, std::wstring> kv;
    kv[L"PORT"] = effPort;
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

    bool createdSiteRoot = false;
    bool createdIndex = false;
    if (appManaged) {
        // app-managed site: create default page + ssl folder for certificates
        createdSiteRoot = !dirExists(siteRoot);
        makeDirs(siteRoot);
        makeDirs(joinPath(siteRoot, L"ssl"));
        std::wstring idx = joinPath(siteRoot, L"index.html");
        if (!fileExists(idx)) {
            createdIndex = writeFileText(idx, L"<html><head><title>" + name + L"</title></head><body><h1>" + name + L"</h1></body></html>");
        }
    }

    // A failure from here on must not leave the site config behind:
    // genNginxConfig() copies every data\nginx\vhosts\*.conf into the runtime
    // prefix on each start, so one file that fails "nginx -t" would make every
    // later start / reload / version switch fail too, with no hint about which
    // site is broken.
    if (!nginxReload(err)) {
        std::wstring reason = err;
        DeleteFileW(file.c_str());
        // Undo the site directory as well — but only the pieces this call
        // created, and only if they are empty: RemoveDirectoryW refuses a
        // non-empty directory, so a www\<name> that already holds the user's
        // files is never touched.
        if (appManaged) {
            if (createdIndex) DeleteFileW(joinPath(siteRoot, L"index.html").c_str());
            if (createdSiteRoot) {
                RemoveDirectoryW(joinPath(siteRoot, L"ssl").c_str());
                RemoveDirectoryW(siteRoot.c_str());
            }
        }
        ComponentStatus now = compStatus(Comp::Nginx);
        if (now.installed) genNginxConfig(now.currentVersion);   // drop it from the runtime prefix
        err = L"站点配置未通过 nginx 校验，已回滚: " + reason;
        return false;
    }
    return true;
}

bool nginxAddVHost(const std::wstring& name, const std::wstring& domain,
                   const std::wstring& port, std::wstring& err) {
    return nginxAddVHostEx(name, domain, port, false, L"", L"", L"", err);
}

bool nginxRemoveVHost(const std::wstring& name, std::wstring& err) {
    if (name.empty()) { err = L"站点名为空"; return false; }
    // Same name rules as add: reject path characters so a crafted name can
    // never point the delete outside data\nginx\vhosts.
    if (name[0] == L'_' || name.find_first_of(L"\\/:. *?\"<>|") != std::wstring::npos) {
        err = L"站点名含非法字符"; return false;
    }
    std::wstring file = joinPath(nginxVhostSourceDir(), name + L".conf");
    if (!fileExists(file)) { err = L"站点不存在: " + name; return false; }
    if (!DeleteFileW(file.c_str())) { err = L"删除文件失败"; return false; }
    if (!nginxReload(err)) return false;
    return true;
}

// ============================ postgresql ============================

// ---- SQL / command-line safety helpers ----
// Usernames and passwords come straight from the pg page's edit boxes. They
// used to be concatenated into the psql command line verbatim, so a password
// like  a'; DROP DATABASE appdb; --  executed arbitrary SQL. Two measures:
//
//   1. Identifiers and the port are whitelisted. They also land on the psql
//      command line (-U, -p), where a space or quote would either split into
//      extra arguments or corrupt the quoting.
//   2. The statement itself is written to a temporary .sql file and run with
//      -f. That removes the command line from the picture entirely: the only
//      escaping left is PostgreSQL's own literal/identifier rules, which are
//      simple and handled here.

static bool pgValidIdent(const std::wstring& s) {
    if (s.empty() || s.size() > 63) return false;
    auto isStart = [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_';
    };
    auto isCont = [&](wchar_t c) {
        return isStart(c) || (c >= L'0' && c <= L'9') || c == L'$';
    };
    if (!isStart(s[0])) return false;
    for (size_t i = 1; i < s.size(); ++i) {
        if (!isCont(s[i])) return false;
    }
    return true;
}

static bool validPort(const std::wstring& s) {
    if (s.empty() || s.size() > 5) return false;
    long v = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
        v = v * 10 + (c - L'0');
    }
    return v > 0 && v <= 65535;
}

// Wrap as a PostgreSQL string literal. While standard_conforming_strings is
// on (the default since 9.1, and what initdb sets here) doubling the single
// quote is the only transformation needed; backslashes are ordinary chars.
static std::wstring pgEscapeLiteral(const std::wstring& s) {
    std::wstring out = L"'";
    for (wchar_t c : s) {
        if (c == L'\'') out += L"''";
        else out += c;
    }
    out += L'\'';
    return out;
}

// Bare when already a plain identifier, otherwise double-quoted with
// embedded quotes doubled. This keeps non-ASCII names (e.g. 张三) usable.
static std::wstring pgEscapeIdent(const std::wstring& s) {
    if (pgValidIdent(s)) return s;
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'"') out += L"\"\"";
        else out += c;
    }
    out += L'"';
    return out;
}

// Ask Windows for a brand-new empty temp file instead of deriving one from
// seconds + PID. The old scheme let two concurrent PG operations in the same
// second (e.g. the user-list refresh plus a password change) collide on the
// same .sql/.tmp path.
static bool makeTempFile(const std::wstring& prefix, std::wstring& path) {
    wchar_t tmpDir[MAX_PATH], name[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmpDir)) return false;
    if (!GetTempFileNameW(tmpDir, prefix.c_str(), 0, name)) return false;
    path = name;
    return true;
}

// Run `sql` against the running server through a temp file. extraArgs are
// inserted verbatim before -f (caller-owned literals such as "-t -A").
// On failure returns false with out.output holding the reason.
static bool pgRunSql(Comp c, const std::wstring& ver, const std::wstring& extraArgs,
                     const std::wstring& sql, RunResult& out) {
    out = RunResult();
    std::wstring ver2 = ver.empty() ? iniGet(L"ver.postgresql", L"") : ver;
    if (ver2.empty()) { out.output = L"未选择 PostgreSQL 版本"; return false; }

    std::wstring port = pgPort();
    std::wstring user = pgUser();
    if (!validPort(port)) { out.output = L"端口号无效: " + port; return false; }
    if (!pgValidIdent(user)) {
        out.output = L"用户名含非法字符（只允许字母、数字、下划线）: " + user;
        return false;
    }
    std::wstring psql;
    if (!findExe(Comp::Postgresql, ver2, L"psql.exe", psql)) {
        out.output = L"未找到 psql.exe";
        return false;
    }

    std::wstring sqlFile;
    if (!makeTempFile(L"lnp", sqlFile)) {
        out.output = L"无法创建临时 SQL 文件";
        return false;
    }
    // Pin the encoding: writeFileText emits UTF-8, but psql would otherwise
    // decode the file with the OS ANSI code page and mangle non-ASCII names.
    std::wstring content = L"\\encoding UTF8\r\n" + sql + L"\r\n";
    if (!writeFileText(sqlFile, content)) {
        out.output = L"无法写入临时 SQL 文件: " + sqlFile;
        DeleteFileW(sqlFile.c_str());
        return false;
    }

    std::wstring cmd = L"-h 127.0.0.1 -p " + port + L" -U " + user +
                       L" -d postgres -v ON_ERROR_STOP=1";
    if (!extraArgs.empty()) cmd += L" " + extraArgs;
    cmd += L" -f \"" + sqlFile + L"\"";

    RunResult r = runProcessCapture(psql, cmd, compBinDirVer(c, ver2),
                                    15000, {{L"PGPASSWORD", pgPassword()}});
    DeleteFileW(sqlFile.c_str());
    if (!r.ok) {
        out.output = r.output.empty() ? L"SQL 执行失败" : r.output;
        return false;
    }
    out = r;
    return true;
}

// ============================ Restore ============================

std::vector<std::wstring> pgListBackups() {
    std::vector<std::wstring> files = listFiles(backupDir(), L"sql");
    std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
        // file mtime, not name: the keep-newest-N pruning already bounds this
        // list, and a name sort would mix versions ("postgresql-18-..." sorts
        // above every "postgresql-17-..." regardless of date).
        HANDLE ha = CreateFileW(joinPath(backupDir(), a).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE hb = CreateFileW(joinPath(backupDir(), b).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        FILETIME fa = {}, fb = {};
        if (ha) GetFileTime(ha, nullptr, nullptr, &fa);
        if (hb) GetFileTime(hb, nullptr, nullptr, &fb);
        if (ha) CloseHandle(ha);
        if (hb) CloseHandle(hb);
        if (fa.dwHighDateTime != fb.dwHighDateTime) return fa.dwHighDateTime > fb.dwHighDateTime;
        if (fa.dwLowDateTime  != fb.dwLowDateTime)  return fa.dwLowDateTime  > fb.dwLowDateTime;
        return a > b;
    });
    return files;
}

bool pgRestoreOutputOk(const std::wstring& output, std::wstring& firstRealError) {
    firstRealError.clear();
    std::vector<std::wstring> lines;
    std::wstringstream ss(output);
    std::wstring line;
    while (std::getline(ss, line)) lines.push_back(trimStr(line));

    // A pg_dumpall dump re-creates its roles and databases with plain CREATE,
    // so a replay always collides with whatever already exists. The bootstrap
    // superuser is named in the dump and exists in EVERY cluster, even one
    // initdb just created — so the collision is expected, not a failed restore.
    //
    // Judge it on the SQLSTATE (duplicate_object = 42710) plus the offending
    // statement keyword. Both are locale-independent, so this stays correct even
    // if someone sets lc_messages to a non-English language — matching on the
    // words "role"/"database" would silently pass everything through then.
    auto createsRoleOrDb = [](const std::wstring& line) {
        std::wstring u = lowerStr(line);
        size_t pos = u.find(L"line ");
        if (pos == std::wstring::npos) return false;
        return u.find(L"create role") != std::wstring::npos ||
               u.find(L"create database") != std::wstring::npos;
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind(L"ERROR:", 0) != 0) continue;
        bool tolerated = false;
        // Primary test: the state code plus the statement that produced it. Both
        // are locale-independent.
        if (lines[i].find(L"42710:") != std::wstring::npos) {
            for (size_t j = i; j < lines.size() && j <= i + 3; ++j) {
                if (createsRoleOrDb(lines[j])) { tolerated = true; break; }
            }
        }
        // Fallback for a server that does not echo the state code. A duplicate
        // *table* must not pass here: it means the dump is being merged into a
        // cluster that still holds that data.
        if (!tolerated) {
            std::wstring low = lowerStr(lines[i]);
            if (low.find(L"already exists") != std::wstring::npos &&
                (low.find(L"role") != std::wstring::npos ||
                 low.find(L"database") != std::wstring::npos))
                tolerated = true;
        }
        if (!tolerated && firstRealError.empty()) firstRealError = lines[i];
    }
    return firstRealError.empty();
}

// Put the pre-restore cluster back. The half-restored directory is moved aside
// rather than deleted, so a failed attempt costs disk space, not data.
static void rollbackRestore(const std::wstring& ver, const std::wstring& aside, std::wstring& err) {
    if (aside.empty()) return;   // nothing was moved aside -> nothing to undo
    std::wstring dataDir = pgDataDir(Comp::Postgresql, ver);
    std::wstring e;
    if (pgRunningVer(Comp::Postgresql, ver)) compStop(Comp::Postgresql, e);
    if (dirExists(dataDir)) {
        std::wstring failed = dataDir + L".failed-" + nowStamp();
        MoveFileW(dataDir.c_str(), failed.c_str());
    }
    if (dirExists(aside) && MoveFileW(aside.c_str(), dataDir.c_str())) {
        compStart(Comp::Postgresql, e);
        err += L"；已回滚到还原前的数据目录并重新启动";
    } else {
        err += L"；回滚失败，原数据目录仍在 " + aside;
    }
}

bool pgRestoreBackup(Comp c, const std::wstring& backupFile, bool reinit, std::wstring& err) {
    std::unique_lock<std::recursive_mutex> lk(g_compOpMtx[(int)c], std::try_to_lock);
    if (!lk.owns_lock()) { err = kCompBusyMsg; return false; }

    if (backupFile.empty() || !fileExists(backupFile)) { err = L"备份文件不存在"; return false; }
    std::wstring ver = iniGet(L"ver.postgresql", L"");
    if (ver.empty()) { err = L"未选择 PostgreSQL 版本"; return false; }
    if (!validPort(pgPort())) { err = L"端口号无效: " + pgPort(); return false; }
    if (!pgValidIdent(pgUser())) { err = L"用户名含非法字符: " + pgUser(); return false; }

    std::wstring dataDir = pgDataDir(c, ver);
    std::wstring aside;                       // previous cluster, preserved
    if (reinit) {
        if (pgRunningVer(c, ver) && !compStop(c, err)) { err = L"停止数据库失败: " + err; return false; }
        if (pgDataInitialized(ver)) {
            aside = dataDir + L".before-restore-" + nowStamp();
            if (!MoveFileW(dataDir.c_str(), aside.c_str())) {
                err = L"无法移走当前数据目录（可能仍被占用）: " + dataDir;
                return false;
            }
            logMsg(L"pg", L"还原前已移走原数据目录: " + aside);
        }
        // Why a rebuild is required rather than "just drop the databases": a
        // pg_dumpall dump re-creates its roles with plain CREATE ROLE, and the
        // bootstrap superuser is named in it, so replaying into a live cluster
        // collides no matter how many databases were dropped.
        if (!pgInit(c, ver, pgUser(), pgPassword(), pgPort(), err)) {
            rollbackRestore(ver, aside, err);
            return false;
        }
    } else if (!pgRunningVer(c, ver)) {
        err = L"PostgreSQL 未运行。请先启动，或勾选「还原前初始化」";
        return false;
    }

    // Only start what is not up yet. In the non-reinit path the server is by
    // definition already running, and compStart() answers 已在运行 and returns
    // false — calling it unconditionally made every plain restore fail with a
    // message that says nothing about the restore.
    if (!pgRunningVer(c, ver)) {
        if (!compStart(c, err)) { rollbackRestore(ver, aside, err); return false; }
        for (int i = 0; i < 60 && !pgRunningVer(c, ver); ++i) Sleep(200);
        if (!pgRunningVer(c, ver)) {
            err = L"数据库启动后未就绪: " + ver;
            rollbackRestore(ver, aside, err);
            return false;
        }
    }

    std::wstring psql;
    if (!findExe(c, ver, L"psql.exe", psql)) {
        err = L"未找到 psql.exe";
        rollbackRestore(ver, aside, err);
        return false;
    }
    // VERBOSITY=verbose makes every error carry its SQLSTATE and the failing
    // statement — that is what pgRestoreOutputOk() judges on, and it stays correct
    // whatever lc_messages is set to. ON_ERROR_STOP is deliberately absent: the
    // dump trips over the bootstrap superuser, and aborting on the first
    // statement would restore nothing.
    std::wstring cmd = L"-h 127.0.0.1 -p " + pgPort() + L" -U " + pgUser() +
                       L" -d postgres -v VERBOSITY=verbose -f \"" + backupFile + L"\"";
    RunResult r = runProcessCapture(psql, cmd, compBinDirVer(c, ver), 1800000,
                                    {{L"PGPASSWORD", pgPassword()}});
    std::wstring realErr;
    if (!pgRestoreOutputOk(r.output, realErr)) {
        err = realErr.empty()
                  ? (r.output.empty() ? L"psql 执行失败" : r.output)
                  : (L"还原过程中出错: " + realErr);
        rollbackRestore(ver, aside, err);
        return false;
    }
    logMsg(L"pg", L"还原完成: " + backupFile);
    err = L"还原完成";
    if (!aside.empty())
        err += L"；还原前的数据目录保留在 " + aside + L"，确认数据无误后可删除";
    return true;
}

bool pgDataInitialized(const std::wstring& ver) {
    return fileExists(joinPath(pgDataDir(Comp::Postgresql, ver), L"PG_VERSION"));
}

bool pgInit(Comp c, const std::wstring& ver, const std::wstring& user,
            const std::wstring& password, const std::wstring& port, std::wstring& err) {
    // Validate before touching the disk: both values end up on the initdb
    // command line, so anything but a plain identifier / plain number would
    // either be rejected by initdb or split into bogus arguments.
    if (!pgValidIdent(user)) {
        err = L"用户名只能包含字母、数字、下划线、$，且以字母或下划线开头";
        return false;
    }
    std::wstring effPort = port.empty() ? L"5432" : port;
    if (!validPort(effPort)) { err = L"端口号无效（应为 1-65535）: " + port; return false; }
    // An empty password would create a passwordless superuser even under
    // --auth scram-sha-256, so refuse instead of quietly building one.
    if (password.empty()) { err = L"密码不能为空"; return false; }
    // initdb --pwfile consumes the *first line* only: a password containing a
    // line break would be truncated on the server while we store the full
    // string, and every later psql connection would fail authentication.
    if (password.find_first_of(L"\r\n") != std::wstring::npos) {
        err = L"密码不能包含换行符";
        return false;
    }

    std::wstring dataDir = pgDataDir(c, ver);
    if (pgDataInitialized(ver)) { err = L"数据库已初始化"; return false; }
    if (!makeDirs(dataDir)) { err = L"无法创建数据目录"; return false; }

    std::wstring initdb;
    if (!findExe(Comp::Postgresql, ver, L"initdb.exe", initdb)) {
        err = L"未找到 initdb.exe"; return false;
    }

    // write temp pw file to system temp (outside data dir so initdb sees empty dir).
    // Random-ish name: a fixed name like lnpp_pwfile.tmp could collide with a
    // stale/foreign file of the same name and feed initdb the wrong password.
    std::wstring pwFile;
    if (!makeTempFile(L"lnp", pwFile)) {
        err = L"无法创建临时密码文件";
        return false;
    }
    if (!writeFileText(pwFile, password)) {
        err = L"无法写入临时密码文件: " + pwFile;
        DeleteFileW(pwFile.c_str());
        return false;
    }

    std::wstring args = L"-D \"" + dataDir + L"\" -U " + user +
                        L" -E UTF8 --locale=C -A scram-sha-256 --pwfile=\"" + pwFile + L"\"";

    RunResult r = runProcessCapture(initdb, args, compBinDirVer(c, ver), 120000);
    DeleteFileW(pwFile.c_str());
    if (!r.ok) {
        err = L"initdb 失败: " + r.output;
        return false;
    }

    // persist settings (password stored DPAPI-encrypted)
    iniSet(L"pg.user", user);
    pgStorePassword(password);
    iniSet(L"pg.port", effPort);

    if (!genPgConfig(ver, dataDir)) { err = L"生成配置失败"; return false; }
    return true;
}

bool pgChangePassword(Comp c, const std::wstring& user, const std::wstring& password, std::wstring& err) {
    if (user.empty()) { err = L"用户名为空"; return false; }
    std::wstring sql = L"ALTER USER " + pgEscapeIdent(user) +
                       L" WITH PASSWORD " + pgEscapeLiteral(password) + L";";
    RunResult r;
    if (!pgRunSql(c, iniGet(L"ver.postgresql", L""), L"", sql, r)) {
        err = r.output.empty() ? L"修改密码失败" : r.output;
        return false;
    }
    if (lowerStr(user) == lowerStr(pgUser())) pgStorePassword(password);
    return true;
}

bool pgCreateUser(Comp c, const std::wstring& user, const std::wstring& password, std::wstring& err) {
    if (user.empty()) { err = L"用户名为空"; return false; }
    std::wstring sql = L"CREATE USER " + pgEscapeIdent(user) +
                       L" WITH PASSWORD " + pgEscapeLiteral(password) + L";";
    RunResult r;
    if (!pgRunSql(c, iniGet(L"ver.postgresql", L""), L"", sql, r)) {
        err = r.output.empty() ? L"创建用户失败" : r.output;
        return false;
    }
    return true;
}

bool pgDropUser(Comp c, const std::wstring& user, std::wstring& err) {
    if (user.empty()) { err = L"用户名为空"; return false; }
    std::wstring sql = L"DROP USER IF EXISTS " + pgEscapeIdent(user) + L";";
    RunResult r;
    if (!pgRunSql(c, iniGet(L"ver.postgresql", L""), L"", sql, r)) {
        err = r.output.empty() ? L"删除用户失败" : r.output;
        return false;
    }
    return true;
}

bool pgListUsers(std::vector<std::wstring>& users, std::wstring& err) {
    users.clear();
    std::wstring ver = iniGet(L"ver.postgresql", L"");
    if (ver.empty()) { err = L"未选择 PostgreSQL 版本"; return false; }
    if (!pgRunningVer(Comp::Postgresql, ver)) { err = L"PostgreSQL 未运行"; return false; }
    RunResult r;
    if (!pgRunSql(Comp::Postgresql, ver, L"-t -A",
                  L"SELECT rolname FROM pg_roles WHERE rolname NOT LIKE 'pg\\_%' ORDER BY 1", r)) {
        err = r.output;
        return false;
    }
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
    // port/user come from settings.ini and land on the command line
    std::wstring port = pgPort();
    std::wstring user = pgUser();
    if (!validPort(port)) { err = L"端口号无效: " + port; return false; }
    if (!pgValidIdent(user)) { err = L"用户名含非法字符: " + user; return false; }
    std::wstring cmd = L"-h 127.0.0.1 -p " + port + L" -U " + user +
                       L" -f \"" + backupFile + L"\"";
    RunResult r = runProcessCapture(pgDumpall, cmd, compBinDirVer(c, ver), 120000,
                                    {{L"PGPASSWORD", pgPassword()}});
    if (!r.ok) {
        err = L"pg_dumpall 失败: " + r.output;
        return false;
    }
    return true;
}

// ============================ nodejs / pm2 ============================

// Parse `pm2 jlist` output (array of objects) into PM2App records. Pure text
// parsing (no process / IO) — kept non-static so unit tests can cover it.
std::vector<PM2App> parsePm2List(const std::wstring& json) {
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
        // real OS pid of the forked app. pm2 reports "online" from its own
        // bookkeeping; when the child died without pm2 noticing (OOM kill, hard
        // crash) this pid is the only way to tell the row is stale.
        k = obj.find(L"\"pid\"");
        if (k != std::wstring::npos) {
            size_t colon = obj.find(L':', k);
            size_t s = obj.find_first_of(L"0123456789", colon);
            size_t e2 = s;
            while (e2 < obj.size() && iswdigit(obj[e2])) e2++;
            app.pid = (s != std::wstring::npos) ? _wtoi(obj.substr(s, e2 - s).c_str()) : 0;
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
        // restarts (the first find is the only one needed: same search)
        k = obj.find(L"\"restart_time\"");        if (k != std::wstring::npos) {
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
    std::vector<PM2App> apps = parsePm2List(r.output);
    // pm2 keeps reporting "online" for a child that died without it noticing
    // (OOM kill, hard crash). The UI used to show those rows as running with
    // 0 memory, which is exactly how a dead app looks healthy. Mark any row
    // whose pid is not a live node process so the list says what is really there.
    for (auto& a : apps) {
        if (a.status != L"online") continue;
        a.stale = !(a.pid != 0 && isPidAlive(a.pid) && processImageNameIs(a.pid, L"node.exe"));
    }
    return apps;
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
    // point at a removed system install -> apps start but never listen).
    // resurrect spawns the daemon, so wait for it to publish pm2.pid first:
    // listing too early returned an empty app list and silently skipped every
    // interpreter fix-up.
    std::wstring ver = iniGet(L"ver.nodejs", L"");
    std::wstring node = joinPath(compBinDirVer(Comp::Nodejs, ver), L"node.exe");
    if (!node.empty() && fileExists(node)) {
        if (!waitForPm2Daemon(5000)) {
            logMsg(L"pm2", L"resurrect 后守护进程未就绪，跳过 interpreter 修正");
            return true;   // apps may still come up; do not fail the whole op
        }
        std::wstring ln = lowerStr(node);
        for (auto& app : nodePm2List()) {
            if (!app.interpreter.empty() && lowerStr(app.interpreter) != ln) {
                runPm2({L"restart", std::to_wstring(app.id), L"--interpreter", node}, 20000);
            }
        }
    }
    return true;
}
