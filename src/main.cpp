#include "common.h"
#include "proc.h"
#include "manager.h"
#include "downloader.h"
#include <commdlg.h>
#include <shlobj.h>
#include <memory>
#include <cstdlib>

// App icon resource (see app.rc)
#define IDI_APP 101

// ============================ Control IDs ============================
enum {
    IDC_TAB = 100,
    // common per-page controls
    IDC_DOT = 200, IDC_STATUS_TXT = 201, IDC_VER_COMBO = 202,
    IDC_BTN_SWITCH = 203, IDC_BTN_START = 204, IDC_BTN_STOP = 205,
    IDC_BTN_CFG = 206, IDC_BTN_DATA = 207, IDC_LOG = 208, IDC_BTN_CLEAR_LOG = 209,
    // nginx page
    IDC_NG_VHOST_LIST = 300, IDC_NG_ADD_NAME = 301, IDC_NG_ADD_DOMAIN = 302,
    IDC_NG_ADD_PORT = 303, IDC_NG_BTN_ADD = 304, IDC_NG_BTN_DEL = 305, IDC_NG_BTN_RELOAD = 306,
    IDC_NG_LABEL_NAME = 307, IDC_NG_LABEL_DOMAIN = 308, IDC_NG_LABEL_PORT = 309,
    IDC_NG_SSL = 310, IDC_NG_CERT = 311, IDC_NG_KEY = 312,
    IDC_NG_BTN_CERT = 313, IDC_NG_BTN_KEY = 314,
    IDC_NG_LABEL_CERT = 315, IDC_NG_LABEL_KEY = 316,
    IDC_NG_ROOT = 317, IDC_NG_BTN_ROOT = 318, IDC_NG_LABEL_ROOT = 319,
    // site kind (node / php) + the php version a php site is pinned to
    IDC_NG_KIND = 320, IDC_NG_LABEL_KIND = 321,
    IDC_NG_PHPVER = 322, IDC_NG_LABEL_PHPVER = 323,
    // postgresql page
    // Every id here must be unique across the whole main window: WM_COMMAND
    // dispatches on LOWORD(wParam) alone, ignoring lParam, so a duplicate turns
    // one control's notification into another control's action. IDC_PG_BTN_RESTORE
    // used to be 406 and collided with IDC_PG_USER below — picking a database
    // user in the combo fired CBN_SELCHANGE and opened the restore dialog.
    IDC_PG_BTN_INIT = 400, IDC_PG_BTN_PWD = 401, IDC_PG_BTN_ADDUSER = 402,
    IDC_PG_BTN_DELUSER = 403, IDC_PG_BTN_BACKUP = 404, IDC_PG_INFO = 405,
    IDC_PG_USER = 406, IDC_PG_PWD = 407, IDC_PG_PORT = 408,
    IDC_PG_LABEL_USER = 409, IDC_PG_LABEL_PWD = 410, IDC_PG_LABEL_PORT = 411,
    IDC_PG_BTN_RESTORE = 412,
    // redis page
    IDC_REDIS_INFO = 500,
    // nodejs page
    IDC_NODE_PM2_LIST = 600, IDC_NODE_BTN_RESTART = 601, IDC_NODE_BTN_STOP = 602, IDC_NODE_BTN_REFRESH = 603,
    IDC_NODE_BTN_RESTART_ALL = 604, IDC_NODE_BTN_DELETE = 605,
    // overview page (first tab)
    IDC_OV_AUTOSTART_ALL = 700, IDC_OV_BTN_ALL_START = 701, IDC_OV_BTN_ALL_RESTART = 702,
    IDC_OV_BTN_ALL_STOP = 703, IDC_OV_RESULT = 704, IDC_OV_BOOT_START = 705,
    IDC_OV_BTN_PATH_ADD = 706, IDC_OV_BTN_PATH_DEL = 707,
    IDC_OV_BTN_DL = 708, IDC_OV_VER_TXT = 709, IDC_OV_ABOUT_LINK = 714,
    IDC_OV_LOG = 715, IDC_OV_BTN_CLEAR_LOG = 716,
    IDC_OV_COMP_START_BASE = 710,   // + comp index: start/stop toggle button
    IDC_OV_COMP_AUTO_BASE = 720,    // + comp index: "随管理器启动" checkbox
    IDC_OV_COMP_STATUS_BASE = 730,  // + comp index: status text
    // php page
    IDC_PHP_POOL_LIST = 800, IDC_PHP_BTN_START = 801, IDC_PHP_BTN_STOP = 802,
    IDC_PHP_BTN_DEFAULT = 803, IDC_PHP_INFO = 804,
    IDC_PHP_WORKERS = 805, IDC_PHP_LABEL_WORKERS = 806, IDC_PHP_BTN_EXT = 807,
    // extension manager dialog
    IDC_PHPX_LIST = 900, IDC_PHPX_APPLY = 901, IDC_PHPX_CLOSE = 902,
    IDC_PHPX_INFO = 903,
};

// WM_COMMAND dispatches on the id alone, so a duplicate silently turns one
// control's notification into another control's action. Catch it at compile time
// rather than in a bug report.
constexpr bool idsDistinct(const int* ids, int n) {
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (ids[i] == ids[j]) return false;
    return true;
}
constexpr int kPgPageIds[] = {
    IDC_PG_BTN_INIT, IDC_PG_BTN_PWD, IDC_PG_BTN_ADDUSER, IDC_PG_BTN_DELUSER,
    IDC_PG_BTN_BACKUP, IDC_PG_BTN_RESTORE, IDC_PG_INFO,
    IDC_PG_USER, IDC_PG_PWD, IDC_PG_PORT,
    IDC_PG_LABEL_USER, IDC_PG_LABEL_PWD, IDC_PG_LABEL_PORT,
};
static_assert(idsDistinct(kPgPageIds, sizeof(kPgPageIds) / sizeof(kPgPageIds[0])),
              "duplicate control id on the postgresql page - see the note above");

// tray menu item ids
enum {
    IDM_TRAY_SHOW = 2000,
    IDM_TRAY_COMP_BASE = 2100,   // + comp index: start/stop
    IDM_TRAY_ALL_START = 2200,
    IDM_TRAY_ALL_STOP = 2201,
    IDM_TRAY_EXIT = 2300,
};

// ============================ Message IDs ============================
enum {
    WM_OP_DONE = WM_APP + 1,      // wParam = comp index, lParam = 0 success / 1 fail
    WM_UI_STATUS = WM_APP + 2,    // background status poll finished -> refresh dots/info
    WM_UI_PM2 = WM_APP + 3,       // background pm2 poll finished -> refresh node page
    WM_ALL_DONE = WM_APP + 4,     // overview all-start/restart/stop finished (wParam=AllOp, lParam=fail count)
    WM_TRAYICON = WM_APP + 5,     // tray icon callback
    WM_REAL_EXIT = WM_APP + 6,    // all components stopped -> destroy window / quit
    WM_PG_USERS = WM_APP + 7,     // pg user list loaded -> fill the user combo
    WM_DL_STAGE = WM_APP + 8,     // downloader: stage changed (lParam = heap wchar_t*)
    WM_DL_PROGRESS = WM_APP + 9,  // downloader: bytes (wParam=done, lParam=total)
    WM_DL_DONE = WM_APP + 10,     // downloader: finished (wParam=ok, lParam=heap wchar_t* err)
    WM_UI_LOG = WM_APP + 11,      // worker thread -> UI: append log line (wParam=edit, lParam=heap wchar_t*)
};

// ============================ Globals ============================
// Assigned in WM_CREATE, NOT after CreateWindowExW returns: WM_CREATE is
// dispatched from inside CreateWindowExW, and autoStartComponents() runs there.
// Before this was set there, every worker it spawned called
// PostMessageW(nullptr, WM_OP_DONE, ...) as it finished, which posts to the
// CALLING THREAD's queue instead of the window's — and the main loop's
// DispatchMessageW then silently drops a message with a null hwnd. The result
// was g_ui[i].busy stuck true forever: the fast components (nginx / pg / redis)
// could never be stopped or started again from the overview, while Node.js —
// slow enough to finish after CreateWindowExW had returned — worked fine.
static HWND g_main = nullptr;
static HWND g_tab = nullptr;
static HFONT g_font = nullptr;
static HFONT g_monoFont = nullptr;
static HFONT g_linkFont = nullptr;
static HWND g_tip = nullptr;   // shared tooltip, created once at WM_CREATE
static std::wstring appVersion();   // defined below

// Background status / pm2 polling snapshot (see statusWorker / pm2Worker).
static std::mutex g_snapMtx;
static bool g_snapRunning[(int)Comp::Count] = {};
static bool g_snapInstalled[(int)Comp::Count] = {};
static std::wstring g_snapCurrent[(int)Comp::Count];
static std::vector<std::wstring> g_snapVersions[(int)Comp::Count];
static std::vector<PM2App> g_snapPm2;
static std::atomic<bool> g_statusWorkerBusy{false};
static std::atomic<bool> g_pm2WorkerBusy{false};
static std::atomic<bool> g_appClosing{false};
// Set by the first shutdown path that starts stopping components, so WM_DESTROY
// does not run the (slow, redundant) stop sequence a second time.
static std::atomic<bool> g_shutdownStarted{false};
static std::atomic<int> g_curTab{0};
static bool g_pendingVhostClear = false;   // clear nginx add form after a successful add
static std::vector<std::wstring> g_pgUsers;   // pg user list for the user combo
static std::atomic<bool> g_pgUsersBusy{false};

// ---- system tray / boot start ----
static bool g_trayAdded = false;
static bool g_startHidden = false;
static NOTIFYICONDATAW g_nid = {};

struct CompUI {
    HWND dot = nullptr, statusTxt = nullptr, verCombo = nullptr;
    HWND btnSwitch = nullptr, btnStart = nullptr, btnStop = nullptr, btnCfg = nullptr, btnData = nullptr;
    HWND logEdit = nullptr;
    std::vector<HWND> pageControls;   // controls belonging to this tab page
    std::atomic<bool> busy{false};
    std::wstring opName;
};
static CompUI g_ui[(int)Comp::Count];

// ---- Overview (first tab) ----
// Tab layout: [0] 总览, [1..] nginx / PostgreSQL / Redis / Node.js
static const int TAB_OVERVIEW = 0;
static const int TAB_COMP_BASE = 1;
static Comp tabToComp(int idx) { return (Comp)(idx - TAB_COMP_BASE); }
static int compToTab(Comp c) { return (int)c + TAB_COMP_BASE; }

struct OverviewUI {
    HWND chkAutoAll = nullptr;
    HWND btnAllStart = nullptr, btnAllRestart = nullptr, btnAllStop = nullptr;
    HWND btnPathAdd = nullptr, btnPathDel = nullptr;
    HWND dot[(int)Comp::Count];
    HWND status[(int)Comp::Count];
    HWND btnToggle[(int)Comp::Count];
    HWND chkAuto[(int)Comp::Count];
    HWND result = nullptr;
    HWND logEdit = nullptr;
    HWND aboutLink = nullptr;
    HWND verTxt = nullptr;
    std::vector<HWND> controls;
    std::atomic<bool> busy{false};
};
static OverviewUI g_ov;

// ============================ Utilities ============================

// write an already-formatted line (timestamp included) to an edit control.
// UI thread only — must not be called from worker threads.
static void logAppendRaw(HWND edit, const std::wstring& text) {
    int len = GetWindowTextLengthW(edit);
    // Cap the buffer: a tray-resident manager can run for weeks and every
    // operation appends, so without this the edit control grows without bound
    // for the whole life of the process.
    const int kMaxChars = 256 * 1024;
    if (len > kMaxChars) {
        // EM_SETSEL(start, end) with start > end selects nothing, so drop the
        // oldest half by re-selecting from the trim point to the end.
        int cut = len - kMaxChars / 2;
        SendMessageW(edit, EM_SETSEL, cut, len);
        SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(edit);
    }
    SendMessageW(edit, EM_SETSEL, len, len);
    SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
    // auto scroll
    SendMessageW(edit, EM_SCROLLCARET, 0, 0);
}

static void logAppendTo(HWND edit, const std::wstring& line) {
    if (!edit) return;
    std::wstring text = nowText() + L"  " + line + L"\r\n";
    // Worker threads (runAsync / ovAllOp) must not touch the edit control
    // directly: a cross-thread SendMessage can deadlock during shutdown or
    // race a destroyed window. Hand the formatted line to the UI thread via
    // WM_UI_LOG; the UI thread frees the copy.
    if (!isUiThread()) {
        wchar_t* copy = _wcsdup(text.c_str());
        if (!PostMessageW(g_main, WM_UI_LOG, (WPARAM)edit, (LPARAM)copy))
            free(copy);   // queue full or window gone: drop the line
        return;
    }
    logAppendRaw(edit, text);
}

static void logAppend(Comp c, const std::wstring& line) {
    logAppendTo(g_ui[(int)c].logEdit, line);
}

static void ovLogAppend(const std::wstring& line) {
    logAppendTo(g_ov.logEdit, line);
}

static void logMsgUi(Comp c, const std::wstring& msg) {
    logAppend(c, msg);
}

static HWND makeCtl(int id, const wchar_t* cls, const wchar_t* text, DWORD style,
                    int x, int y, int w, int cth, HWND parent) {
    // every control built here is a child of `parent`; without WS_CHILD the
    // system would create a draggable top-level popup instead
    style |= WS_CHILD | WS_VISIBLE;
    HWND ctl = CreateWindowW(cls, text, style, x, y, w, cth, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    if (ctl) SendMessageW(ctl, WM_SETFONT, (WPARAM)g_font, TRUE);
    return ctl;
}

static void setStatus(Comp c, bool running, bool installed) {
    CompUI& ui = g_ui[(int)c];
    if (!ui.statusTxt) return;
    if (!installed) {
        SetWindowTextW(ui.statusTxt, L"未安装");
        SetWindowLongPtrW(ui.dot, GWLP_USERDATA, 0);
        InvalidateRect(ui.dot, nullptr, TRUE);
    } else if (running) {
        SetWindowTextW(ui.statusTxt, L"运行中");
        SetWindowLongPtrW(ui.dot, GWLP_USERDATA, 2);
        InvalidateRect(ui.dot, nullptr, TRUE);
    } else {
        SetWindowTextW(ui.statusTxt, L"已停止");
        SetWindowLongPtrW(ui.dot, GWLP_USERDATA, 1);
        InvalidateRect(ui.dot, nullptr, TRUE);
    }
}

static void setCombo(HWND combo, const std::vector<std::wstring>& items, const std::wstring& sel) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int selIdx = -1;
    for (size_t i = 0; i < items.size(); ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)items[i].c_str());
        if (items[i] == sel) selIdx = (int)i;
    }
    SendMessageW(combo, CB_SETCURSEL, selIdx < 0 ? 0 : selIdx, 0);
}

static void refreshVersions(Comp c) {
    CompUI& ui = g_ui[(int)c];
    if (!ui.verCombo) return;
    std::vector<std::wstring> versions;
    std::wstring current;
    bool running = false, installed = false;
    {
        std::lock_guard<std::mutex> lk(g_snapMtx);
        versions = g_snapVersions[(int)c];
        current = g_snapCurrent[(int)c];
        running = g_snapRunning[(int)c];
        installed = g_snapInstalled[(int)c];
    }
    setCombo(ui.verCombo, versions, current);
    setStatus(c, running, installed);
    EnableWindow(ui.verCombo, installed);
    EnableWindow(ui.btnSwitch, installed);
    EnableWindow(ui.btnStart, installed);
    EnableWindow(ui.btnStop, installed);
}

// ============================ Worker helpers ============================

static void beginOp(Comp c, const std::wstring& name) {
    CompUI& ui = g_ui[(int)c];
    ui.busy = true;
    ui.opName = name;
    logMsgUi(c, L"==> " + name + L" ...");
    EnableWindow(ui.btnStart, FALSE);
    EnableWindow(ui.btnStop, FALSE);
    EnableWindow(ui.btnSwitch, FALSE);
    EnableWindow(ui.verCombo, FALSE);
}

static void endOp(Comp c, bool ok, const std::wstring& msg) {
    CompUI& ui = g_ui[(int)c];
    // ui.opName is a plain std::wstring written by beginOp on the UI thread.
    // Take the snapshot BEFORE clearing busy: once busy is false the UI thread
    // may pass runAsync's guard and start the next op, which overwrites opName
    // while this worker is still formatting the log line.
    std::wstring name = ui.opName;
    logMsgUi(c, ok ? (L"==> " + name + L" 完成") : (L"==> " + name + L" 失败: " + msg));
    // busy is cleared by the UI thread in WM_OP_DONE — it owns ui.opName.
    PostMessageW(g_main, WM_OP_DONE, (WPARAM)c, ok ? 0 : 1);
}

static void runAsync(Comp c, const std::wstring& name,
                     std::function<bool(std::wstring&)> fn) {
    CompUI& ui = g_ui[(int)c];
    if (ui.busy) return;
    beginOp(c, name);
    std::thread([c, fn]() {
        std::wstring err;
        bool ok = fn(err);
        endOp(c, ok, err);
    }).detach();
}

