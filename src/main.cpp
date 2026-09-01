#include "common.h"
#include "process.h"
#include "manager.h"
#include "downloader.h"
#include <commdlg.h>
#include <shlobj.h>

// App icon resource (see app.rc)
#define IDI_APP 101

// ============================ Control IDs ============================
enum {
    IDC_TAB = 100,
    // common per-page controls
    IDC_DOT = 200, IDC_STATUS_TXT = 201, IDC_VER_COMBO = 202,
    IDC_BTN_SWITCH = 203, IDC_BTN_START = 204, IDC_BTN_STOP = 205,
    IDC_BTN_CFG = 206, IDC_BTN_DATA = 207, IDC_LOG = 208,
    // nginx page
    IDC_NG_VHOST_LIST = 300, IDC_NG_ADD_NAME = 301, IDC_NG_ADD_DOMAIN = 302,
    IDC_NG_ADD_PORT = 303, IDC_NG_BTN_ADD = 304, IDC_NG_BTN_DEL = 305, IDC_NG_BTN_RELOAD = 306,
    IDC_NG_LABEL_NAME = 307, IDC_NG_LABEL_DOMAIN = 308, IDC_NG_LABEL_PORT = 309,
    IDC_NG_SSL = 310, IDC_NG_CERT = 311, IDC_NG_KEY = 312,
    IDC_NG_BTN_CERT = 313, IDC_NG_BTN_KEY = 314,
    IDC_NG_LABEL_CERT = 315, IDC_NG_LABEL_KEY = 316,
    IDC_NG_ROOT = 317, IDC_NG_BTN_ROOT = 318, IDC_NG_LABEL_ROOT = 319,
    // postgresql page
    IDC_PG_BTN_INIT = 400, IDC_PG_BTN_PWD = 401, IDC_PG_BTN_ADDUSER = 402,
    IDC_PG_BTN_DELUSER = 403, IDC_PG_BTN_BACKUP = 404, IDC_PG_INFO = 405,
    IDC_PG_USER = 406, IDC_PG_PWD = 407, IDC_PG_PORT = 408,
    IDC_PG_LABEL_USER = 409, IDC_PG_LABEL_PWD = 410, IDC_PG_LABEL_PORT = 411,
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
    IDC_OV_LOG = 715,
    IDC_OV_COMP_START_BASE = 710,   // + comp index: start/stop toggle button
    IDC_OV_COMP_AUTO_BASE = 720,    // + comp index: "随管理器启动" checkbox
    IDC_OV_COMP_STATUS_BASE = 730,  // + comp index: status text
};

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
static std::atomic<int> g_curTab{0};
static bool g_pendingVhostClear = false;   // clear nginx add form after a successful add
static std::vector<std::wstring> g_pgUsers;   // pg user list for the user combo
static std::atomic<bool> g_pgUsersBusy{false};

// ---- system tray / boot start ----
static bool g_trayAdded = false;
static bool g_realExit = false;
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
    ui.busy = false;
    logMsgUi(c, ok ? (L"==> " + ui.opName + L" 完成") : (L"==> " + ui.opName + L" 失败: " + msg));
    // refresh UI on UI thread
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
    if (g_pm2WorkerBusy.exchange(true)) return;
    std::thread(pm2Worker).detach();
}

// ============================ Refresh functions ============================

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
        ListView_SetItemText(lv, item.iItem, 3, (LPWSTR)v.root.c_str());
    }
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
        ListView_SetItemText(lv, item.iItem, 3, (LPWSTR)std::to_wstring(a.restarts).c_str());
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
        std::wstring txt = !installed ? L"未安装"
                         : (running ? L"运行中 " + ver : L"已停止");
        SetWindowTextW(ov.status[i], txt.c_str());
        SetWindowTextW(ov.btnToggle[i], running ? L"停止" : L"启动");
        EnableWindow(ov.btnToggle[i], !g_ui[i].busy && !allBusy);
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
    wchar_t buf[128];
    SendMessageW(ui.verCombo, CB_GETLBTEXT, idx, (LPARAM)buf);
    std::wstring ver = buf;
    ComponentStatus st = compStatus(c);
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