static void ovRunAsync(Comp c, const std::wstring& name,
                       std::function<bool(std::wstring&)> fn) {
    if (g_ov.busy || g_ui[(int)c].busy) return;
    ovLogAppend(L"==> " + name + L" ...");
    runAsync(c, name, [c, name, fn](std::wstring& err) {
        bool ok = fn(err);
        ovLogAppend(ok ? L"==> " + name + L" 完成"
                       : L"==> " + name + L" 失败: " + err);
        return ok;
    });
}

// ============================ Background polling ============================
// Component status and the pm2 list are computed on worker threads so the UI
// thread never blocks on disk scans or helper-process spawns (the source of
// drag / tab-switch lag). Workers fill this snapshot and post a message; the
// UI thread reads it and repaints.

static void statusWorker() {
    if (g_appClosing) return;
    for (int i = 0; i < (int)Comp::Count; ++i) {
        ComponentStatus st = compStatusQuick((Comp)i);
        std::lock_guard<std::mutex> lk(g_snapMtx);
        g_snapRunning[i] = st.running;
        g_snapInstalled[i] = st.installed;
        g_snapCurrent[i] = st.currentVersion;
        g_snapVersions[i] = st.versions;
    }
    g_statusWorkerBusy = false;
    PostMessageW(g_main, WM_UI_STATUS, 0, 0);
}

static void pm2Worker() {
    if (g_appClosing) return;
    ComponentStatus st = compStatusQuick(Comp::Nodejs);
    {
        std::lock_guard<std::mutex> lk(g_snapMtx);
        g_snapRunning[(int)Comp::Nodejs] = st.running;
        g_snapInstalled[(int)Comp::Nodejs] = st.installed;
        g_snapCurrent[(int)Comp::Nodejs] = st.currentVersion;
        g_snapVersions[(int)Comp::Nodejs] = st.versions;
    }
    // nodePm2List() -> runPm2() refuses to touch pm2 while the daemon is down,
    // so a stale pm2.pid can no longer make this poll spawn a daemon (the path
    // that produced the rpc.sock EPERM storm).
    if (g_curTab == compToTab(Comp::Nodejs)) {
        std::vector<PM2App> apps = nodePm2List();
        std::lock_guard<std::mutex> lk(g_snapMtx);
        g_snapPm2 = apps;
    }
    g_pm2WorkerBusy = false;
    PostMessageW(g_main, WM_UI_PM2, 0, 0);
}

static void kickStatusPoll() {
    if (g_statusWorkerBusy.exchange(true)) return;
    std::thread(statusWorker).detach();
}

static void kickPm2Poll() {
    // Cheap gate on the caller's side: do not even start a worker thread (nor
    // let it reach pm2) while the daemon is down or pm2.pid is stale.
    if (!compRunningQuick(Comp::Nodejs)) return;
    if (g_pm2WorkerBusy.exchange(true)) return;
    std::thread(pm2Worker).detach();
}

// ============================ Refresh functions ============================

// Defined with the add-site form (initNginxPage), but the vhost refresh needs
// it too: a php site can only point at a running php-cgi.
static void refreshPhpVerCombo(HWND parent);
static void showPhpExtDialog(HWND owner, const std::wstring& ver);

static void refreshNginxVHosts() {
    HWND lv = GetDlgItem(g_main, IDC_NG_VHOST_LIST);
    if (!lv) return;
    ListView_DeleteAllItems(lv);
    auto hosts = nginxListVHosts();
    for (auto& v : hosts) {
        LVITEMW item = {0};
        item.mask = LVIF_TEXT;
        item.iItem = ListView_GetItemCount(lv);
        item.pszText = (LPWSTR)v.name.c_str();
        ListView_InsertItem(lv, &item);
        ListView_SetItemText(lv, item.iItem, 1, (LPWSTR)v.domain.c_str());
        ListView_SetItemText(lv, item.iItem, 2, (LPWSTR)v.port.c_str());
        // 类型 shows which backend a site actually got. A php site also carries
        // its FastCGI port, so a stopped php-cgi is visible from here.
        std::wstring kind = v.kind == SiteKind::Php
            ? (v.phpTarget.empty() ? L"PHP" : L"PHP " + v.phpTarget)
            : L"Node";
        ListView_SetItemText(lv, item.iItem, 3, (LPWSTR)kind.c_str());
        ListView_SetItemText(lv, item.iItem, 4, (LPWSTR)v.root.c_str());
    }
    // A php site can only target a running php-cgi, so the version list the add
    // form offers has to follow the same state this list shows.
    refreshPhpVerCombo(g_main);
}

// The PHP version pool: one php-cgi per version, each on its own FastCGI port.
// All installed versions run at once, so this is the real status view — the
// common one-dot-one-version chrome above cannot express it.
static void syncWorkersEdit();

static void refreshPhpPool() {
    HWND lv = GetDlgItem(g_main, IDC_PHP_POOL_LIST);
    if (!lv) return;
    int prev = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    std::wstring keepSel;
    // Row -> version is the order of phpListPool(), so the selected version is
    // recovered by re-querying rather than by reading the control's text:
    // LVM_GETITEMTEXT copies whatever length the caller claims (the P0-2 trap)
    // and a version name is a directory name on disk, so it is not bounded.
    if (prev >= 0) {
        std::vector<PhpInstance> pool = phpListPool();
        if (prev < (int)pool.size()) keepSel = pool[prev].version;
    }
    ListView_DeleteAllItems(lv);
    int row = 0;
    for (const PhpInstance& pi : phpListPool()) {
        LVITEMW item = {0};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = (LPWSTR)pi.version.c_str();
        ListView_InsertItem(lv, &item);
        ListView_SetItemText(lv, row, 1, (LPWSTR)std::to_wstring(pi.workers).c_str());
        std::wstring ports = pi.port;
        if (pi.workers > 1 && !ports.empty())
            ports += L"-" + std::to_wstring(_wtoi(pi.port.c_str()) + pi.workers - 1);
        ListView_SetItemText(lv, row, 2, (LPWSTR)(ports.empty() ? L"未分配" : ports.c_str()));
        ListView_SetItemText(lv, row, 3, (LPWSTR)(pi.running ? L"运行中" : L"已停止"));
        ListView_SetItemText(lv, row, 4, (LPWSTR)(pi.isDefault ? L"★ 默认" : L""));
        ListView_SetItemText(lv, row, 5,
                             (LPWSTR)compDataVerDir(Comp::Php, pi.version).c_str());
        ++row;
    }
    if (!keepSel.empty()) {
        std::vector<PhpInstance> pool = phpListPool();
        for (int i = 0; i < (int)pool.size(); ++i) {
            if (pool[i].version == keepSel) {
                ListView_SetItemState(lv, i, LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                break;
            }
        }
    }
    syncWorkersEdit();
    refreshPhpVerCombo(g_main);
}

// Defined with the PHP pool list (below), but the vhost refresh and the
// workers box both need it earlier in the file.
static std::wstring phpSelectedVersion();

// Keep the "进程数" box in step with the selected row, so it always shows what
// that version is actually running rather than a stale number.
static void syncWorkersEdit() {
    HWND e = GetDlgItem(g_main, IDC_PHP_WORKERS);
    if (!e) return;
    std::wstring v = phpSelectedVersion();
    if (v.empty()) { SetWindowTextW(e, L""); return; }
    SetWindowTextW(e, std::to_wstring(phpWorkerCount(v)).c_str());
}

// Version string of the selected row in the PHP pool, or "" when nothing is
// selected. Same reason as above: derived from the pool, never from the
// control's own text.
static std::wstring phpSelectedVersion() {
    HWND lv = GetDlgItem(g_main, IDC_PHP_POOL_LIST);
    if (!lv) return std::wstring();
    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (sel < 0) return std::wstring();
    std::vector<PhpInstance> pool = phpListPool();
    if (sel >= (int)pool.size()) return std::wstring();
    return pool[sel].version;
}

static void refreshPgInfo() {
    HWND info = GetDlgItem(g_main, IDC_PG_INFO);
    if (!info) return;
    std::wstring ver;
    bool installed = false;
    {
        std::lock_guard<std::mutex> lk(g_snapMtx);
        ver = g_snapCurrent[(int)Comp::Postgresql];
        installed = g_snapInstalled[(int)Comp::Postgresql];
    }
    std::wstring txt = L"当前版本: " + ver + L"\r\n"
        + L"端口: " + pgPort() + L"\r\n"
        + L"用户: " + pgUser() + L"\r\n"
        + L"数据目录: " + (installed ? compDataVerDir(Comp::Postgresql, ver) : L"(未安装)");
    SetWindowTextW(info, txt.c_str());
}

static void refreshRedisInfo() {
    HWND info = GetDlgItem(g_main, IDC_REDIS_INFO);
    if (!info) return;
    std::wstring ver;
    bool installed = false;
    {
        std::lock_guard<std::mutex> lk(g_snapMtx);
        ver = g_snapCurrent[(int)Comp::Redis];
        installed = g_snapInstalled[(int)Comp::Redis];
    }
    std::wstring txt = L"当前版本: " + ver + L"\r\n端口: " + redisPort()
        + L"\r\n数据目录: " + (installed ? compDataVerDir(Comp::Redis, ver) : L"(未安装)");
    SetWindowTextW(info, txt.c_str());
}

static void refreshPm2List() {
    HWND lv = GetDlgItem(g_main, IDC_NODE_PM2_LIST);
    if (!lv) return;
    std::vector<PM2App> apps;
    {
        std::lock_guard<std::mutex> lk(g_snapMtx);
        apps = g_snapPm2;
    }
    // remember the selected pm_id so a periodic refresh doesn't drop the selection
    int selPmId = -1;
    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (sel >= 0) {
        wchar_t idBuf[64];
        ListView_GetItemText(lv, sel, 0, idBuf, 64);
        selPmId = _wtoi(idBuf);
    }
    ListView_DeleteAllItems(lv);
    int selNew = -1;
    for (auto& a : apps) {
        LVITEMW item = {0};
        item.mask = LVIF_TEXT;
        item.iItem = ListView_GetItemCount(lv);
        std::wstring id = std::to_wstring(a.id);
        item.pszText = (LPWSTR)id.c_str();
        ListView_InsertItem(lv, &item);
        ListView_SetItemText(lv, item.iItem, 1, (LPWSTR)a.name.c_str());
        ListView_SetItemText(lv, item.iItem, 2, (LPWSTR)a.status.c_str());
        // pid / alive / stale are separate columns now, so an app pm2 still calls
        // "online" while its process is gone is visible at a glance instead of
        // being hidden behind pm2's own bookkeeping. ListView_SetItemText copies
        // the text during the call, so these temporaries are safe.
        std::wstring pidText = a.pid ? std::to_wstring(a.pid) : std::wstring(L"-");
        ListView_SetItemText(lv, item.iItem, 3, (LPWSTR)pidText.c_str());
        bool alive = a.pid != 0 && isPidAlive(a.pid);
        ListView_SetItemText(lv, item.iItem, 4, (LPWSTR)(alive ? L"是" : L"否"));
        ListView_SetItemText(lv, item.iItem, 5, (LPWSTR)(a.stale ? L"是" : L"否"));
        std::wstring restartText = std::to_wstring(a.restarts);
        ListView_SetItemText(lv, item.iItem, 6, (LPWSTR)restartText.c_str());
        if (a.id == selPmId) selNew = item.iItem;
    }
    if (selNew >= 0) {
        ListView_SetItemState(lv, selNew, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
}

static void refreshAll(Comp c) {
    refreshVersions(c);
    switch (c) {
        case Comp::Nginx: refreshNginxVHosts(); break;
        case Comp::Postgresql: refreshPgInfo(); break;
        case Comp::Redis: refreshRedisInfo(); break;
        case Comp::Nodejs: refreshPm2List(); break;
        case Comp::Php: refreshPhpPool(); break;
        default: break;
    }
}

static void refreshOverview() {
    OverviewUI& ov = g_ov;
    bool allBusy = ov.busy;
    for (int i = 0; i < (int)Comp::Count; ++i) {
        if (!ov.dot[i]) continue;
        bool running, installed;
        std::wstring ver;
        {
            std::lock_guard<std::mutex> lk(g_snapMtx);
            running = g_snapRunning[i];
            installed = g_snapInstalled[i];
            ver = g_snapCurrent[i];
        }
        int state = installed ? (running ? 2 : 1) : 0;
        SetWindowLongPtrW(ov.dot[i], GWLP_USERDATA, state);
        InvalidateRect(ov.dot[i], nullptr, TRUE);
        // Say "操作中" while an operation is in flight. The poller reports
        // liveness independently, so a component can read "运行中" while its
        // button is disabled — with no such wording the two look contradictory
        // and the greyed button reads as a bug rather than as "wait".
        bool busy = g_ui[i].busy;
        std::wstring txt = !installed ? L"未安装"
                         : (busy ? L"操作中…"
                                 : (running ? L"运行中 " + ver : L"已停止"));
        SetWindowTextW(ov.status[i], txt.c_str());
        SetWindowTextW(ov.btnToggle[i], busy ? L"…" : (running ? L"停止" : L"启动"));
        EnableWindow(ov.btnToggle[i], !busy && !allBusy);
    }
    EnableWindow(ov.btnAllStart, !allBusy);
    EnableWindow(ov.btnAllRestart, !allBusy);
    EnableWindow(ov.btnAllStop, !allBusy);
}

// ============================ Actions ============================

static void actStart(Comp c) {
    runAsync(c, L"启动 " + std::wstring(compDisplay(c)), [c](std::wstring& err) {
        return compStart(c, err);
    });
}

static void actStop(Comp c) {
    runAsync(c, L"停止 " + std::wstring(compDisplay(c)), [c](std::wstring& err) {
        return compStop(c, err);
    });
}

static void actSwitch(Comp c) {
    CompUI& ui = g_ui[(int)c];
    int idx = (int)SendMessageW(ui.verCombo, CB_GETCURSEL, 0, 0);
    if (idx < 0) return;
    // CB_GETLBTEXT copies as much as the caller claims to have room for — the
    // control has no idea how big `buf` is. Ask for the length first: version
    // names come from directory names under bin\ and a long one used to smash
    // the stack.
    int len = (int)SendMessageW(ui.verCombo, CB_GETLBTEXTLEN, idx, 0);
    if (len < 0) return;
    std::vector<wchar_t> buf((size_t)len + 1, L'\0');
    SendMessageW(ui.verCombo, CB_GETLBTEXT, idx, (LPARAM)buf.data());
    std::wstring ver = buf.data();
    // Quick probe on purpose: compStatus() can spawn redis-cli / pm2 and block
    // the UI thread for seconds (see isUiThread() in proc.h).
    ComponentStatus st = compStatusQuick(c);
    if (ver == st.currentVersion) {
        logMsgUi(c, L"已是当前版本 " + ver);
        return;
    }
    runAsync(c, L"切换到版本 " + ver, [c, ver](std::wstring& err) {
        return compSwitchVersion(c, ver, err);
    });
}

// ---- Overview actions ----

static std::wstring ovAutoKey(Comp c) { return L"autostart." + std::wstring(compName(c)); }
static bool ovAutoEnabled(Comp c) { return iniGet(ovAutoKey(c), L"false") == L"true"; }
static bool ovAutoAllEnabled() { return iniGet(L"autostart.enabled", L"false") == L"true"; }

static void ovToggle(Comp c) {
    if (g_ov.busy || g_ui[(int)c].busy) return;
    bool running = false;
    {
        std::lock_guard<std::mutex> lk(g_snapMtx);
        running = g_snapRunning[(int)c];
    }
    std::wstring name = (running ? L"停止 " : L"启动 ") + std::wstring(compDisplay(c));
    ovRunAsync(c, name, [c, running](std::wstring& err) {
        return running ? compStop(c, err) : compStart(c, err);
    });
}

enum class AllOp { Start, Restart, Stop };

static void ovAllOp(AllOp op) {
    if (g_ov.busy) return;
    g_ov.busy = true;
    refreshOverview();
    const wchar_t* opName =
        op == AllOp::Start ? L"全部启动" :
        op == AllOp::Restart ? L"全部重启" : L"全部停止";
    ovLogAppend(L"==> " + std::wstring(opName) + L" ...");
    std::thread([op]() {
        int fail = 0;
        for (int i = 0; i < (int)Comp::Count; ++i) {
            if (g_appClosing) break;
            Comp c = (Comp)i;
            // A per-component operation may already be in flight (startup
            // autostart, or a button the user pressed just before this one):
            // running both would race on the same port / data directory.
            if (g_ui[i].busy) {
                ovLogAppend(L"- " + std::wstring(compDisplay(c)) + L": 跳过 (正在执行其他操作)");
                continue;
            }
            std::wstring e;
            bool ok = true;
            switch (op) {
                case AllOp::Start: ok = compStart(c, e); break;
                case AllOp::Restart: ok = compStop(c, e); if (ok) ok = compStart(c, e); break;
                case AllOp::Stop: ok = compStop(c, e); break;
            }
            if (!ok) {
                bool ignore = (e.find(L"已在运行") != std::wstring::npos) ||
                              (e.find(L"组件未安装") != std::wstring::npos) ||
                              (e.find(L"未初始化") != std::wstring::npos) ||
                              (e.find(L"未选择") != std::wstring::npos);
                if (ignore) {
                    ovLogAppend(L"- " + std::wstring(compDisplay(c)) + L": 跳过 (" + e + L")");
                } else {
                    ++fail;
                    logMsgUi(c, L"[全部操作] " + e);
                    ovLogAppend(L"- " + std::wstring(compDisplay(c)) + L": 失败 (" + e + L")");
                }
            } else {
                ovLogAppend(L"- " + std::wstring(compDisplay(c)) + L": 成功");
            }
        }
        g_ov.busy = false;
        PostMessageW(g_main, WM_ALL_DONE, (WPARAM)op, fail);
    }).detach();
}

static void ovAutoAll(HWND ctl) {
    bool on = SendMessageW(ctl, BM_GETCHECK, 0, 0) == BST_CHECKED;
    iniSet(L"autostart.enabled", on ? L"true" : L"false");
    ovLogAppend(on ? L"随管理器自动启动：总开关已开启"
                   : L"随管理器自动启动：总开关已关闭");
}

static void ovAutoComp(Comp c, HWND ctl) {
    bool on = SendMessageW(ctl, BM_GETCHECK, 0, 0) == BST_CHECKED;
    iniSet(ovAutoKey(c), on ? L"true" : L"false");
    ovLogAppend(std::wstring(compDisplay(c)) +
                (on ? L"：已设置为随管理器启动"
                    : L"：已取消随管理器启动"));
}

// Called once at startup: start components whose "随管理器启动" box is checked.
static void autoStartComponents() {
    if (!ovAutoAllEnabled()) return;
    for (int i = 0; i < (int)Comp::Count; ++i) {
        Comp c = (Comp)i;
        if (!ovAutoEnabled(c)) continue;
        ovRunAsync(c, L"随管理器自动启动 " + std::wstring(compDisplay(c)), [c](std::wstring& err) {
            return compStart(c, err);
        });
    }
}

// ============================ Boot start (registry Run key) ============================

static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_VALUE = L"LNPP Manager";

static bool bootStartEnabled() {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    LONG r = RegQueryValueExW(hk, RUN_VALUE, nullptr, &type, nullptr, &size);
    RegCloseKey(hk);
    return r == ERROR_SUCCESS;
}

static void bootStartSet(bool on) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
        return;
    if (on) {
        // start hidden to tray at Windows login
        std::wstring exe = L"\"" + joinPath(exeDir(), L"lnpp.exe") + L"\" --hidden";
        RegSetValueExW(hk, RUN_VALUE, 0, REG_SZ, (const BYTE*)exe.c_str(),
                       (DWORD)((exe.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(hk, RUN_VALUE);
    }
    RegCloseKey(hk);
}

// ---- user PATH helpers (persist the portable node dir) ----

enum class PathRead { Ok, Missing, Failed };

// Read HKCU\Environment\Path. The three outcomes must stay distinct: treating
// "read failed" as "empty PATH" made pathAddNode() write back a Path value
// holding nothing but the node directory — i.e. wiping the user's PATH.
static PathRead readUserPath(std::wstring& path, bool& present) {
    present = false;
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return PathRead::Failed;
    DWORD type = REG_EXPAND_SZ;
    std::vector<wchar_t> buf(32768, L'\0');
    DWORD size = (DWORD)(buf.size() * sizeof(wchar_t));
    LONG r = RegQueryValueExW(hk, L"Path", nullptr, &type, (LPBYTE)buf.data(), &size);
    RegCloseKey(hk);
    if (r == ERROR_FILE_NOT_FOUND) { path.clear(); return PathRead::Missing; }
    if (r != ERROR_SUCCESS) { path.clear(); return PathRead::Failed; }
    size_t len = 0;
    while (len < buf.size() && buf[len]) ++len;
    path.assign(buf.data(), len);
    std::wstring nodeDir = lowerStr(compBinDirVer(Comp::Nodejs, iniGet(L"ver.nodejs", L"")));
    std::wstringstream ss(path);
    std::wstring item;
    while (std::getline(ss, item, L';')) {
        if (lowerStr(trimStr(item)) == nodeDir) { present = true; break; }
    }
    return PathRead::Ok;
}

static void pathAddNode() {
    std::wstring nodeDir = compBinDirVer(Comp::Nodejs, iniGet(L"ver.nodejs", L""));
    if (nodeDir.empty() || !dirExists(nodeDir)) {
        ovLogAppend(L"Node 路径不可用，未加入用户 PATH");
        return;
    }
    std::wstring path;
    bool present = false;
    PathRead rd = readUserPath(path, present);
    if (rd == PathRead::Failed) {
        // Refuse to write: a half-read PATH must never become a whole new one.
        ovLogAppend(L"读取用户 PATH 失败，未做修改（不会覆盖现有 PATH）");
        logMsg(L"path", L"读取用户 PATH 失败，未加入 Node 目录");
        return;
    }
    if (present) {
        ovLogAppend(L"Node 路径已在用户 PATH 中: " + nodeDir);
        logMsg(L"path", L"Node 路径已在 PATH 中: " + nodeDir);
        return;
    }
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS) {
        ovLogAppend(L"打开用户环境变量失败，未加入 Node 目录");
        return;
    }
    std::wstring np = path;
    if (!np.empty() && np.back() != L';') np += L";";
    np += nodeDir;
    RegSetValueExW(hk, L"Path", 0, REG_EXPAND_SZ, (const BYTE*)np.c_str(),
                   (DWORD)((np.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
                        SMTO_ABORTIFHUNG, 5000, nullptr);
    logMsg(L"path", L"已将 Node 目录加入用户 PATH: " + nodeDir);
    ovLogAppend(L"已将 Node 目录加入用户 PATH: " + nodeDir);
}

static void pathRemoveNode() {
    std::wstring nodeDir = lowerStr(compBinDirVer(Comp::Nodejs, iniGet(L"ver.nodejs", L"")));
    std::wstring path;
    bool present = false;
    PathRead rd = readUserPath(path, present);
    if (rd != PathRead::Ok) {
        ovLogAppend(rd == PathRead::Missing ? L"用户 PATH 不存在，无需移除"
                                            : L"读取用户 PATH 失败，未做修改");
        return;
    }
    if (!present) {
        ovLogAppend(L"Node 路径不在用户 PATH 中");
        logMsg(L"path", L"Node 路径不在 PATH 中");
        return;
    }
    std::wstringstream ss(path);
    std::wstring item, np;
    while (std::getline(ss, item, L';')) {
        if (lowerStr(trimStr(item)) == nodeDir) continue;
        if (!np.empty()) np += L";";
        np += item;
    }
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS) {
        ovLogAppend(L"打开用户环境变量失败，未移除 Node 目录");
        return;
    }
    RegSetValueExW(hk, L"Path", 0, REG_EXPAND_SZ, (const BYTE*)np.c_str(),
                   (DWORD)((np.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
                        SMTO_ABORTIFHUNG, 5000, nullptr);
    logMsg(L"path", L"已从用户 PATH 移除 Node 目录");
    ovLogAppend(L"已从用户 PATH 移除 Node 目录");
}

// ============================ System tray ============================

static void trayAdd() {
    if (g_trayAdded || !g_main) return;
    ZeroMemory(&g_nid, sizeof(g_nid));
    // V3 size supports balloons and is the modern recommended struct size
    g_nid.cbSize = NOTIFYICONDATAW_V3_SIZE;
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP),
                                    IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    wcscpy_s(g_nid.szTip, L"LNPP 组件管理器");
    BOOL ok = Shell_NotifyIconW(NIM_ADD, &g_nid);
    if (!ok) {
        logMsg(L"tray", L"Shell_NotifyIcon 添加失败 错误 " + std::to_wstring(GetLastError()));
        return;
    }
    g_trayAdded = true;
    g_nid.uVersion = NOTIFYICON_VERSION;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void trayRemove() {
    if (g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
}

static void trayBalloon(const wchar_t* title, const wchar_t* msg) {
    if (!g_trayAdded || !g_main) return;
    ZeroMemory(g_nid.szInfoTitle, sizeof(g_nid.szInfoTitle));
    ZeroMemory(g_nid.szInfo, sizeof(g_nid.szInfo));
    wcscpy_s(g_nid.szInfoTitle, title);
    wcscpy_s(g_nid.szInfo, msg);
    g_nid.dwInfoFlags = NIIF_INFO;
    g_nid.uTimeout = 3000;
    g_nid.uFlags = NIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// Stop every component and verify it is really down (retry a few times).
// Runs on a worker thread only: a full stop cycle can take tens of seconds
// (pg_ctl -w, nginx grace period, pm2 kill), which must never happen on the UI
// thread. Sets g_shutdownStarted so WM_DESTROY knows the stack is being handled.
static void shutdownAllComponents() {
    g_shutdownStarted = true;
    for (int i = 0; i < (int)Comp::Count; ++i) {
        if (g_appClosing) break;
        Comp c = (Comp)i;
        std::wstring err;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (!compRunningQuick(c)) break;   // already down
            compStop(c, err);
            if (!compRunningQuick(c)) break;   // confirmed down
            Sleep(500);                        // wait for a lingering process
        }
    }
}

static void trayShowMain() {
    trayAdd();   // re-add in case explorer dropped the icon
    ShowWindow(g_main, SW_SHOW);
    SetForegroundWindow(g_main);
}

static void showTrayMenu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_TRAY_SHOW, L"显示主窗口");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < (int)Comp::Count; ++i) {
        bool running = false;
        {
            std::lock_guard<std::mutex> lk(g_snapMtx);
            running = g_snapRunning[i];
        }
        std::wstring label = std::wstring(compDisplay((Comp)i)) +
                             (running ? L": 运行中" : L": 已停止");
        AppendMenuW(m, MF_STRING, IDM_TRAY_COMP_BASE + i, label.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TRAY_ALL_START, L"全部启动");
    AppendMenuW(m, MF_STRING, IDM_TRAY_ALL_STOP, L"全部停止");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TRAY_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    int cmd = (int)TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);

    if (cmd == IDM_TRAY_SHOW) {
        trayShowMain();
    } else if (cmd >= IDM_TRAY_COMP_BASE && cmd < IDM_TRAY_COMP_BASE + (int)Comp::Count) {
        ovToggle((Comp)(cmd - IDM_TRAY_COMP_BASE));
    } else if (cmd == IDM_TRAY_ALL_START) {
        ovAllOp(AllOp::Start);
    } else if (cmd == IDM_TRAY_ALL_STOP) {
        ovAllOp(AllOp::Stop);
    } else if (cmd == IDM_TRAY_EXIT) {
        // stop all components first (background), then really quit
        ShowWindow(hwnd, SW_HIDE);
        trayBalloon(L"LNPP 组件管理器", L"正在停止所有组件...");
        std::thread([]() {
            shutdownAllComponents();
            PostMessageW(g_main, WM_REAL_EXIT, 0, 0);
        }).detach();
    }
}

// ============================ Tab control ============================
static void refreshPgUsers();   // defined later
static void modal_loop(HWND dlg, HWND owner);   // defined later
static void centerOn(HWND dlg, HWND owner);   // defined later

static void showPage(int idx) {
    g_curTab = idx;
    bool ov = (idx == TAB_OVERVIEW);
    for (HWND h : g_ov.controls)
        ShowWindow(h, ov ? SW_SHOW : SW_HIDE);
    for (int i = 0; i < (int)Comp::Count; ++i) {
        bool show = (idx == compToTab((Comp)i));
        for (HWND h : g_ui[i].pageControls)
            ShowWindow(h, show ? SW_SHOW : SW_HIDE);
    }
    // nginx SSL inputs are toggled by the checkbox (not in pageControls);
    // hide them when leaving the nginx page, re-apply the checkbox state on entry
    bool ngActive = (idx == compToTab(Comp::Nginx));
    bool sslOn = ngActive &&
                 SendMessageW(GetDlgItem(g_main, IDC_NG_SSL), BM_GETCHECK, 0, 0) == BST_CHECKED;
    for (int ctl : { IDC_NG_LABEL_CERT, IDC_NG_CERT, IDC_NG_BTN_CERT,
                     IDC_NG_LABEL_KEY, IDC_NG_KEY, IDC_NG_BTN_KEY }) {
        HWND c = GetDlgItem(g_main, ctl);
        if (c) ShowWindow(c, sslOn ? SW_SHOW : SW_HIDE);
    }
    // refresh current page content (reads the snapshot) and ask workers for fresh data
    if (ov) refreshOverview();
    else refreshAll(tabToComp(idx));
    if (idx == compToTab(Comp::Postgresql)) refreshPgUsers();
    kickStatusPoll();
    kickPm2Poll();
}

// ============================ Poll timers ============================

static void CALLBACK statusTimer(HWND, UINT, UINT_PTR, DWORD) {
    kickStatusPoll();
}

static void CALLBACK pm2Timer(HWND, UINT, UINT_PTR, DWORD) {
    kickPm2Poll();
}

// drain any pending ini writes. Runs on the UI thread every 50ms; cheap
// when nothing's dirty.
static void CALLBACK iniFlushTimer(HWND, UINT, UINT_PTR, DWORD) {
    iniFlushIfDue();
}

// ---- apply snapshots on the UI thread (cheap repaints only) ----

static void applyStatusSnapshot() {
    for (int i = 0; i < (int)Comp::Count; ++i) {
        CompUI& ui = g_ui[i];
        if (!ui.statusTxt) continue;
        bool running, installed;
        {
            std::lock_guard<std::mutex> lk(g_snapMtx);
            running = g_snapRunning[i];
            installed = g_snapInstalled[i];
        }
        setStatus((Comp)i, running, installed);
    }
    int cur = g_tab ? TabCtrl_GetCurSel(g_tab) : -1;
    if (cur >= TAB_COMP_BASE) refreshVersions(tabToComp(cur));
    refreshPgInfo();
    refreshRedisInfo();
    refreshOverview();
}

static void applyPm2Snapshot() {
    CompUI& ui = g_ui[(int)Comp::Nodejs];
    if (ui.statusTxt) {
        bool running, installed;
        {
            std::lock_guard<std::mutex> lk(g_snapMtx);
            running = g_snapRunning[(int)Comp::Nodejs];
            installed = g_snapInstalled[(int)Comp::Nodejs];
        }
        setStatus(Comp::Nodejs, running, installed);
    }
    refreshVersions(Comp::Nodejs);
    int cur = g_tab ? TabCtrl_GetCurSel(g_tab) : -1;
    if (cur >= TAB_COMP_BASE && tabToComp(cur) == Comp::Nodejs) {
        refreshPm2List();
    }
    refreshOverview();
}

// ============================ Dot control ============================

static const wchar_t* DOT_CLASS = L"LNPPStatusDot";

static LRESULT CALLBACK DotProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            // 0 = not installed, 1 = installed but stopped, 2 = running.
            // Create one brush and actually select it into the DC: the old code
            // built two brushes per paint (leaking the first) and never selected
            // either, so every dot rendered with the DC's default white brush.
            static const COLORREF kStateColor[3] = {
                RGB(150, 150, 150), RGB(220, 60, 60), RGB(60, 200, 60)
            };
            int state = (int)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (state < 0 || state > 2) state = 0;
            HBRUSH br = CreateSolidBrush(kStateColor[state]);
            if (br) {
                HGDIOBJ oldBr = SelectObject(dc, br);
                Ellipse(dc, rc.left + 2, rc.top + 2, rc.right - 2, rc.bottom - 2);
                SelectObject(dc, oldBr);
                DeleteObject(br);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ============================ Overview page (first tab) ============================

static void addTooltip(HWND parent, HWND ctl, const wchar_t* text) {
    if (!g_tip) {
        g_tip = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr,
                                WS_POPUP | TTS_ALWAYSTIP,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!g_tip) return;
    }
    TOOLINFOW ti = {0};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND;
    ti.hwnd = parent;
    ti.uId = (UINT_PTR)ctl;
    ti.lpszText = (LPWSTR)text;
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static void initOverviewPage(HWND parent) {
    OverviewUI& ov = g_ov;
    ov.controls.clear();
    HWND chk = makeCtl(IDC_OV_AUTOSTART_ALL, L"BUTTON", L"随管理器自动启动",
                       BS_AUTOCHECKBOX, 20, 40, 340, 20, parent);
    ov.chkAutoAll = chk;
    SendMessageW(chk, BM_SETCHECK, ovAutoAllEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    ov.controls.push_back(chk);
    addTooltip(parent, chk, L"总开关：勾选后，下方勾选了「随管理器启动」的组件随管理器一起启动");

    HWND chkBoot = makeCtl(IDC_OV_BOOT_START, L"BUTTON", L"开机启动",
                           BS_AUTOCHECKBOX, 370, 40, 330, 20, parent);
    SendMessageW(chkBoot, BM_SETCHECK, bootStartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    ov.controls.push_back(chkBoot);
    addTooltip(parent, chkBoot, L"勾选后，管理器随 Windows 开机启动（最小化到托盘）");

    ov.btnAllStart = makeCtl(IDC_OV_BTN_ALL_START, L"BUTTON", L"全部启动", BS_PUSHBUTTON, 20, 68, 90, 26, parent);
    ov.btnAllRestart = makeCtl(IDC_OV_BTN_ALL_RESTART, L"BUTTON", L"全部重启", BS_PUSHBUTTON, 118, 68, 90, 26, parent);
    ov.btnAllStop = makeCtl(IDC_OV_BTN_ALL_STOP, L"BUTTON", L"全部停止", BS_PUSHBUTTON, 216, 68, 90, 26, parent);
    ov.controls.push_back(ov.btnAllStart);
    ov.controls.push_back(ov.btnAllRestart);
    ov.controls.push_back(ov.btnAllStop);

    // Component rows. The pitch is derived from Comp::Count instead of being
    // hardcoded: with four components a fixed 40px stride happened to end just
    // above the PATH button row, so the fifth row (PHP) landed at y=272 and
    // painted straight over the buttons at y=264. Now the band between the
    // "全部启动" row and that button row is divided by however many components
    // exist, capped so few components do not spread out and many do not crush.
    const int kRowTop = 102;
    const int kRowLimit = 258;          // first y the PATH button row may use
    int pitch = (kRowLimit - kRowTop) / (int)Comp::Count;
    if (pitch > 40) pitch = 40;
    if (pitch < 26) pitch = 26;         // a row is 24px tall; below this they touch
    for (int i = 0; i < (int)Comp::Count; ++i) {
        int y = kRowTop + i * pitch;
        Comp c = (Comp)i;
        HWND dot = makeCtl(0, DOT_CLASS, L"", WS_CHILD | WS_VISIBLE, 24, y + 4, 16, 16, parent);
        HWND name = makeCtl(0, L"STATIC", compDisplay(c), SS_LEFT, 48, y, 96, 20, parent);
        HWND st = makeCtl(IDC_OV_COMP_STATUS_BASE + i, L"STATIC", L"", SS_LEFT, 150, y, 110, 20, parent);
        HWND tgl = makeCtl(IDC_OV_COMP_START_BASE + i, L"BUTTON", L"启动", BS_PUSHBUTTON, 270, y - 2, 80, 24, parent);
        HWND ac = makeCtl(IDC_OV_COMP_AUTO_BASE + i, L"BUTTON", L"随管理器启动", BS_AUTOCHECKBOX, 365, y, 130, 20, parent);
        SendMessageW(ac, BM_SETCHECK, ovAutoEnabled(c) ? BST_CHECKED : BST_UNCHECKED, 0);
        ov.dot[i] = dot;
        ov.status[i] = st;
        ov.btnToggle[i] = tgl;
        ov.chkAuto[i] = ac;
        ov.controls.push_back(dot);
        ov.controls.push_back(name);
        ov.controls.push_back(st);
        ov.controls.push_back(tgl);
        ov.controls.push_back(ac);
    }

    ov.btnPathAdd = makeCtl(IDC_OV_BTN_PATH_ADD, L"BUTTON", L"Node 加入 PATH", BS_PUSHBUTTON, 20, 264, 130, 26, parent);
    ov.btnPathDel = makeCtl(IDC_OV_BTN_PATH_DEL, L"BUTTON", L"Node 移除 PATH", BS_PUSHBUTTON, 160, 264, 130, 26, parent);
    ov.controls.push_back(ov.btnPathAdd);
    ov.controls.push_back(ov.btnPathDel);

    HWND btnDl = makeCtl(IDC_OV_BTN_DL, L"BUTTON", L"下载组件", BS_PUSHBUTTON, 300, 264, 130, 26, parent);
    ov.controls.push_back(btnDl);
    addTooltip(parent, btnDl, L"从 packages.conf 列出的地址下载并解压组件到 bin（首次运行也会自动弹出）");

    ov.result = makeCtl(IDC_OV_RESULT, L"STATIC", L"", SS_LEFT, 20, 294, 700, 20, parent);
    ov.controls.push_back(ov.result);

    HWND log = makeCtl(IDC_OV_LOG, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
                       ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                       10, 320, 700, 190, parent);
    SendMessageW(log, WM_SETFONT, (WPARAM)g_monoFont, TRUE);
    ov.logEdit = log;
    ov.controls.push_back(log);
    // Ends at y=540. The client area is 553 (window 760x610 less the caption
    // and border), so the button clears the bottom edge instead of being sliced
    // in half by it — which is what happened at 560.
    HWND bClearLog = makeCtl(IDC_OV_BTN_CLEAR_LOG, L"BUTTON", L"清空日志", BS_PUSHBUTTON, 620, 516, 90, 24, parent);
    ov.controls.push_back(bClearLog);

    // bottom-right: version text + "关于" link
    std::wstring verTxt = L"v" + appVersion();
    HWND ver = makeCtl(IDC_OV_VER_TXT, L"STATIC", verTxt.c_str(), SS_RIGHT, 620, 292, 90, 20, parent);
    ov.controls.push_back(ver);
    HWND about = makeCtl(IDC_OV_ABOUT_LINK, L"STATIC", L"关于", SS_NOTIFY | SS_RIGHT, 690, 292, 40, 20, parent);
    SendMessageW(about, WM_SETFONT, (WPARAM)g_linkFont, TRUE);
    ov.controls.push_back(about);
    ov.aboutLink = about;
    ov.verTxt = ver;
}


// ============================ Main window proc ============================

static void initNginxPage(HWND parent) {
    CompUI& ui = g_ui[(int)Comp::Nginx];
    // NOTE: no clear() here — pageControls already holds the common controls
    // from initCommonControls; append the page-specific ones on top.
    HWND lv = makeCtl(IDC_NG_VHOST_LIST, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
                      LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                      20, 82, 420, 160, parent);
    ui.pageControls.push_back(lv);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW col = {0};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 100; col.pszText = (LPWSTR)L"站点"; ListView_InsertColumn(lv, 0, &col);
    col.cx = 130; col.pszText = (LPWSTR)L"域名"; ListView_InsertColumn(lv, 1, &col);
    col.cx = 50;  col.pszText = (LPWSTR)L"端口"; ListView_InsertColumn(lv, 2, &col);
    col.cx = 60;  col.pszText = (LPWSTR)L"类型"; ListView_InsertColumn(lv, 3, &col);
    col.cx = 80;  col.pszText = (LPWSTR)L"根目录"; ListView_InsertColumn(lv, 4, &col);

    HWND bAdd = makeCtl(IDC_NG_BTN_ADD, L"BUTTON", L"添加站点", BS_PUSHBUTTON, 20, 248, 90, 26, parent);
    HWND bDel = makeCtl(IDC_NG_BTN_DEL, L"BUTTON", L"删除", BS_PUSHBUTTON, 120, 248, 70, 26, parent);
    HWND bReload = makeCtl(IDC_NG_BTN_RELOAD, L"BUTTON", L"重载", BS_PUSHBUTTON, 200, 248, 70, 26, parent);

    // Right-hand add-site form. 21px pitch instead of 22 so the two extra rows
    // (site kind, php version) still end above the log box at y=280.
    HWND lName = makeCtl(IDC_NG_LABEL_NAME, L"STATIC", L"站点名:", SS_LEFT, 450, 82, 60, 20, parent);
    HWND eName = makeCtl(IDC_NG_ADD_NAME, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, 510, 80, 170, 22, parent);
    HWND lDomain = makeCtl(IDC_NG_LABEL_DOMAIN, L"STATIC", L"域名:", SS_LEFT, 450, 103, 60, 20, parent);
    HWND eDomain = makeCtl(IDC_NG_ADD_DOMAIN, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, 510, 101, 170, 22, parent);
    HWND lPort = makeCtl(IDC_NG_LABEL_PORT, L"STATIC", L"端口:", SS_LEFT, 450, 124, 60, 20, parent);
    HWND ePort = makeCtl(IDC_NG_ADD_PORT, L"EDIT", L"80", WS_CHILD | WS_VISIBLE | WS_BORDER, 510, 122, 170, 22, parent);
    HWND lRoot = makeCtl(IDC_NG_LABEL_ROOT, L"STATIC", L"根目录:", SS_LEFT, 450, 145, 60, 20, parent);
    HWND eRoot = makeCtl(IDC_NG_ROOT, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, 510, 143, 150, 22, parent);
    HWND bRoot = makeCtl(IDC_NG_BTN_ROOT, L"BUTTON", L"浏览", BS_PUSHBUTTON, 665, 143, 45, 22, parent);
    // The kind decides which etc\ template the vhost is rendered from: node
    // (static files first, then proxy_pass) or php (static files first, then
    // fastcgi_pass to a php-cgi instance).
    HWND lKind = makeCtl(IDC_NG_LABEL_KIND, L"STATIC", L"类型:", SS_LEFT, 450, 166, 60, 20, parent);
    HWND cKind = makeCtl(IDC_NG_KIND, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
                         CBS_DROPDOWNLIST | WS_VSCROLL, 510, 164, 170, 200, parent);
    SendMessageW(cKind, CB_ADDSTRING, 0, (LPARAM)L"Node.js（静态优先）");
    SendMessageW(cKind, CB_ADDSTRING, 0, (LPARAM)L"PHP（FastCGI）");
    SendMessageW(cKind, CB_SETCURSEL, 0, 0);
    // Which php-cgi this site talks to. Only meaningful for php sites, so it
    // starts hidden, like the SSL inputs below.
    HWND lPhpVer = makeCtl(IDC_NG_LABEL_PHPVER, L"STATIC", L"PHP 版本:", SS_LEFT, 450, 187, 60, 20, parent);
    HWND cPhpVer = makeCtl(IDC_NG_PHPVER, WC_COMBOBOXW, L"", WS_CHILD | WS_BORDER |
                           CBS_DROPDOWNLIST | WS_VSCROLL, 510, 185, 170, 200, parent);
    for (HWND h : { lPhpVer, cPhpVer }) ShowWindow(h, SW_HIDE);
    HWND chkSsl = makeCtl(IDC_NG_SSL, L"BUTTON", L"HTTPS/SSL", BS_AUTOCHECKBOX, 450, 208, 120, 20, parent);
    HWND lCert = makeCtl(IDC_NG_LABEL_CERT, L"STATIC", L"证书:", SS_LEFT, 450, 229, 60, 20, parent);
    HWND eCert = makeCtl(IDC_NG_CERT, L"EDIT", L"", WS_CHILD | WS_BORDER, 510, 227, 150, 22, parent);
    HWND bCert = makeCtl(IDC_NG_BTN_CERT, L"BUTTON", L"浏览", BS_PUSHBUTTON, 665, 227, 45, 22, parent);
    HWND lKey = makeCtl(IDC_NG_LABEL_KEY, L"STATIC", L"Key:", SS_LEFT, 450, 250, 60, 20, parent);
    HWND eKey = makeCtl(IDC_NG_KEY, L"EDIT", L"", WS_CHILD | WS_BORDER, 510, 248, 150, 22, parent);
    HWND bKey = makeCtl(IDC_NG_BTN_KEY, L"BUTTON", L"浏览", BS_PUSHBUTTON, 665, 248, 45, 22, parent);
    // SSL inputs start hidden; the HTTPS/SSL checkbox reveals them
    for (HWND h : { lCert, eCert, bCert, lKey, eKey, bKey }) ShowWindow(h, SW_HIDE);
    // Kind / php-version / SSL controls are driven by their own combo and
    // checkbox, not by the tab, so keep them out of pageControls — otherwise
    // switching tabs would force them all visible.
    ui.pageControls.push_back(lName);
    ui.pageControls.push_back(lDomain);
    ui.pageControls.push_back(lPort);
    ui.pageControls.push_back(lRoot);
    ui.pageControls.push_back(eRoot);
    ui.pageControls.push_back(bRoot);
    ui.pageControls.push_back(lKind);
    ui.pageControls.push_back(cKind);
    ui.pageControls.push_back(chkSsl);
    ui.pageControls.push_back(eName);
    ui.pageControls.push_back(eDomain);
    ui.pageControls.push_back(ePort);
    ui.pageControls.push_back(bAdd);
    ui.pageControls.push_back(bDel);
    ui.pageControls.push_back(bReload);
    refreshPhpVerCombo(parent);
}

// Fill the "PHP 版本" combo of the add-site form with the versions that are
// actually running — a php site pointing at a port nothing listens on answers
// every request with 502, so an idle version must not be offered here.
// Takes the parent explicitly: this runs from WM_CREATE, where g_main is still
// null (it is only assigned once CreateWindowExW returns).
static void refreshPhpVerCombo(HWND parent) {
    HWND c = GetDlgItem(parent, IDC_NG_PHPVER);
    if (!c) return;
    std::wstring keep;
    int cur = (int)SendMessageW(c, CB_GETCURSEL, 0, 0);
    if (cur != CB_ERR) {
        // Ask for the length first: CB_GETLBTEXT copies what the caller claims
        // to have and never checks (the P0-2 bug), and the version text is
        // built from a directory name on disk, which can be arbitrarily long.
        int len = (int)SendMessageW(c, CB_GETLBTEXTLEN, (WPARAM)cur, 0);
        if (len >= 0 && len < 4096) {
            std::vector<wchar_t> buf((size_t)len + 1);
            SendMessageW(c, CB_GETLBTEXT, (WPARAM)cur, (LPARAM)buf.data());
            keep = buf.data();
        }
    }
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    int n = 0;
    for (const PhpInstance& pi : phpListPool()) {
        if (!pi.running) continue;
        std::wstring item = pi.version + L"（端口 " + pi.port + L"）";
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)item.c_str());
        ++n;
    }
    if (n == 0) {
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"（没有正在运行的 PHP）");
        SendMessageW(c, CB_SETCURSEL, 0, 0);
        return;
    }
    if (!keep.empty()) SendMessageW(c, CB_SELECTSTRING, (WPARAM)-1, (LPARAM)keep.c_str());
    if (SendMessageW(c, CB_GETCURSEL, 0, 0) == CB_ERR) SendMessageW(c, CB_SETCURSEL, 0, 0);
}

// Show the PHP version row only while the kind combo says PHP, and keep it in
// sync with what is actually running. Returns true when the site is a php site.
static bool siteFormIsPhp(HWND hwnd) {
    HWND c = GetDlgItem(hwnd, IDC_NG_KIND);
    bool isPhp = c && SendMessageW(c, CB_GETCURSEL, 0, 0) == 1;
    for (int id : { IDC_NG_LABEL_PHPVER, IDC_NG_PHPVER }) {
        HWND h = GetDlgItem(hwnd, id);
        if (h) ShowWindow(h, isPhp ? SW_SHOW : SW_HIDE);
    }
    return isPhp;
}

static void initPgPage(HWND parent) {
    CompUI& ui = g_ui[(int)Comp::Postgresql];
    // 32px pitch, not the 34px used elsewhere: this column needs six rows and
    // the log edit below starts at y=280. The last button ends at 274.
    HWND bInit = makeCtl(IDC_PG_BTN_INIT, L"BUTTON", L"初始化数据库", BS_PUSHBUTTON, 20, 88, 120, 26, parent);
    HWND bPwd = makeCtl(IDC_PG_BTN_PWD, L"BUTTON", L"修改密码", BS_PUSHBUTTON, 20, 120, 120, 26, parent);
    HWND bAddU = makeCtl(IDC_PG_BTN_ADDUSER, L"BUTTON", L"创建用户", BS_PUSHBUTTON, 20, 152, 120, 26, parent);
    HWND bDelU = makeCtl(IDC_PG_BTN_DELUSER, L"BUTTON", L"删除用户", BS_PUSHBUTTON, 20, 184, 120, 26, parent);
    HWND bBak = makeCtl(IDC_PG_BTN_BACKUP, L"BUTTON", L"备份数据库", BS_PUSHBUTTON, 20, 216, 120, 26, parent);
    // Stacked under 备份 rather than beside it: the right column is the
    // username/password/port form and the info text, and a button there covered them.
    HWND bRes = makeCtl(IDC_PG_BTN_RESTORE, L"BUTTON", L"还原数据库", BS_PUSHBUTTON, 20, 248, 120, 26, parent);
    ui.pageControls.push_back(bInit);
    ui.pageControls.push_back(bPwd);
    ui.pageControls.push_back(bAddU);
    ui.pageControls.push_back(bDelU);
    ui.pageControls.push_back(bBak);
    ui.pageControls.push_back(bRes);
    addTooltip(parent, bRes, L"用 backup\\ 下的 pg_dumpall 备份覆盖当前数据库。可选择先初始化（重建数据目录）。危险操作，会二次确认");

    HWND lUser = makeCtl(IDC_PG_LABEL_USER, L"STATIC", L"用户名:", SS_LEFT, 160, 90, 60, 20, parent);
    HWND eUser = makeCtl(IDC_PG_USER, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
                         CBS_DROPDOWN | WS_VSCROLL, 225, 88, 170, 200, parent);
    HWND lPwd = makeCtl(IDC_PG_LABEL_PWD, L"STATIC", L"密码:", SS_LEFT, 160, 118, 60, 20, parent);
    HWND ePwd = makeCtl(IDC_PG_PWD, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_PASSWORD, 225, 116, 170, 22, parent);
    HWND lPort = makeCtl(IDC_PG_LABEL_PORT, L"STATIC", L"端口:", SS_LEFT, 160, 146, 60, 20, parent);
    HWND ePort = makeCtl(IDC_PG_PORT, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, 225, 144, 170, 22, parent);
    ui.pageControls.push_back(lUser);
    ui.pageControls.push_back(lPwd);
    ui.pageControls.push_back(lPort);
    ui.pageControls.push_back(eUser);
    ui.pageControls.push_back(ePwd);
    ui.pageControls.push_back(ePort);

    // pre-fill with current settings (password left blank: never round-trip
    // the stored secret through the UI; the user types it when operating)
    SetWindowTextW(eUser, pgUser().c_str());
    SetWindowTextW(ePort, pgPort().c_str());
    SetWindowTextW(ePwd, L"");

    HWND info = makeCtl(IDC_PG_INFO, L"STATIC", L"", SS_LEFT, 160, 178, 300, 90, parent);
    ui.pageControls.push_back(info);
}

static void initRedisPage(HWND parent) {
    CompUI& ui = g_ui[(int)Comp::Redis];
    HWND info = makeCtl(IDC_REDIS_INFO, L"STATIC", L"", SS_LEFT, 20, 90, 400, 160, parent);
    ui.pageControls.push_back(info);
}

static void initNodePage(HWND parent) {
    CompUI& ui = g_ui[(int)Comp::Nodejs];
    HWND lv = makeCtl(IDC_NODE_PM2_LIST, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
                      LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                      20, 82, 660, 120, parent);
    ui.pageControls.push_back(lv);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW col = {0};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 45;  col.pszText = (LPWSTR)L"ID"; ListView_InsertColumn(lv, 0, &col);
    col.cx = 150; col.pszText = (LPWSTR)L"名称"; ListView_InsertColumn(lv, 1, &col);
    col.cx = 200; col.pszText = (LPWSTR)L"状态"; ListView_InsertColumn(lv, 2, &col);
    col.cx = 70;  col.pszText = (LPWSTR)L"PID"; ListView_InsertColumn(lv, 3, &col);
    col.cx = 60;  col.pszText = (LPWSTR)L"存活"; ListView_InsertColumn(lv, 4, &col);
    col.cx = 60;  col.pszText = (LPWSTR)L"僵死"; ListView_InsertColumn(lv, 5, &col);
    col.cx = 75;  col.pszText = (LPWSTR)L"重启次数"; ListView_InsertColumn(lv, 6, &col);
    HWND bRefresh = makeCtl(IDC_NODE_BTN_REFRESH, L"BUTTON", L"刷新", BS_PUSHBUTTON, 20, 216, 100, 26, parent);
    HWND bRestartAll = makeCtl(IDC_NODE_BTN_RESTART_ALL, L"BUTTON", L"全部重启", BS_PUSHBUTTON, 130, 216, 100, 26, parent);
    HWND bDelete = makeCtl(IDC_NODE_BTN_DELETE, L"BUTTON", L"删除选中", BS_PUSHBUTTON, 240, 216, 100, 26, parent);
    ui.pageControls.push_back(bRefresh);
    ui.pageControls.push_back(bRestartAll);
    ui.pageControls.push_back(bDelete);
}

static void initPhpPage(HWND parent) {
    CompUI& ui = g_ui[(int)Comp::Php];
    // The common chrome above (one status dot, one version combo, one 启动) can
    // only express a single current version, which is not how PHP works here:
    // every installed version runs at once on its own FastCGI port. The list is
    // therefore the real view of the component, and the common 启动/停止 act on
    // all versions while the combo only picks the default for new sites.
    HWND lv = makeCtl(IDC_PHP_POOL_LIST, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
                      LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                      20, 82, 660, 120, parent);
    ui.pageControls.push_back(lv);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW col = {0};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 90;  col.pszText = (LPWSTR)L"版本"; ListView_InsertColumn(lv, 0, &col);
    col.cx = 60;  col.pszText = (LPWSTR)L"进程数"; ListView_InsertColumn(lv, 1, &col);
    col.cx = 110; col.pszText = (LPWSTR)L"FastCGI 端口"; ListView_InsertColumn(lv, 2, &col);
    col.cx = 100; col.pszText = (LPWSTR)L"状态"; ListView_InsertColumn(lv, 3, &col);
    col.cx = 100; col.pszText = (LPWSTR)L"默认版本"; ListView_InsertColumn(lv, 4, &col);
    col.cx = 200; col.pszText = (LPWSTR)L"配置文件"; ListView_InsertColumn(lv, 5, &col);

    HWND bStart = makeCtl(IDC_PHP_BTN_START, L"BUTTON", L"启动选中", BS_PUSHBUTTON, 20, 216, 90, 26, parent);
    HWND bStop  = makeCtl(IDC_PHP_BTN_STOP,  L"BUTTON", L"停止选中", BS_PUSHBUTTON, 118, 216, 90, 26, parent);
    HWND bDef   = makeCtl(IDC_PHP_BTN_DEFAULT, L"BUTTON", L"设为默认", BS_PUSHBUTTON, 216, 216, 90, 26, parent);
    HWND bExt   = makeCtl(IDC_PHP_BTN_EXT,   L"BUTTON", L"扩展管理", BS_PUSHBUTTON, 314, 216, 90, 26, parent);
    ui.pageControls.push_back(bStart);
    ui.pageControls.push_back(bStop);
    ui.pageControls.push_back(bDef);
    ui.pageControls.push_back(bExt);
    addTooltip(parent, bDef, L"只影响新建 PHP 站点默认选中哪个版本；所有已安装版本始终一起运行");
    addTooltip(parent, bExt, L"勾选要启用的扩展，写入该版本的 php.ini 并重启它");

    // Worker count. Windows has no PHP-FPM, so this number is the whole
    // concurrency story for a version: N php-cgi processes on N consecutive
    // ports, round-robined by an nginx upstream.
    HWND lWorkers = makeCtl(IDC_PHP_LABEL_WORKERS, L"STATIC", L"选中版本进程数:", SS_LEFT, 420, 222, 110, 20, parent);
    HWND eWorkers = makeCtl(IDC_PHP_WORKERS, L"EDIT", L"1", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, 534, 220, 50, 22, parent);
    ui.pageControls.push_back(lWorkers);
    ui.pageControls.push_back(eWorkers);
    addTooltip(parent, eWorkers, L"1-32。修改后点「启动选中」按新进程数重启该版本；站点配置会自动改指向对应的 upstream");

    HWND info = makeCtl(IDC_PHP_INFO, L"STATIC", L"", SS_LEFT, 20, 250, 660, 24, parent);
    ui.pageControls.push_back(info);
    // One honest sentence about the platform limit, so nobody reads a green
    // status light as "this scales".
    SetWindowTextW(info,
        L"Windows 无 PHP-FPM：单个 php-cgi 串行处理请求。进程数 >1 时 nginx 用 upstream 轮询多个端口。");
}

static void initCommonControls(HWND parent, Comp c) {
    CompUI& ui = g_ui[(int)c];
    ui.pageControls.clear();
    // status row (below the 30px tab strip)
    HWND dot = makeCtl(IDC_DOT, DOT_CLASS, L"", WS_CHILD | WS_VISIBLE, 14, 42, 16, 16, parent);
    HWND st = makeCtl(IDC_STATUS_TXT, L"STATIC", L"", SS_LEFT, 34, 40, 100, 20, parent);
    makeCtl(0, L"STATIC", L"版本:", SS_LEFT, 130, 40, 40, 20, parent);
    HWND combo = makeCtl(IDC_VER_COMBO, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
                         CBS_DROPDOWNLIST | WS_VSCROLL, 175, 38, 140, 200, parent);
    HWND bSwitch = makeCtl(IDC_BTN_SWITCH, L"BUTTON", L"切换版本", BS_PUSHBUTTON, 325, 38, 80, 24, parent);
    HWND bStart = makeCtl(IDC_BTN_START, L"BUTTON", L"启动", BS_PUSHBUTTON, 415, 38, 70, 24, parent);
    HWND bStop = makeCtl(IDC_BTN_STOP, L"BUTTON", L"停止", BS_PUSHBUTTON, 490, 38, 70, 24, parent);
    HWND bCfg = makeCtl(IDC_BTN_CFG, L"BUTTON", L"配置", BS_PUSHBUTTON, 570, 38, 70, 24, parent);
    HWND bData = makeCtl(IDC_BTN_DATA, L"BUTTON", L"数据目录", BS_PUSHBUTTON, 645, 38, 80, 24, parent);
    // Log 280..470 with the clear button at 476..500, so the taller window is
    // used instead of leaving a band of dead space under every component page.
    HWND bClearLog = makeCtl(IDC_BTN_CLEAR_LOG, L"BUTTON", L"清空日志", BS_PUSHBUTTON, 620, 476, 90, 24, parent);
    HWND log = makeCtl(IDC_LOG, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                       ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, 10, 280, 700, 190, parent);
    SendMessageW(log, WM_SETFONT, (WPARAM)g_monoFont, TRUE);

    ui.dot = dot; ui.statusTxt = st; ui.verCombo = combo;
    ui.btnSwitch = bSwitch; ui.btnStart = bStart; ui.btnStop = bStop;
    ui.btnCfg = bCfg; ui.btnData = bData; ui.logEdit = log;

    ui.pageControls.push_back(dot);
    ui.pageControls.push_back(st);
    ui.pageControls.push_back(combo);
    ui.pageControls.push_back(bSwitch);
    ui.pageControls.push_back(bStart);
    ui.pageControls.push_back(bStop);
    ui.pageControls.push_back(bCfg);
    ui.pageControls.push_back(bData);
    ui.pageControls.push_back(bClearLog);
    ui.pageControls.push_back(log);
}

// ---- inline field helpers (values come from the pg page, not popups) ----

static std::wstring pgEditText(int id) {
    wchar_t buf[512];
    GetDlgItemTextW(g_main, id, buf, 512);
    return std::wstring(buf);
}

static void pgOpInit() {
    std::wstring user = pgEditText(IDC_PG_USER);
    if (user.empty()) user = L"postgres";
    std::wstring pwd = pgEditText(IDC_PG_PWD);
    std::wstring port = pgEditText(IDC_PG_PORT);
    if (port.empty()) port = L"5432";
    // Quick probe: compStatus() may spawn helper processes (redis-cli / pm2) and
    // this runs on the UI thread, where a synchronous capture freezes the window.
    ComponentStatus st = compStatusQuick(Comp::Postgresql);
    runAsync(Comp::Postgresql, L"初始化数据库", [st, user, pwd, port](std::wstring& err) {
        return pgInit(Comp::Postgresql, st.currentVersion, user, pwd, port, err);
    });
}

static void pgOpPwd() {
    std::wstring user = pgEditText(IDC_PG_USER);
    if (user.empty()) user = pgUser();
    std::wstring pwd = pgEditText(IDC_PG_PWD);
    if (pwd.empty()) { logAppend(Comp::Postgresql, L"请填写新密码"); return; }
    runAsync(Comp::Postgresql, L"修改密码", [user, pwd](std::wstring& err) {
        return pgChangePassword(Comp::Postgresql, user, pwd, err);
    });
}

static void pgOpAddUser() {
    std::wstring user = pgEditText(IDC_PG_USER);
    if (user.empty()) { logAppend(Comp::Postgresql, L"请填写用户名"); return; }
    std::wstring pwd = pgEditText(IDC_PG_PWD);
    if (pwd.empty()) { logAppend(Comp::Postgresql, L"请填写密码"); return; }
    runAsync(Comp::Postgresql, L"创建用户", [user, pwd](std::wstring& err) {
        return pgCreateUser(Comp::Postgresql, user, pwd, err);
    });
}

static void pgOpDelUser() {
    std::wstring user = pgEditText(IDC_PG_USER);
    if (user.empty()) { logAppend(Comp::Postgresql, L"请填写用户名"); return; }
    runAsync(Comp::Postgresql, L"删除用户", [user](std::wstring& err) {
        return pgDropUser(Comp::Postgresql, user, err);
    });
}

static void pgOpBackup() {
    runAsync(Comp::Postgresql, L"备份数据库", [](std::wstring& err) {
        std::wstring file;
        bool ok = pgBackup(Comp::Postgresql, file, err);
        if (ok) err = L"已保存到 " + file;
        return ok;
    });
}

// ---- PostgreSQL restore dialog ----
// The dialog only collects intent (which dump, whether to rebuild first).
// pgOpRestore owns the confirmation prompts and runs the work off-thread — a
// restore of a multi-hundred-MB dump takes minutes and must not block the UI.

enum { IDC_PGR_FILE = 970, IDC_PGR_REINIT = 971, IDC_PGR_INFO = 972,
       IDC_PGR_GO = 973, IDC_PGR_CANCEL = 974 };

struct PgRestoreState {
    std::vector<std::wstring> files;   // backup\*.sql, newest first
    std::wstring file;                 // full path; empty means the user backed out
    bool reinit = false;
};

static std::wstring fileStamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return L"";
    FILETIME ft = {};
    SYSTEMTIME st;
    if (!d.ftLastWriteTime.dwHighDateTime && !d.ftLastWriteTime.dwLowDateTime) return L"";
    ft.dwLowDateTime = d.ftLastWriteTime.dwLowDateTime;
    ft.dwHighDateTime = d.ftLastWriteTime.dwHighDateTime;
    if (!FileTimeToLocalFileTime(&ft, &ft) || !FileTimeToSystemTime(&ft, &st)) return L"";
    return wstrfmt(L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

static std::wstring humanSize(LONGLONG bytes) {
    if (bytes >= 1024 * 1024 * 1024)
        return wstrfmt(L"%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    if (bytes >= 1024 * 1024)
        return wstrfmt(L"%.1f MB", (double)bytes / (1024.0 * 1024.0));
    return wstrfmt(L"%lld KB", (LONGLONG)(bytes / 1024));
}

static void pgrRefreshInfo(HWND hwnd, PgRestoreState& st) {
    HWND combo = GetDlgItem(hwnd, IDC_PGR_FILE);
    int idx = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    HWND info = GetDlgItem(hwnd, IDC_PGR_INFO);
    if (idx < 0 || idx >= (int)st.files.size()) {
        SetWindowTextW(info, L"请选择要还原的备份文件");
        return;
    }
    std::wstring full = joinPath(backupDir(), st.files[idx]);
    WIN32_FILE_ATTRIBUTE_DATA d = {};
    std::wstring size = L"";
    if (GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &d))
        size = humanSize(((LONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow);
    std::wstring txt = L"文件: " + st.files[idx] + L"\r\n" +
                       L"时间: " + fileStamp(full) + L"    大小: " + size;
    if (SendMessageW(GetDlgItem(hwnd, IDC_PGR_REINIT), BM_GETCHECK, 0, 0) == BST_CHECKED) {
        txt += L"\r\n\r\n⚠ 将重建数据目录：当前所有数据库与角色都会被删除，";
        txt += L"且原数据目录会被移到一边（不删除），还原失败可自动回滚。";
    } else {
        txt += L"\r\n\r\n直接覆盖同名库。dump 里已存在的角色/数据库会跳过，";
        txt += L"还原不会删除备份中不存在的库。";
    }
    SetWindowTextW(info, txt.c_str());
}

static LRESULT CALLBACK PgRestoreProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PgRestoreState* st = (PgRestoreState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        // The caller owns PgRestoreState and outlives the dialog, so it arrives
        // through lpCreateParams. WM_DESTROY must not free it.
        case WM_NCCREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return TRUE;
        }
        case WM_CREATE: {
            if (!st) return -1;
            st->files = pgListBackups();
            makeCtl(1000, L"STATIC", L"备份文件:", SS_LEFT, 16, 14, 80, 20, hwnd);
            HWND combo = makeCtl(IDC_PGR_FILE, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
                                 CBS_DROPDOWNLIST | WS_VSCROLL, 100, 12, 440, 240, hwnd);
            for (auto& f : st->files) {
                std::wstring label = fileStamp(joinPath(backupDir(), f)) + L"   " + f;
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)label.c_str());
            }
            if (st->files.empty()) {
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"（backup\\ 下没有 .sql 备份）");
                EnableWindow(combo, FALSE);
                EnableWindow(GetDlgItem(hwnd, IDC_PGR_GO), FALSE);
            } else {
                SendMessageW(combo, CB_SETCURSEL, 0, 0);
            }
            makeCtl(IDC_PGR_REINIT, L"BUTTON",
                    L"还原前初始化（重建数据目录，清空当前所有业务库与角色）",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 16, 268, 520, 22, hwnd);
            makeCtl(IDC_PGR_INFO, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 296, 530, 90, hwnd);
            makeCtl(IDC_PGR_GO, L"BUTTON", L"下一步…", BS_PUSHBUTTON, 360, 396, 100, 28, hwnd);
            makeCtl(IDC_PGR_CANCEL, L"BUTTON", L"取消", BS_PUSHBUTTON, 470, 396, 76, 28, hwnd);
            pgrRefreshInfo(hwnd, *st);
            return 0;
        }
        case WM_COMMAND: {
            if (!st) break;
            int id = LOWORD(wParam);
            if (id == IDC_PGR_CANCEL) { DestroyWindow(hwnd); return 0; }
            if (id == IDC_PGR_REINIT) {
                st->reinit = SendMessageW((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED;
                pgrRefreshInfo(hwnd, *st);
                return 0;
            }
            if (id == IDC_PGR_GO) {
                int idx = (int)SendMessageW(GetDlgItem(hwnd, IDC_PGR_FILE), CB_GETCURSEL, 0, 0);
                if (idx < 0 || idx >= (int)st->files.size()) {
                    MessageBoxW(hwnd, L"请先选择要还原的备份文件。", L"还原数据库",
                                MB_OK | MB_ICONWARNING);
                    return 0;
                }
                st->file = joinPath(backupDir(), st->files[idx]);
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void pgOpRestore() {
    if (iniGet(L"ver.postgresql", L"").empty()) {
        logAppend(Comp::Postgresql, L"未选择 PostgreSQL 版本，无法还原");
        return;
    }
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = PgRestoreProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"LNPPPgRestore";
        RegisterClassW(&wc);
        reg = true;
    }
    // A restore replaces the whole cluster. Say so before even opening the
    // dialog, and again below if the rebuild option is ticked.
    if (MessageBoxW(g_main,
            L"「还原数据库」会用 backup\\ 下的备份覆盖当前的 PostgreSQL 数据库。\r\n\r\n"
            L"被覆盖的数据不会留在数据库里。\r\n继续打开还原对话框？",
            L"还原数据库", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;

    PgRestoreState st;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"LNPPPgRestore", L"还原数据库",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX,
                               0, 0, 562, 462, g_main, nullptr, GetModuleHandleW(nullptr), &st);
    if (!dlg) return;
    centerOn(dlg, g_main);
    modal_loop(dlg, g_main);
    DestroyWindow(dlg);

    if (st.file.empty()) return;   // backed out

    // Second, sharper confirmation when the cluster is about to be rebuilt.
    if (st.reinit) {
        std::wstring msg =
            L"即将重建数据目录并还原：\r\n\r\n"
            L"  备份文件: " + st.file + L"\r\n"
            L"  目标版本: " + iniGet(L"ver.postgresql", L"") + L"\r\n\r\n"
            L"当前所有数据库与角色都会被删除。此操作不可撤销。\r\n"
            L"（原数据目录会移到 .before-restore-<时间戳> 保留，不删除）\r\n\r\n"
            L"确认继续？";
        if (MessageBoxW(g_main, msg.c_str(), L"危险操作：确认重建数据目录？",
                        MB_YESNO | MB_ICONERROR | MB_DEFBUTTON2) != IDYES)
            return;
    }

    std::wstring file = st.file;
    bool reinit = st.reinit;
    std::wstring name = reinit ? L"初始化并还原数据库" : L"还原数据库";
    runAsync(Comp::Postgresql, name, [file, reinit](std::wstring& err) {
        return pgRestoreBackup(Comp::Postgresql, file, reinit, err);
    });
}


// load the pg user list in the background, then refresh the user combo
static void refreshPgUsers() {
    if (g_pgUsersBusy.exchange(true)) return;
    std::thread([]() {
        std::vector<std::wstring> users;
        std::wstring err;
        if (pgListUsers(users, err)) {
            std::lock_guard<std::mutex> lk(g_snapMtx);
            g_pgUsers = users;
            PostMessageW(g_main, WM_PG_USERS, 0, 0);
        }
        g_pgUsersBusy = false;
    }).detach();
}

// reset the nginx "add site" form after a successful add
static void clearNginxAddForm() {
    SetWindowTextW(GetDlgItem(g_main, IDC_NG_ADD_NAME), L"");
    SetWindowTextW(GetDlgItem(g_main, IDC_NG_ADD_DOMAIN), L"");
    SetWindowTextW(GetDlgItem(g_main, IDC_NG_ADD_PORT), L"80");
    SetWindowTextW(GetDlgItem(g_main, IDC_NG_ROOT), L"");
    SetWindowTextW(GetDlgItem(g_main, IDC_NG_CERT), L"");
    SetWindowTextW(GetDlgItem(g_main, IDC_NG_KEY), L"");
    SendMessageW(GetDlgItem(g_main, IDC_NG_SSL), BM_SETCHECK, BST_UNCHECKED, 0);
    for (int ctl : { IDC_NG_LABEL_CERT, IDC_NG_CERT, IDC_NG_BTN_CERT,
                     IDC_NG_LABEL_KEY, IDC_NG_KEY, IDC_NG_BTN_KEY }) {
        HWND c = GetDlgItem(g_main, ctl);
        if (c) ShowWindow(c, SW_HIDE);
    }
}

// ============================ About dialog ============================

static std::wstring appVersion() {
    static const std::wstring cached = []() {
    wchar_t exe[MAX_PATH];
    DWORD pathLen = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (pathLen == 0 || pathLen >= MAX_PATH) return std::wstring(L"1.0.0.0");
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(exe, &handle);
    if (size > 0) {
        std::vector<BYTE> data(size);
        if (GetFileVersionInfoW(exe, 0, size, data.data())) {
            VS_FIXEDFILEINFO* fi = nullptr;
            UINT len = 0;
            if (VerQueryValueW(data.data(), L"\\", (LPVOID*)&fi, &len) &&
                fi && len >= sizeof(VS_FIXEDFILEINFO) &&
                fi->dwSignature == VS_FFI_SIGNATURE) {
                return std::to_wstring(HIWORD(fi->dwFileVersionMS)) + L"." +
                       std::to_wstring(LOWORD(fi->dwFileVersionMS)) + L"." +
                       std::to_wstring(HIWORD(fi->dwFileVersionLS)) + L"." +
                       std::to_wstring(LOWORD(fi->dwFileVersionLS));
            }
        }
    }
    return std::wstring(L"1.0.0.0");
    }();
    return cached;
}

static void modal_loop(HWND dlg, HWND owner) {
    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    MSG msg;
    bool parentQuitting = false;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_QUIT) {
            // Don't re-post: the parent message loop owns the quit signal.
            // Remember it so we can hand control back to wWinMain instead
            // of swallowing the quit forever.
            parentQuitting = true;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (!IsWindow(dlg)) break;
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (parentQuitting) PostQuitMessage((int)msg.wParam);
}

static void centerOn(HWND dlg, HWND owner) {
    RECT orc, drc;
    GetWindowRect(owner, &orc);
    GetWindowRect(dlg, &drc);
    int w = drc.right - drc.left, h = drc.bottom - drc.top;
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - h) / 2;
    SetWindowPos(dlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// About dialog control ids (local range)
enum { IDC_AB_LINK = 950, IDC_AB_OK = 951 };

static LRESULT CALLBACK AboutProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            HWND t = CreateWindowExW(0, L"STATIC", L"LNPP 组件管理器", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     20, 22, 300, 24, hwnd, (HMENU)0, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(t, WM_SETFONT, (WPARAM)g_font, TRUE);
            std::wstring ver = L"版本: v" + appVersion();
            t = CreateWindowExW(0, L"STATIC", ver.c_str(), WS_CHILD | WS_VISIBLE,
                                20, 54, 300, 20, hwnd, (HMENU)0, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(t, WM_SETFONT, (WPARAM)g_font, TRUE);
            std::wstring ab = L"作者: arbog";
            t = CreateWindowExW(0, L"STATIC", ab.c_str(), WS_CHILD | WS_VISIBLE,
                                20, 80, 300, 20, hwnd, (HMENU)0, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(t, WM_SETFONT, (WPARAM)g_font, TRUE);
            // clickable GitHub link
            t = CreateWindowExW(0, L"STATIC", L"GitHub: https://github.com/arbog2/lnpp",
                                WS_CHILD | WS_VISIBLE | SS_NOTIFY,
                                20, 106, 300, 20, hwnd, (HMENU)IDC_AB_LINK, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(t, WM_SETFONT, (WPARAM)g_linkFont, TRUE);
            // OK
            HWND ok = CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      130, 148, 90, 28, hwnd, (HMENU)IDC_AB_OK, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(ok, WM_SETFONT, (WPARAM)g_font, TRUE);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            int id = GetDlgCtrlID((HWND)lParam);
            if (id == IDC_AB_LINK) {
                SetTextColor((HDC)wParam, RGB(0, 102, 204));
                SetBkMode((HDC)wParam, TRANSPARENT);
                return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }
            break;
        }
        case WM_SETCURSOR: {
            if ((HWND)wParam == GetDlgItem(hwnd, IDC_AB_LINK)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == IDC_AB_LINK && HIWORD(wParam) == STN_CLICKED) {
                ShellExecuteW(hwnd, L"open", L"https://github.com/arbog2/lnpp", nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            if (LOWORD(wParam) == IDC_AB_OK) { DestroyWindow(hwnd); return 0; }
            break;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void showAboutDialog(HWND owner) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = AboutProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"LNPPAbout";
        RegisterClassW(&wc);
        reg = true;
    }
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"LNPPAbout", L"关于",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, 360, 220,
                               owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!dlg) return;
    centerOn(dlg, owner);
    modal_loop(dlg, owner);
    DestroyWindow(dlg);
}

// ============================ PHP extension manager ============================
//
// Checkbox list over one version's ext\ directory. The rows are rendered into
// the list's text column with a [x]/[ ] prefix rather than using a state image
// list: it keeps the dialog to one image list (the shared one the dot control
// already creates) and the "missing dll" case is visible in the same row.

struct PhpExtState {
    std::wstring version;
    std::vector<PhpExtension> exts;
    std::vector<bool> want;
    HWND list = nullptr, info = nullptr;
    bool applied = false;
};

static std::wstring phpExtRowText(const PhpExtension& e, bool on) {
    std::wstring s = on ? L"[x] " : L"[ ] ";
    s += e.name;
    if (!e.loaded) s += L"   ← 缺少 " + e.dll + L"（该版本未附带）";
    return s;
}

static void phpExtRefresh(HWND hwnd) {
    PhpExtState* st = (PhpExtState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!st || !st->list) return;
    int prev = ListView_GetNextItem(st->list, -1, LVNI_SELECTED);
    ListView_DeleteAllItems(st->list);
    for (size_t i = 0; i < st->exts.size(); ++i) {
        LVITEMW item = {0};
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        std::wstring row = phpExtRowText(st->exts[i], st->want[i]);
        item.pszText = (LPWSTR)row.c_str();
        int idx = ListView_InsertItem(st->list, &item);
        if (idx >= 0 && prev == (int)i)
            ListView_SetItemState(st->list, idx, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
    }
    int onCount = 0, missing = 0;
    for (size_t i = 0; i < st->exts.size(); ++i) {
        if (st->want[i]) ++onCount;
        if (!st->exts[i].loaded) ++missing;
    }
    std::wstring info = L"共 " + std::to_wstring(st->exts.size()) + L" 个扩展，已启用 " +
                        std::to_wstring(onCount) + L" 个";
    if (missing) info += L"；其中 " + std::to_wstring(missing) + L" 个缺少 dll（通常是 PECL 扩展，需自行安装）";
    info += L"。点「应用」写入 " + st->version + L" 的 php.ini，然后重启该版本生效。";
    SetWindowTextW(st->info, info.c_str());
}

static void phpExtToggle(HWND hwnd) {
    PhpExtState* st = (PhpExtState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!st || !st->list) return;
    int sel = ListView_GetNextItem(st->list, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= (int)st->exts.size()) return;
    st->want[sel] = !st->want[sel];
    phpExtRefresh(hwnd);
}

static LRESULT CALLBACK PhpExtProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // WM_CREATE hands us a CREATESTRUCT, not the pointer we passed as
            // lpCreateParams — reading lParam directly yields a wild pointer and
            // the very next store through it takes the whole process down.
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            PhpExtState* st = cs ? (PhpExtState*)cs->lpCreateParams : nullptr;
            if (!st) return -1;   // refuse to create rather than crash
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
            RECT rc; GetClientRect(hwnd, &rc);
            st->list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP |
                LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                10, 10, rc.right - 20, rc.bottom - 66, hwnd,
                (HMENU)(INT_PTR)IDC_PHPX_LIST, GetModuleHandleW(nullptr), nullptr);
            if (st->list) {
                ListView_SetExtendedListViewStyle(st->list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
                LVCOLUMNW col = {0};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.cx = rc.right - 40; col.pszText = (LPWSTR)L"扩展（点一行切换勾选）";
                ListView_InsertColumn(st->list, 0, &col);
            }
            st->info = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                10, rc.bottom - 52, rc.right - 20, 30, hwnd,
                (HMENU)(INT_PTR)IDC_PHPX_INFO, GetModuleHandleW(nullptr), nullptr);
            HWND bApply = CreateWindowExW(0, L"BUTTON", L"应用并重启", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                rc.right - 200, rc.bottom - 30, 90, 24, hwnd,
                (HMENU)(INT_PTR)IDC_PHPX_APPLY, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                rc.right - 100, rc.bottom - 30, 90, 24, hwnd,
                (HMENU)(INT_PTR)IDC_PHPX_CLOSE, GetModuleHandleW(nullptr), nullptr);
            SetFocus(bApply);
            phpExtRefresh(hwnd);
            return 0;
        }
        case WM_NOTIFY: {
            if (((LPNMHDR)lParam)->idFrom == IDC_PHPX_LIST &&
                ((LPNMHDR)lParam)->code == NM_CLICK) {
                phpExtToggle(hwnd);
                return 0;
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_PHPX_CLOSE) { DestroyWindow(hwnd); return 0; }
            if (id == IDC_PHPX_APPLY) {
                PhpExtState* st = (PhpExtState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
                if (!st) return 0;
                std::vector<std::wstring> wanted;
                for (size_t i = 0; i < st->exts.size(); ++i)
                    if (st->want[i]) wanted.push_back(st->exts[i].name);
                std::wstring ver = st->version;
                // Writing php.ini and restarting a php-cgi both take a moment
                // and must not run on the UI thread (see isUiThread's contract).
                runAsync(Comp::Php, L"应用 PHP " + ver + L" 扩展设置", [ver, wanted](std::wstring& err) {
                    int n = phpApplyExtensions(ver, wanted, err);
                    if (n < 0) return false;
                    // The ini is only read when php-cgi starts, so a live pool
                    // keeps the old extension set until it is restarted.
                    std::wstring serr;
                    if (phpRunning(ver)) {
                        phpStop(ver, serr);
                        if (!phpStart(ver, serr)) {
                            err = L"扩展已写入，但重启 PHP 失败: " + serr;
                            return false;
                        }
                    }
                    return true;
                });
                st->applied = true;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void showPhpExtDialog(HWND owner, const std::wstring& ver) {
    std::wstring err;
    std::vector<PhpExtension> exts = phpListExtensions(ver, err);
    if (exts.empty()) {
        MessageBoxW(owner, err.empty() ? L"该版本没有可用的扩展目录" : err.c_str(),
                    L"扩展管理", MB_OK | MB_ICONWARNING);
        return;
    }
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = PhpExtProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"LNPPPhpExt";
        RegisterClassW(&wc);
        reg = true;
    }
    // The state outlives the window: the apply path hands the version and the
    // wanted set to a worker thread by value before the dialog is destroyed.
    static PhpExtState st;
    st.version = ver;
    st.exts = exts;
    st.want.clear();
    st.want.reserve(exts.size());
    for (const PhpExtension& e : exts) st.want.push_back(e.enabled);
    st.applied = false;

    std::wstring title = L"PHP " + ver + L" 扩展管理";
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"LNPPPhpExt", title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, 520, 420,
                               owner, nullptr, GetModuleHandleW(nullptr), &st);
    if (!dlg) return;
    centerOn(dlg, owner);
    modal_loop(dlg, owner);
    DestroyWindow(dlg);
}

// ============================ Downloader dialog ============================

struct DlState {
    HWND list = nullptr, btnGo = nullptr, btnCancel = nullptr, btnClose = nullptr;
    HWND progress = nullptr, statusTxt = nullptr;
    std::vector<PkgItem> items;
    std::atomic<bool> busy{false};
    // shared_ptr so the download thread outlives the dialog: if the window is
    // destroyed mid-download (app quit), the worker still owns a live cancel
    // flag instead of dereferencing a freed DlState.
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    std::wstring stage;
    std::wstring curName;
};

// dl control ids (local range)
enum { IDC_DL_LIST = 960, IDC_DL_GO = 961, IDC_DL_CANCEL = 962, IDC_DL_CLOSE = 963 };

static void dlPopulate(DlState& st) {
    ListView_DeleteAllItems(st.list);
    st.items.clear();
    std::wstring err;
    auto secs = pkgsParseConf(err);
    if (secs.empty()) { SetWindowTextW(st.statusTxt, err.c_str()); return; }
    for (auto& s : secs) {
        for (auto& it : s.items) st.items.push_back(it);
    }
    for (size_t i = 0; i < st.items.size(); ++i) {
        auto& it = st.items[i];
        LVITEMW vi = {0};
        vi.mask = LVIF_TEXT;
        vi.iItem = (int)i;
        vi.pszText = (LPWSTR)it.comp.c_str();
        ListView_InsertItem(st.list, &vi);
        ListView_SetItemText(st.list, (int)i, 1, (LPWSTR)it.name.c_str());
        Comp c = it.comp == L"nginx" ? Comp::Nginx :
                 it.comp == L"nodejs" ? Comp::Nodejs :
                 it.comp == L"postgresql" ? Comp::Postgresql : Comp::Redis;
        std::wstring status = compVersionUsable(c, it.ver) ? L"已安装" : L"未安装";
        ListView_SetItemText(st.list, (int)i, 2, (LPWSTR)status.c_str());
    }
}

static DlState* dlState(HWND hwnd) { return (DlState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA); }

static void dlStart(HWND hwnd) {
    DlState* st = dlState(hwnd);
    if (!st || st->busy.load()) return;
    int sel = ListView_GetNextItem(st->list, -1, LVNI_SELECTED);
    if (sel < 0) { SetWindowTextW(st->statusTxt, L"请先选择要下载的组件"); return; }
    if (sel >= (int)st->items.size()) { SetWindowTextW(st->statusTxt, L"列表与数据不同步，请刷新后重试"); return; }
    PkgItem item = st->items[sel];
    std::wstring target = joinPath(binCompDir(item.comp), item.ver);
    if (dirExists(target)) { SetWindowTextW(st->statusTxt, L"该版本已安装"); return; }
    st->busy = true;
    st->cancel = std::make_shared<std::atomic<bool>>(false);
    st->curName = item.name;
    EnableWindow(st->btnGo, FALSE);
    EnableWindow(st->btnClose, FALSE);
    EnableWindow(st->btnCancel, TRUE);
    PkgItem copy = item;
    std::shared_ptr<std::atomic<bool>> cancelSp = st->cancel;
    std::thread([hwnd, copy, cancelSp]() {
        std::wstring err;
        // pkgsInstall reports progress once per received chunk — tens of
        // thousands of calls for a ~300 MB PostgreSQL zip. Posting two messages
        // and re-setting the status text per chunk floods the UI queue, so the
        // stage string is only allocated when the stage actually changes and
        // the progress line is throttled to a whole percent / 200 ms.
        struct ProgressState {
            std::wstring stage;
            int pct = -1;
            DWORD tick = 0;
            bool first = true;
        };
        auto pst = std::make_shared<ProgressState>();
        auto prog = [hwnd, pst](const std::wstring& stage, DWORD done, DWORD total) {
            int pct = total > 0 ? (int)((__int64)done * 100 / total) : -1;
            DWORD now = GetTickCount();
            bool stageChanged = (stage != pst->stage);
            if (!stageChanged && !pst->first) {
                if (pct >= 0 && pct == pst->pct) return;        // same percent
                if (now - pst->tick < 200) return;              // too soon
            }
            pst->stage = stage;
            pst->pct = pct;
            pst->tick = now;
            pst->first = false;
            if (!IsWindow(hwnd)) return;   // dialog gone: drop the update
            if (stageChanged) {
                wchar_t* s = _wcsdup(stage.c_str());
                if (!PostMessageW(hwnd, WM_DL_STAGE, 0, (LPARAM)s)) free(s);
            }
            PostMessageW(hwnd, WM_DL_PROGRESS, (WPARAM)done, (LPARAM)total);
        };
        bool ok = pkgsInstall(copy, prog, cancelSp.get(), err);
        if (!IsWindow(hwnd)) return;   // dialog gone: nothing to report
        wchar_t* e = _wcsdup(err.c_str());
        if (!PostMessageW(hwnd, WM_DL_DONE, ok ? 1 : 0, (LPARAM)e)) free(e);
    }).detach();
}

static LRESULT CALLBACK DownloaderProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DlState* st = dlState(hwnd);
    switch (msg) {
        case WM_CREATE: {
            st = new DlState();
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
            makeCtl(IDC_DL_LIST, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
                    LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 16, 12, 560, 300, hwnd);
            st->list = GetDlgItem(hwnd, IDC_DL_LIST);
            ListView_SetExtendedListViewStyle(st->list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            LVCOLUMNW col = {0};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.cx = 90;  col.pszText = (LPWSTR)L"组件"; ListView_InsertColumn(st->list, 0, &col);
            col.cx = 250; col.pszText = (LPWSTR)L"条目"; ListView_InsertColumn(st->list, 1, &col);
            col.cx = 80;  col.pszText = (LPWSTR)L"状态"; ListView_InsertColumn(st->list, 2, &col);

            st->progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER,
                                           16, 320, 420, 20, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(st->progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            st->statusTxt = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                            16, 348, 560, 20, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(st->statusTxt, WM_SETFONT, (WPARAM)g_font, TRUE);

            st->btnGo = makeCtl(IDC_DL_GO, L"BUTTON", L"下载", BS_PUSHBUTTON, 452, 316, 122, 28, hwnd);
            st->btnCancel = makeCtl(IDC_DL_CANCEL, L"BUTTON", L"取消", BS_PUSHBUTTON, 452, 352, 122, 28, hwnd);
            EnableWindow(st->btnCancel, FALSE);
            st->btnClose = makeCtl(IDC_DL_CLOSE, L"BUTTON", L"关闭", BS_PUSHBUTTON, 452, 388, 122, 28, hwnd);

            dlPopulate(*st);
            return 0;
        }
        case WM_SIZE: {
            if (st && st->list) {
                int w = LOWORD(lParam);
                MoveWindow(st->list, 16, 12, w - 32, 300, TRUE);
                MoveWindow(st->progress, 16, 320, w - 200, 20, TRUE);
                MoveWindow(st->statusTxt, 16, 348, w - 32, 20, TRUE);
            }
            break;
        }
        case WM_DL_STAGE: {
            std::wstring stage((const wchar_t*)lParam);
            free((void*)lParam);
            if (st) st->stage = stage;
            if (st) SetWindowTextW(st->statusTxt, (stage + L"  " + st->curName).c_str());
            return 0;
        }
        case WM_DL_PROGRESS: {
            if (st) {
                DWORD done = (DWORD)wParam;
                DWORD total = (DWORD)lParam;
                if (total > 0) {
                    // done is DWORD; done*100 overflows past ~42MB, so widen
                    // before multiplying (postgresql zips are ~300MB)
                    int pct = (int)((__int64)done * 100 / total);
                    SendMessageW(st->progress, PBM_SETPOS, pct, 0);
                    wchar_t buf[96];
                    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%s  %s  %u%%",
                                 st->stage.c_str(), st->curName.c_str(), pct);
                    SetWindowTextW(st->statusTxt, buf);
                } else {
                    int pct = (int)((done / 1024) % 100);
                    SendMessageW(st->progress, PBM_SETPOS, pct, 0);
                    wchar_t buf[96];
                    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%s  %s  %u KB",
                                 st->stage.c_str(), st->curName.c_str(), done / 1024);
                    SetWindowTextW(st->statusTxt, buf);
                }
            }
            return 0;
        }
        case WM_DL_DONE: {
            bool ok = (wParam != 0);
            std::wstring err((const wchar_t*)lParam);
            free((void*)lParam);
            if (st) {
                st->busy = false;
                EnableWindow(st->btnGo, TRUE);
                EnableWindow(st->btnClose, TRUE);
                EnableWindow(st->btnCancel, FALSE);
                if (ok) {
                    SetWindowTextW(st->statusTxt, L"安装完成");
                    dlPopulate(*st);
                    ovLogAppend(L"组件下载安装完成: " + st->curName);
                    // Refresh the version combos. This used to borrow WM_OP_DONE,
                    // whose job is to clear a component's busy flag — posting it
                    // here force-cleared all four components and re-enabled their
                    // buttons while a real start/stop was still running. Just ask
                    // the pollers for fresh data instead.
                    refreshOverview();
                    kickStatusPoll();
                    kickPm2Poll();
                } else {
                    SetWindowTextW(st->statusTxt, (L"失败: " + err).c_str());
                    ovLogAppend(L"组件下载失败: " + st->curName + L" (" + err + L")");
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDC_DL_GO: dlStart(hwnd); break;
                case IDC_DL_CANCEL:
                    if (st && st->cancel) { *st->cancel = true; SetWindowTextW(st->statusTxt, L"正在取消..."); }
                    break;
                case IDC_DL_CLOSE: {
                    if (st && st->busy.load()) { SetWindowTextW(st->statusTxt, L"正在下载，请先取消"); return 0; }
                    DestroyWindow(hwnd);
                    break;
                }
            }
            return 0;
        }
        case WM_CLOSE: {
            if (st && st->busy.load()) { SetWindowTextW(st->statusTxt, L"正在下载，请先取消"); return 0; }
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY: {
            if (st) { delete st; SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void showDownloaderDialog(HWND owner) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = DownloaderProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"LNPPDownloader";
        RegisterClassW(&wc);
        reg = true;
    }
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"LNPPDownloader", L"下载组件",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX,
                               0, 0, 610, 440, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!dlg) return;
    centerOn(dlg, owner);
    modal_loop(dlg, owner);
    DestroyWindow(dlg);
}

// The four component pages are built by initCommonControls() with the *same*
// control IDs (IDC_BTN_START / IDC_LOG / ...) under the same parent window, so
// GetDlgItem(g_main, ...) only ever finds the nginx copy. Resolve a WM_COMMAND
// sender back to its component instead of trusting the active tab: the tab
// happens to match today because the other pages are hidden, but any keyboard
// or programmatic notification would then act on the wrong component.
static Comp compFromControl(HWND ctl) {
    if (!ctl) return Comp::Count;
    for (int i = 0; i < (int)Comp::Count; ++i) {
        for (HWND h : g_ui[i].pageControls) {
            if (h == ctl) return (Comp)i;
        }
    }
    return Comp::Count;
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Must be the first statement: autoStartComponents() below spawns
            // workers that PostMessage back to this window as they finish, and
            // WM_CREATE runs while CreateWindowExW has not yet returned.
            g_main = hwnd;
            g_tab = makeCtl(IDC_TAB, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 720, 30, hwnd);
            TCITEMW ti = {0};
            ti.mask = TCIF_TEXT;
            ti.pszText = (LPWSTR)L"总览";
            TabCtrl_InsertItem(g_tab, TAB_OVERVIEW, &ti);
            ti.pszText = (LPWSTR)L"nginx";
            TabCtrl_InsertItem(g_tab, compToTab(Comp::Nginx), &ti);
            ti.pszText = (LPWSTR)L"PostgreSQL";
            TabCtrl_InsertItem(g_tab, compToTab(Comp::Postgresql), &ti);
            ti.pszText = (LPWSTR)L"Redis";
            TabCtrl_InsertItem(g_tab, compToTab(Comp::Redis), &ti);
            ti.pszText = (LPWSTR)L"Node.js";
            TabCtrl_InsertItem(g_tab, compToTab(Comp::Nodejs), &ti);
            ti.pszText = (LPWSTR)L"PHP";
            TabCtrl_InsertItem(g_tab, compToTab(Comp::Php), &ti);

            initOverviewPage(hwnd);
            initCommonControls(hwnd, Comp::Nginx);
            initNginxPage(hwnd);
            initCommonControls(hwnd, Comp::Postgresql);
            initPgPage(hwnd);
            initCommonControls(hwnd, Comp::Redis);
            initRedisPage(hwnd);
            initCommonControls(hwnd, Comp::Nodejs);
            initNodePage(hwnd);
            initCommonControls(hwnd, Comp::Php);
            initPhpPage(hwnd);

            showPage(TAB_OVERVIEW);
            kickStatusPoll();
            kickPm2Poll();
            SetTimer(hwnd, 1, 3000, pm2Timer);    // pm2 refresh
            SetTimer(hwnd, 2, 2000, statusTimer); // status poll
            SetTimer(hwnd, 3, 50, iniFlushTimer); // drain pending ini writes
            // autoStartComponents() is NOT called here: it spawns worker threads,
            // and doing that from inside WM_CREATE means they can finish and
            // PostMessage back before CreateWindowExW has returned. WinMain calls
            // it right after the window exists.
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int cur = TabCtrl_GetCurSel(g_tab);
            // Component for the shared per-page controls: the sending control
            // wins, the active tab is only the fallback (see compFromControl).
            Comp sender = compFromControl((HWND)lParam);
            if (sender == Comp::Count && cur >= TAB_COMP_BASE) sender = tabToComp(cur);
            switch (id) {
                case IDC_OV_AUTOSTART_ALL: ovAutoAll((HWND)lParam); break;
                case IDC_OV_BOOT_START: {
                    bool on = SendMessageW((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    bootStartSet(on);
                    ovLogAppend(on ? L"开机启动：已启用"
                                   : L"开机启动：已关闭");
                    break;
                }
                case IDC_OV_BTN_ALL_START: ovAllOp(AllOp::Start); break;
                case IDC_OV_BTN_ALL_RESTART: ovAllOp(AllOp::Restart); break;
                case IDC_OV_BTN_ALL_STOP: ovAllOp(AllOp::Stop); break;
                case IDC_OV_BTN_PATH_ADD: pathAddNode(); break;
                case IDC_OV_BTN_PATH_DEL: pathRemoveNode(); break;
                case IDC_OV_BTN_DL:
                    ovLogAppend(L"打开组件下载器");
                    showDownloaderDialog(hwnd);
                    break;
                case IDC_OV_ABOUT_LINK:
                    if (HIWORD(wParam) == STN_CLICKED) {
                        ovLogAppend(L"打开关于对话框");
                        showAboutDialog(hwnd);
                    }
                    break;
                case IDC_OV_COMP_START_BASE + 0: ovToggle(Comp::Nginx); break;
                case IDC_OV_COMP_START_BASE + 1: ovToggle(Comp::Postgresql); break;
                case IDC_OV_COMP_START_BASE + 2: ovToggle(Comp::Redis); break;
                case IDC_OV_COMP_START_BASE + 3: ovToggle(Comp::Nodejs); break;
                case IDC_OV_COMP_AUTO_BASE + 0: ovAutoComp(Comp::Nginx, (HWND)lParam); break;
                case IDC_OV_COMP_AUTO_BASE + 1: ovAutoComp(Comp::Postgresql, (HWND)lParam); break;
                case IDC_OV_COMP_AUTO_BASE + 2: ovAutoComp(Comp::Redis, (HWND)lParam); break;
                case IDC_OV_COMP_AUTO_BASE + 3: ovAutoComp(Comp::Nodejs, (HWND)lParam); break;
                case IDC_BTN_START:
                    if (sender != Comp::Count) actStart(sender);
                    break;
                case IDC_BTN_STOP:
                    if (sender != Comp::Count) actStop(sender);
                    break;
                case IDC_BTN_SWITCH:
                    if (sender != Comp::Count) actSwitch(sender);
                    break;
                case IDC_BTN_CFG: {
                    if (sender == Comp::Count) break;
                    // Opens the component's live configuration, not etc\:
                    // etc holds templates that are never read at runtime.
                    std::wstring dir = compConfigDir(sender);
                    if (!dirExists(dir) && !makeDirs(dir)) {
                        logAppend(sender, L"无法创建配置目录: " + dir);
                        break;
                    }
                    logAppend(sender, L"打开配置目录: " + dir);
                    if (sender == Comp::Nodejs)
                        logAppend(sender, L"（pm2 自己的 dump / 日志 / pid 在 %USERPROFILE%\\.pm2）");
                    // ShellExecuteW returns HINSTANCE; anything <= 32 is an error
                    // code rather than a handle.
                    if ((INT_PTR)ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr,
                                               SW_SHOWNORMAL) <= 32)
                        logAppend(sender, L"无法打开目录，请手动前往: " + dir);
                    break;
                }
                case IDC_BTN_DATA: {
                    if (sender == Comp::Count) break;
                    // Quick probe — this is a UI-thread handler and compStatus()
                    // can block for seconds behind a helper process.
                    ComponentStatus st = compStatusQuick(sender);
                    if (st.installed) {
                        std::wstring dir = compDataVerDir(sender, st.currentVersion);
                        makeDirs(dir);
                        ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                    break;
                }
                case IDC_BTN_CLEAR_LOG: {
                    if (sender == Comp::Count) break;
                    HWND log = g_ui[(int)sender].logEdit;
                    if (log) SetWindowTextW(log, L"");
                    break;
                }
                case IDC_OV_BTN_CLEAR_LOG:
                    if (g_ov.logEdit) SetWindowTextW(g_ov.logEdit, L"");
                    break;
                case IDC_NG_BTN_ADD: {
                    wchar_t name[256], domain[256], port[256], cert[1024], key[1024], root[1024];
                    GetDlgItemTextW(hwnd, IDC_NG_ADD_NAME, name, 256);
                    GetDlgItemTextW(hwnd, IDC_NG_ADD_DOMAIN, domain, 256);
                    GetDlgItemTextW(hwnd, IDC_NG_ADD_PORT, port, 256);
                    GetDlgItemTextW(hwnd, IDC_NG_ROOT, root, 1024);
                    bool ssl = SendMessageW(GetDlgItem(hwnd, IDC_NG_SSL), BM_GETCHECK, 0, 0) == BST_CHECKED;
                    GetDlgItemTextW(hwnd, IDC_NG_CERT, cert, 1024);
                    GetDlgItemTextW(hwnd, IDC_NG_KEY, key, 1024);
                    bool isPhp = siteFormIsPhp(hwnd);
                    std::wstring phpVer;
                    if (isPhp) {
                        // The combo text is "8.3（端口 9000）"; the version is the
                        // leading token before the bracket.
                        wchar_t buf[256] = {0};
                        GetDlgItemTextW(hwnd, IDC_NG_PHPVER, buf, 256);
                        std::wstring full = buf;
                        size_t br = full.find(L'（');
                        phpVer = trimStr(br == std::wstring::npos ? full : full.substr(0, br));
                        if (phpVer.empty() || phpVer.rfind(L"没有", 0) == 0) {
                            MessageBoxW(hwnd, L"没有正在运行的 PHP 版本。\n\n"
                                            L"请先到「PHP」页签启动至少一个版本，再来添加 PHP 站点。",
                                        L"无法添加站点", MB_OK | MB_ICONWARNING);
                            break;
                        }
                    }
                    std::wstring nm = name, dm = domain, pt = port, ct = cert, ky = key, rt = root;
                    SiteKind kind = isPhp ? SiteKind::Php : SiteKind::Node;
                    g_pendingVhostClear = true;
                    runAsync(Comp::Nginx, L"添加虚拟站点 " + nm,
                             [nm, dm, pt, ssl, ct, ky, rt, kind, phpVer](std::wstring& err) {
                        return nginxAddVHostEx(nm, dm, pt, ssl, ct, ky, rt, kind, phpVer, err);
                    });
                    break;
                }
                case IDC_NG_KIND: {
                    if (HIWORD(wParam) == CBN_SELCHANGE) siteFormIsPhp(hwnd);
                    break;
                }
                case IDC_NG_BTN_ROOT: {
                    BROWSEINFOW bi = {0};
                    bi.hwndOwner = hwnd;
                    bi.lpszTitle = L"选择站点根目录";
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                    if (pidl) {
                        wchar_t path[MAX_PATH];
                        if (SHGetPathFromIDListW(pidl, path)) {
                            SetWindowTextW(GetDlgItem(hwnd, IDC_NG_ROOT), path);
                        }
                        CoTaskMemFree(pidl);
                    }
                    break;
                }
                case IDC_NG_SSL: {
                    bool on = SendMessageW((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    // default port: 80 for http, 443 for https (only if untouched)
                    HWND portCtl = GetDlgItem(hwnd, IDC_NG_ADD_PORT);
                    wchar_t pbuf[32];
                    GetWindowTextW(portCtl, pbuf, 32);
                    std::wstring portText = pbuf;
                    if (on && portText == L"80") SetWindowTextW(portCtl, L"443");
                    else if (!on && portText == L"443") SetWindowTextW(portCtl, L"80");
                    for (int ctl : { IDC_NG_LABEL_CERT, IDC_NG_CERT, IDC_NG_BTN_CERT,
                                     IDC_NG_LABEL_KEY, IDC_NG_KEY, IDC_NG_BTN_KEY }) {
                        HWND c = GetDlgItem(hwnd, ctl);
                        if (c) ShowWindow(c, on ? SW_SHOW : SW_HIDE);
                    }
                    break;
                }
                case IDC_NG_BTN_CERT:
                case IDC_NG_BTN_KEY: {
                    bool isCert = (id == IDC_NG_BTN_CERT);
                    static const wchar_t FILTER_CERT[] = L"证书文件 (*.crt;*.pem;*.cer)\0*.crt;*.pem;*.cer\0所有文件 (*.*)\0*.*\0";
                    static const wchar_t FILTER_KEY[]  = L"Key 文件 (*.key;*.pem)\0*.key;*.pem\0所有文件 (*.*)\0*.*\0";
                    OPENFILENAMEW ofn = {0};
                    wchar_t file[1024] = {0};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = isCert ? FILTER_CERT : FILTER_KEY;
                    ofn.lpstrFile = file;
                    ofn.nMaxFile = 1024;
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    if (GetOpenFileNameW(&ofn)) {
                        SetWindowTextW(GetDlgItem(hwnd, isCert ? IDC_NG_CERT : IDC_NG_KEY), file);
                    }
                    break;
                }
                case IDC_NG_BTN_DEL: {
                    HWND lv = GetDlgItem(hwnd, IDC_NG_VHOST_LIST);
                    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
                    if (sel < 0) { logAppend(Comp::Nginx, L"请先选中要删除的站点"); break; }
                    wchar_t name[256];
                    ListView_GetItemText(lv, sel, 0, name, 256);
                    std::wstring nm = name;
                    runAsync(Comp::Nginx, L"删除虚拟站点 " + nm, [nm](std::wstring& err) {
                        return nginxRemoveVHost(nm, err);
                    });
                    break;
                }
                case IDC_NG_BTN_RELOAD: {
                    runAsync(Comp::Nginx, L"重载 nginx 配置", [](std::wstring& err) {
                        return nginxReload(err);
                    });
                    break;
                }
                case IDC_PHP_BTN_START:
                case IDC_PHP_BTN_STOP: {
                    std::wstring v = phpSelectedVersion();
                    if (v.empty()) { logAppend(Comp::Php, L"请先在版本列表中选中一个 PHP 版本"); break; }
                    bool start = (id == IDC_PHP_BTN_START);
                    // The process count is a setting, not a start option, so it
                    // is committed here: reading it from the edit box at click
                    // time means the user can just type a number and press 启动.
                    int want = 1;
                    if (HWND ew = GetDlgItem(hwnd, IDC_PHP_WORKERS)) {
                        wchar_t buf[16] = {0};
                        GetWindowTextW(ew, buf, 16);
                        std::wstring t = trimStr(buf);
                        if (!t.empty()) {
                            want = _wtoi(t.c_str());
                            if (want < 1) want = 1;
                            if (want > 32) want = 32;
                        }
                    }
                    if (start && want != phpWorkerCount(v)) {
                        iniSet(L"php.workers." + v, std::to_wstring(want));
                        // A site points at an upstream as soon as the pool is
                        // bigger than one, so the vhost has to be rewritten.
                        // nginxReload regenerates the config (picking up the new
                        // {{PHP_UPSTREAM}}) and re-copies every site.
                        ComponentStatus ng = compStatus(Comp::Nginx);
                        if (ng.installed) {
                            std::wstring nerr;
                            if (!nginxReload(nerr))
                                logAppend(Comp::Php, L"改进程数后重载 nginx 失败: " + nerr);
                        }
                    }
                    runAsync(Comp::Php, (start ? L"启动 PHP " : L"停止 PHP ") + v,
                             [v, start](std::wstring& err) {
                        return start ? phpStart(v, err) : phpStop(v, err);
                    });
                    break;
                }
                case IDC_PHP_BTN_DEFAULT: {
                    std::wstring v = phpSelectedVersion();
                    if (v.empty()) { logAppend(Comp::Php, L"请先选中要设为默认的 PHP 版本"); break; }
                    runAsync(Comp::Php, L"设 PHP " + v + L" 为默认版本", [v](std::wstring& err) {
                        return compSwitchVersion(Comp::Php, v, err);
                    });
                    break;
                }
                case IDC_PHP_BTN_EXT: {
                    std::wstring v = phpSelectedVersion();
                    if (v.empty()) { logAppend(Comp::Php, L"请先选中要管理扩展的 PHP 版本"); break; }
                    showPhpExtDialog(hwnd, v);
                    break;
                }
                case IDC_PG_BTN_INIT: pgOpInit(); break;
                case IDC_PG_BTN_PWD: pgOpPwd(); break;
                case IDC_PG_BTN_ADDUSER: pgOpAddUser(); break;
                case IDC_PG_BTN_DELUSER: pgOpDelUser(); break;
                case IDC_PG_BTN_BACKUP: pgOpBackup(); break;
                case IDC_PG_BTN_RESTORE: pgOpRestore(); break;
                case IDC_NODE_BTN_RESTART_ALL:
                    runAsync(Comp::Nodejs, L"重启全部 PM2 应用", [](std::wstring& err) {
                        return nodePm2RestartAll(err);
                    });
                    break;
                case IDC_NODE_BTN_DELETE: {
                    HWND lv = GetDlgItem(hwnd, IDC_NODE_PM2_LIST);
                    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
                    if (sel < 0) { logAppend(Comp::Nodejs, L"请先选中要删除的项目"); break; }
                    wchar_t idBuf[64];
                    ListView_GetItemText(lv, sel, 0, idBuf, 64);
                    int pm2Id = _wtoi(idBuf);
                    runAsync(Comp::Nodejs, L"删除 PM2 项目 #" + std::to_wstring(pm2Id), [pm2Id](std::wstring& err) {
                        return nodePm2Delete(pm2Id, err);
                    });
                    break;
                }
                case IDC_NODE_BTN_REFRESH:
                    refreshPm2List();
                    break;
            }
            return 0;
        }
        case WM_NOTIFY: {
            NMHDR* nm = (NMHDR*)lParam;
            if (nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE) {
                showPage(TabCtrl_GetCurSel(g_tab));
            } else if (nm->idFrom == IDC_PHP_POOL_LIST && nm->code == LVN_ITEMCHANGED) {
                // Keep the process-count box showing what the selected row
                // actually runs, not whatever was typed for a previous row.
                syncWorkersEdit();
            }
            break;
        }
        case WM_OP_DONE: {
            Comp c = (Comp)wParam;
            CompUI& ui = g_ui[(int)c];
            ui.busy = false;
            refreshAll(c);
            refreshOverview();
            kickStatusPoll();
            kickPm2Poll();
            if (c == Comp::Nginx && lParam == 0 && g_pendingVhostClear) {
                g_pendingVhostClear = false;
                clearNginxAddForm();
            }
            break;
        }
        case WM_ALL_DONE: {
            AllOp op = (AllOp)wParam;
            int fail = (int)lParam;
            std::wstring txt = (op == AllOp::Start ? L"全部启动" :
                                op == AllOp::Restart ? L"全部重启" : L"全部停止");
            txt += fail == 0 ? L"完成" : (L"完成，失败 " + std::to_wstring(fail) + L" 项");
            if (g_ov.result) {
                SetWindowTextW(g_ov.result, txt.c_str());
            }
            ovLogAppend(L"==> " + txt);
            refreshOverview();
            kickStatusPoll();
            kickPm2Poll();
            break;
        }
        case WM_REAL_EXIT:
            // components are already stopped; tear down and quit
            // Flush any pending ini writes (autostart toggles etc.) before
            // the window dies, since the debounce timer won't fire again.
            iniFlushNow();
            DestroyWindow(hwnd);
            break;
        case WM_PG_USERS: {
            HWND combo = GetDlgItem(hwnd, IDC_PG_USER);
            if (!combo) break;
            std::vector<std::wstring> users;
            {
                std::lock_guard<std::mutex> lk(g_snapMtx);
                users = g_pgUsers;
            }
            wchar_t cur[128];
            GetWindowTextW(combo, cur, 128);
            SendMessageW(combo, CB_RESETCONTENT, 0, 0);
            for (auto& u : users) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)u.c_str());
            SetWindowTextW(combo, cur);
            break;
        }
        case WM_UI_LOG: {
            HWND edit = (HWND)wParam;
            const wchar_t* text = (const wchar_t*)lParam;
            if (IsWindow(edit) && text) logAppendRaw(edit, text);
            free((void*)lParam);
            break;
        }
        case WM_UI_STATUS:
            applyStatusSnapshot();
            break;
        case WM_UI_PM2:
            applyPm2Snapshot();
            break;
        case WM_CTLCOLORSTATIC: {
            if ((int)GetDlgCtrlID((HWND)lParam) == IDC_OV_ABOUT_LINK) {
                SetTextColor((HDC)wParam, RGB(0, 102, 204));
                SetBkMode((HDC)wParam, TRANSPARENT);
                return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }
            break;
        }
        case WM_SETCURSOR: {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            HWND h = ChildWindowFromPoint(hwnd, pt);
            if (h && GetDlgCtrlID(h) == IDC_OV_ABOUT_LINK) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_TRAYICON: {
            UINT evt = LOWORD(lParam);
            if (evt == WM_RBUTTONUP) {
                showTrayMenu(hwnd);
            } else if (evt == WM_LBUTTONDBLCLK) {
                trayShowMain();
            }
            break;
        }
        case WM_CLOSE:
            // Clicking the window's close button only hides it to the tray —
            // components keep running. Real exit happens exclusively from the
            // tray menu's "退出" item (IDM_TRAY_EXIT -> shutdownAllComponents
            // -> WM_REAL_EXIT), so a user who hits X by accident loses nothing.
            ShowWindow(hwnd, SW_HIDE);
            trayBalloon(L"LNPP 组件管理器", L"已最小化到系统托盘，退出请右键托盘图标");
            return 0;
        case WM_SIZE: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            MoveWindow(g_tab, 0, 0, rc.right, 30, TRUE);
            break;
        }
        case WM_DESTROY:
            g_appClosing = true;
            trayRemove();
            KillTimer(hwnd, 1);
            KillTimer(hwnd, 2);
            KillTimer(hwnd, 3);
            // Stopping the stack is the tray Exit path's job (shutdownAllComponents
            // on a worker thread, then WM_REAL_EXIT). Reaching WM_DESTROY from
            // anywhere else - Windows logoff, or a future DestroyWindow caller -
            // must still not leave servers running, but it must not block this
            // thread for a full stop cycle either: skip components that are
            // already down and skip the whole sequence when the exit path has
            // taken over (g_shutdownStarted).
            if (!g_shutdownStarted.exchange(true)) {
                for (int i = 0; i < (int)Comp::Count; ++i) {
                    Comp c = (Comp)i;
                    if (!compRunningQuick(c)) continue;   // nothing to stop
                    std::wstring err;
                    compStop(c, err);
                }
            }
            // flush any pending ini writes before we exit; without this
            // the last 200ms of toggles would be lost.
            iniFlushNow();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================ Entry ============================

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    // single-instance guard: a second launch just focuses the existing window
    HANDLE hSingle = CreateMutexW(nullptr, FALSE, L"LNPPManager_SingleInstance");
    if (!hSingle) {
        // guard creation failed (rare); run anyway rather than dying silently
        logMsg(L"app", L"单实例互斥体创建失败，跳过单实例检查");
    } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"LNPPManager", nullptr);
        if (existing) {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        CloseHandle(hSingle);
        return 0;
    }
    // primary instance: keep hSingle open for the process lifetime
    // (the kernel releases it when we exit — no explicit CloseHandle needed)
    // boot start ("开机启动") launches minimized into the tray
    if (lpCmdLine && wcsstr(lpCmdLine, L"--hidden")) g_startHidden = true;

    // Mark this thread as the UI thread so isUiThread() can guard
    // synchronous component ops (pgBackup / compStart / pgRestore etc.)
    // that would otherwise hang the window for 30s+.
    registerUiThread(GetCurrentThreadId());

    // Layout before anything else touches a path: create data\ when missing,
    // seed the download list from the etc\ template, and move per-site vhost
    // files out of etc\ (templates only) into data\.
    prepareRuntimeLayout();

    INITCOMMONCONTROLSEX icc = {0};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    g_font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
    g_monoFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    {
        LOGFONTW lf = {0};
        GetObjectW(g_font, sizeof(lf), &lf);
        lf.lfUnderline = TRUE;
        g_linkFont = CreateFontIndirectW(&lf);
    }

    WNDCLASSW dotClass = {0};
    dotClass.lpfnWndProc = DotProc;
    dotClass.hInstance = hInst;
    dotClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    dotClass.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    dotClass.lpszClassName = DOT_CLASS;
    RegisterClassW(&dotClass);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP),
                                   IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"LNPPManager";
    RegisterClassExW(&wc);

    g_main = CreateWindowExW(0, L"LNPPManager", L"LNPP 组件管理器",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                             WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 760, 610,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_main) return 0;

    if (g_startHidden) ShowWindow(g_main, SW_HIDE);
    else ShowWindow(g_main, nCmdShow);
    UpdateWindow(g_main);
    trayAdd();

    // Start the components marked "随管理器启动". Outside WM_CREATE on purpose:
    // each runs on its own thread and reports back by posting to g_main, so the
    // window must be fully created before any of them can finish.
    autoStartComponents();

    // first run: no components under bin -> prompt the downloader automatically
    if (!g_startHidden && pkgsNeedSetup()) showDownloaderDialog(g_main);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