static bool pathHasNode(std::wstring& path, bool& present) {
    present = false;
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return false;
    DWORD type = REG_EXPAND_SZ;
    wchar_t buf[32768] = {0};
    DWORD size = sizeof(buf);
    LONG r = RegQueryValueExW(hk, L"Path", nullptr, &type, (LPBYTE)buf, &size);
    if (r != ERROR_SUCCESS) { RegCloseKey(hk); path.clear(); return true; }
    path = buf;
    RegCloseKey(hk);
    std::wstring nodeDir = lowerStr(compBinDirVer(Comp::Nodejs, iniGet(L"ver.nodejs", L"")));
    std::wstringstream ss(path);
    std::wstring item;
    while (std::getline(ss, item, L';')) {
        if (lowerStr(trimStr(item)) == nodeDir) { present = true; break; }
    }
    return true;
}

static void pathAddNode() {
    std::wstring nodeDir = compBinDirVer(Comp::Nodejs, iniGet(L"ver.nodejs", L""));
    if (nodeDir.empty() || !dirExists(nodeDir)) {
        ovLogAppend(L"Node 路径不可用，未加入用户 PATH");
        return;
    }
    std::wstring path;
    bool present;
    if (!pathHasNode(path, present)) {
        ovLogAppend(L"读取用户 PATH 失败，未加入 Node 目录");
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
    bool present;
    if (!pathHasNode(path, present)) {
        ovLogAppend(L"读取用户 PATH 失败，未移除 Node 目录");
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
static void shutdownAllComponents() {
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
        g_realExit = true;
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

    for (int i = 0; i < (int)Comp::Count; ++i) {
        int y = 112 + i * 40;
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
                       10, 320, 700, 170, parent);
    SendMessageW(log, WM_SETFONT, (WPARAM)g_monoFont, TRUE);
    ov.logEdit = log;
    ov.controls.push_back(log);

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
    col.cx = 120; col.pszText = (LPWSTR)L"站点"; ListView_InsertColumn(lv, 0, &col);
    col.cx = 160; col.pszText = (LPWSTR)L"域名"; ListView_InsertColumn(lv, 1, &col);
    col.cx = 60;  col.pszText = (LPWSTR)L"端口"; ListView_InsertColumn(lv, 2, &col);
    col.cx = 80;  col.pszText = (LPWSTR)L"根目录"; ListView_InsertColumn(lv, 3, &col);

    HWND bAdd = makeCtl(IDC_NG_BTN_ADD, L"BUTTON", L"添加站点", BS_PUSHBUTTON, 20, 248, 90, 26, parent);
    HWND bDel = makeCtl(IDC_NG_BTN_DEL, L"BUTTON", L"删除", BS_PUSHBUTTON, 120, 248, 70, 26, parent);
    HWND bReload = makeCtl(IDC_NG_BTN_RELOAD, L"BUTTON", L"重载", BS_PUSHBUTTON, 200, 248, 70, 26, parent);

    HWND lName = makeCtl(IDC_NG_LABEL_NAME, L"STATIC", L"站点名:", SS_LEFT, 450, 82, 60, 20, parent);
    HWND eName = makeCtl(IDC_NG_ADD_NAME, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, 510, 80, 170, 22, parent);
    HWND lDomain = makeCtl(IDC_NG_LABEL_DOMAIN, L"STATIC", L"域名:", SS_LEFT, 450, 104, 60, 20, parent);
    HWND eDomain = makeCtl(IDC_NG_ADD_DOMAIN, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, 510, 102, 170, 22, parent);
    HWND lPort = makeCtl(IDC_NG_LABEL_PORT, L"STATIC", L"端口:", SS_LEFT, 450, 126, 60, 20, parent);
    HWND ePort = makeCtl(IDC_NG_ADD_PORT, L"EDIT", L"80", WS_CHILD | WS_VISIBLE | WS_BORDER, 510, 124, 170, 22, parent);
    HWND lRoot = makeCtl(IDC_NG_LABEL_ROOT, L"STATIC", L"根目录:", SS_LEFT, 450, 148, 60, 20, parent);
    HWND eRoot = makeCtl(IDC_NG_ROOT, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, 510, 146, 150, 22, parent);
    HWND bRoot = makeCtl(IDC_NG_BTN_ROOT, L"BUTTON", L"浏览", BS_PUSHBUTTON, 665, 146, 45, 22, parent);

    HWND chkSsl = makeCtl(IDC_NG_SSL, L"BUTTON", L"HTTPS/SSL", BS_AUTOCHECKBOX, 450, 170, 120, 20, parent);
    HWND lCert = makeCtl(IDC_NG_LABEL_CERT, L"STATIC", L"证书:", SS_LEFT, 450, 192, 60, 20, parent);
    HWND eCert = makeCtl(IDC_NG_CERT, L"EDIT", L"", WS_CHILD | WS_BORDER, 510, 190, 150, 22, parent);
    HWND bCert = makeCtl(IDC_NG_BTN_CERT, L"BUTTON", L"浏览", BS_PUSHBUTTON, 665, 190, 45, 22, parent);
    HWND lKey = makeCtl(IDC_NG_LABEL_KEY, L"STATIC", L"Key:", SS_LEFT, 450, 214, 60, 20, parent);
    HWND eKey = makeCtl(IDC_NG_KEY, L"EDIT", L"", WS_CHILD | WS_BORDER, 510, 212, 150, 22, parent);
    HWND bKey = makeCtl(IDC_NG_BTN_KEY, L"BUTTON", L"浏览", BS_PUSHBUTTON, 665, 212, 45, 22, parent);
    // SSL inputs start hidden; the HTTPS/SSL checkbox reveals them
    for (HWND h : { lCert, eCert, bCert, lKey, eKey, bKey }) ShowWindow(h, SW_HIDE);
    // SSL-only controls are toggled by the checkbox; keep them out of pageControls
    // so switching tabs never forces them visible
    ui.pageControls.push_back(lName);
    ui.pageControls.push_back(lDomain);
    ui.pageControls.push_back(lPort);
    ui.pageControls.push_back(lRoot);
    ui.pageControls.push_back(eRoot);
    ui.pageControls.push_back(bRoot);
    ui.pageControls.push_back(chkSsl);
    ui.pageControls.push_back(eName);
    ui.pageControls.push_back(eDomain);
    ui.pageControls.push_back(ePort);
    ui.pageControls.push_back(bAdd);
    ui.pageControls.push_back(bDel);
    ui.pageControls.push_back(bReload);
}

static void initPgPage(HWND parent) {
    CompUI& ui = g_ui[(int)Comp::Postgresql];
    HWND bInit = makeCtl(IDC_PG_BTN_INIT, L"BUTTON", L"初始化数据库", BS_PUSHBUTTON, 20, 88, 120, 26, parent);
    HWND bPwd = makeCtl(IDC_PG_BTN_PWD, L"BUTTON", L"修改密码", BS_PUSHBUTTON, 20, 122, 120, 26, parent);
    HWND bAddU = makeCtl(IDC_PG_BTN_ADDUSER, L"BUTTON", L"创建用户", BS_PUSHBUTTON, 20, 156, 120, 26, parent);
    HWND bDelU = makeCtl(IDC_PG_BTN_DELUSER, L"BUTTON", L"删除用户", BS_PUSHBUTTON, 20, 190, 120, 26, parent);
    HWND bBak = makeCtl(IDC_PG_BTN_BACKUP, L"BUTTON", L"备份数据库", BS_PUSHBUTTON, 20, 224, 120, 26, parent);
    ui.pageControls.push_back(bInit);
    ui.pageControls.push_back(bPwd);
    ui.pageControls.push_back(bAddU);
    ui.pageControls.push_back(bDelU);
    ui.pageControls.push_back(bBak);

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
    col.cx = 60;  col.pszText = (LPWSTR)L"ID"; ListView_InsertColumn(lv, 0, &col);
    col.cx = 220; col.pszText = (LPWSTR)L"名称"; ListView_InsertColumn(lv, 1, &col);
    col.cx = 100; col.pszText = (LPWSTR)L"状态"; ListView_InsertColumn(lv, 2, &col);
    col.cx = 80;  col.pszText = (LPWSTR)L"重启次数"; ListView_InsertColumn(lv, 3, &col);
    HWND bRefresh = makeCtl(IDC_NODE_BTN_REFRESH, L"BUTTON", L"刷新", BS_PUSHBUTTON, 20, 216, 100, 26, parent);
    HWND bRestartAll = makeCtl(IDC_NODE_BTN_RESTART_ALL, L"BUTTON", L"全部重启", BS_PUSHBUTTON, 130, 216, 100, 26, parent);
    HWND bDelete = makeCtl(IDC_NODE_BTN_DELETE, L"BUTTON", L"删除选中", BS_PUSHBUTTON, 240, 216, 100, 26, parent);
    ui.pageControls.push_back(bRefresh);
    ui.pageControls.push_back(bRestartAll);
    ui.pageControls.push_back(bDelete);
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
    HWND log = makeCtl(IDC_LOG, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                       ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, 10, 280, 700, 170, parent);
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
    ComponentStatus st = compStatus(Comp::Postgresql);
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
        auto prog = [hwnd](const std::wstring& stage, DWORD done, DWORD total) {
            wchar_t* s = _wcsdup(stage.c_str());
            if (!IsWindow(hwnd)) { free(s); return; }   // dialog gone: drop progress
            PostMessageW(hwnd, WM_DL_STAGE, 0, (LPARAM)s);
            PostMessageW(hwnd, WM_DL_PROGRESS, (WPARAM)done, (LPARAM)total);
        };
        bool ok = pkgsInstall(copy, prog, cancelSp.get(), err);
        wchar_t* e = _wcsdup(err.c_str());
        if (IsWindow(hwnd)) PostMessageW(hwnd, WM_DL_DONE, ok ? 1 : 0, (LPARAM)e);
        else free(e);
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
                    wchar_t buf[64];
                    swprintf(buf, 64, L"%s  %s  %u%%", st->stage.c_str(), st->curName.c_str(), pct);
                    SetWindowTextW(st->statusTxt, buf);
                } else {
                    int pct = (int)((done / 1024) % 100);
                    SendMessageW(st->progress, PBM_SETPOS, pct, 0);
                    wchar_t buf[64];
                    swprintf(buf, 64, L"%s  %s  %u KB", st->stage.c_str(), st->curName.c_str(), done / 1024);
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
                    // refresh main window version combos
                    if (g_main) PostMessageW(g_main, WM_OP_DONE, (WPARAM)Comp::Nodejs, 0);
                    if (g_main) PostMessageW(g_main, WM_OP_DONE, (WPARAM)Comp::Postgresql, 0);
                    if (g_main) PostMessageW(g_main, WM_OP_DONE, (WPARAM)Comp::Nginx, 0);
                    if (g_main) PostMessageW(g_main, WM_OP_DONE, (WPARAM)Comp::Redis, 0);
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

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
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

            initOverviewPage(hwnd);
            initCommonControls(hwnd, Comp::Nginx);
            initNginxPage(hwnd);
            initCommonControls(hwnd, Comp::Postgresql);
            initPgPage(hwnd);
            initCommonControls(hwnd, Comp::Redis);
            initRedisPage(hwnd);
            initCommonControls(hwnd, Comp::Nodejs);
            initNodePage(hwnd);

            showPage(TAB_OVERVIEW);
            kickStatusPoll();
            kickPm2Poll();
            SetTimer(hwnd, 1, 3000, pm2Timer);    // pm2 refresh
            SetTimer(hwnd, 2, 2000, statusTimer); // status poll
            SetTimer(hwnd, 3, 50, iniFlushTimer); // drain pending ini writes
            autoStartComponents();
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int cur = TabCtrl_GetCurSel(g_tab);
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
                    if (cur >= TAB_COMP_BASE) actStart(tabToComp(cur));
                    break;
                case IDC_BTN_STOP:
                    if (cur >= TAB_COMP_BASE) actStop(tabToComp(cur));
                    break;
                case IDC_BTN_SWITCH:
                    if (cur >= TAB_COMP_BASE) actSwitch(tabToComp(cur));
                    break;
                case IDC_BTN_CFG: {
                    if (cur < TAB_COMP_BASE) break;
                    Comp c = tabToComp(cur);
                    ShellExecuteW(hwnd, L"open", compEtcDir(c).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                }
                case IDC_BTN_DATA: {
                    if (cur < TAB_COMP_BASE) break;
                    Comp c = tabToComp(cur);
                    ComponentStatus st = compStatus(c);
                    if (st.installed) {
                        std::wstring dir = compDataVerDir(c, st.currentVersion);
                        makeDirs(dir);
                        ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                    break;
                }
                case IDC_NG_BTN_ADD: {
                    wchar_t name[256], domain[256], port[256], cert[1024], key[1024], root[1024];
                    GetDlgItemTextW(hwnd, IDC_NG_ADD_NAME, name, 256);
                    GetDlgItemTextW(hwnd, IDC_NG_ADD_DOMAIN, domain, 256);
                    GetDlgItemTextW(hwnd, IDC_NG_ADD_PORT, port, 256);
                    GetDlgItemTextW(hwnd, IDC_NG_ROOT, root, 1024);
                    bool ssl = SendMessageW(GetDlgItem(hwnd, IDC_NG_SSL), BM_GETCHECK, 0, 0) == BST_CHECKED;
                    GetDlgItemTextW(hwnd, IDC_NG_CERT, cert, 1024);
                    GetDlgItemTextW(hwnd, IDC_NG_KEY, key, 1024);
                    std::wstring nm = name, dm = domain, pt = port, ct = cert, ky = key, rt = root;
                    g_pendingVhostClear = true;
                    runAsync(Comp::Nginx, L"添加虚拟站点 " + nm, [nm, dm, pt, ssl, ct, ky, rt](std::wstring& err) {
                        return nginxAddVHostEx(nm, dm, pt, ssl, ct, ky, rt, err);
                    });
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
                case IDC_PG_BTN_INIT: pgOpInit(); break;
                case IDC_PG_BTN_PWD: pgOpPwd(); break;
                case IDC_PG_BTN_ADDUSER: pgOpAddUser(); break;
                case IDC_PG_BTN_DELUSER: pgOpDelUser(); break;
                case IDC_PG_BTN_BACKUP: pgOpBackup(); break;
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
            // closing the window always goes through the "shutdown, then
            // really exit" path. Even when minimized-to-tray was intended,
            // we still need a clean shutdown when the user picks Quit from
            // the tray menu; running compStop synchronously inside
            // WM_DESTROY would block the UI thread for up to 30s×4
            // (each component has its own 30s stop timeout).
            if (!g_realExit) {
                g_realExit = true;
                ShowWindow(hwnd, SW_HIDE);
                trayBalloon(L"LNPP 组件管理器", L"正在停止所有组件...");
                std::thread([]() {
                    shutdownAllComponents();
                    PostMessageW(g_main, WM_REAL_EXIT, 0, 0);
                }).detach();
                return 0;
            }
            DestroyWindow(hwnd);
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
            // stop all components
            for (int i = 0; i < (int)Comp::Count; ++i) {
                std::wstring err;
                compStop((Comp)i, err);
            }
            KillTimer(hwnd, 1);
            KillTimer(hwnd, 2);
            KillTimer(hwnd, 3);
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
                             CW_USEDEFAULT, CW_USEDEFAULT, 760, 560,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_main) return 0;

    if (g_startHidden) ShowWindow(g_main, SW_HIDE);
    else ShowWindow(g_main, nCmdShow);
    UpdateWindow(g_main);
    trayAdd();

    // first run: no components under bin -> prompt the downloader automatically
    if (!g_startHidden && pkgsNeedSetup()) showDownloaderDialog(g_main);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
