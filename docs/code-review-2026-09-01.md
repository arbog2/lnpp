# LNPP 组件管理器 — 代码审查报告

- 审查日期：2026-09-01
- 审查范围：`src/`（4 个模块，约 3300 行）、`build.bat` / `test.bat` / `app.rc`、`.gitignore`、仓库卫生
- 审查方式：静态代码审查（未编译运行）
- 结论：**功能完整度不错，但存在 3 个必修缺陷（其中 1 个会让程序挂机一小时后界面失效）**

## 总体评价

项目结构清晰：common（路径/INI/日志）、process（进程封装）、manager（组件业务逻辑）、main（Win32 GUI）
四层分离得当，后台轮询 + 快照 + PostMessage 回 UI 线程的架构是对的，注释也记录了大量踩坑经验
（pm2 守护进程复活、redis pidfile 优于 ping、nginx 相对路径可移植等），工程质量在中上水平。

主要短板集中在三处：**GDI 资源与线程安全这两类"长时间运行才暴露"的问题**，
**外部命令拼接缺少转义**，以及**部分函数存在自我承认的死代码**。

---

## P0 — 必修（会导致崩溃、界面失效或安全漏洞）

### P0-1 状态灯控件每次重绘泄漏一个 GDI 刷子，且颜色从未生效

**位置**：`src/main.cpp:878-898`（`DotProc` 的 `WM_PAINT`）

```cpp
HBRUSH br = CreateSolidBrush(RGB(200, 200, 200));          // #1 创建
if (state == 2) br = CreateSolidBrush(RGB(60, 200, 60));   // #2 覆盖，#1 泄漏
else if (state == 1) br = CreateSolidBrush(RGB(220, 60, 60));
else br = CreateSolidBrush(RGB(150, 150, 150));
Ellipse(dc, rc.left + 2, rc.top + 2, rc.right - 2, rc.bottom - 2);  // 从未 SelectObject(br)
DeleteObject(br);   // 只删了 #2
```

两个问题叠加：

1. **泄漏**：每次 `WM_PAINT` 泄漏 1 个 `HBRUSH`。触发频率：`statusTimer`(2s) +
   `pm2Timer`(3s) → `setStatus()`/`refreshOverview()` → `InvalidateRect()`，
   可见区域约 **每秒 2.5~4 次重绘**。Windows 每进程 GDI 句柄上限默认 10000，
   **约 1 小时内耗尽**，之后所有绘图 API 失败，界面彻底卡死/白屏。
   对一个"常驻托盘的服务器管理器"来说，这是致命的。
2. **颜色失效**：`br` 从未 `SelectObject` 进 DC，`Ellipse` 用的是 DC 默认刷子（白色），
   所以"运行中绿 / 已停止红 / 未安装灰"三态**从来没有真正显示过**，全是白圆。

**修复**：用 `HBRUSH` 局部变量 + `FillEllipse` 的常规写法，或一个静态刷子数组复用。

```cpp
case WM_PAINT: {
    PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    static const COLORREF kColor[] = { RGB(150,150,150), RGB(220,60,60), RGB(60,200,60) };
    int state = (int)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (state < 0 || state > 2) state = 0;
    HBRUSH br = CreateSolidBrush(kColor[state]);
    HGDIOBJ old = SelectObject(dc, br);
    Ellipse(dc, rc.left + 2, rc.top + 2, rc.right - 2, rc.bottom - 2);
    SelectObject(dc, old);
    DeleteObject(br);
    EndPaint(hwnd, &ps);
    return 0;
}
```

### P0-2 INI 缓存无锁，后台线程与 UI 线程并发读写 std::map

**位置**：`src/common.cpp:244`（`g_iniCache`）、`271-311`（`iniSave` / `iniGet` / `iniSet`）

`g_iniCache`、`g_iniDirty`、`g_iniLastChangeTick` 全部无同步保护，但存在明确的并发路径：

| 线程 | 调用链 |
|---|---|
| 后台轮询线程 | `statusWorker` → `compStatusQuick` → `compStatusImpl` → `iniGet` **且 `iniSet`**（`manager.cpp:98`） |
| pm2 轮询线程 | `pm2Worker` → 同上 |
| UI 线程（50ms 定时器） | `iniFlushTimer` → `iniFlushIfDue` → `iniSave` **遍历整个 map** |
| UI 线程（用户操作） | `ovAutoComp` / `pgInit` / `autoStartComponents` → `iniSet` |

`std::map` 在插入触发红黑树旋转时被并发遍历或读取，会读到半重建的树 → **随机崩溃或死循环**。
`iniSave` 遍历时另一个线程 `iniSet` 插入节点，迭代器直接失效。

**修复**：加一把互斥锁保护 `iniMap()` 的整个生命周期。

```cpp
static std::mutex g_iniMtx;
std::map<std::wstring, std::wstring>& iniMap() {
    std::lock_guard<std::mutex> lk(g_iniMtx);   // 注意：返回引用 + 锁的组合不安全
    ...
}
```

更稳妥的做法是把锁粒度提到函数级：让 `iniGet/iniSet/iniDelete/iniFlushIfDue/iniFlushNow`
各自加锁，并且**不要对外暴露 map 引用**（`iniMap()` 目前是公开返回引用的私有实现，
确保所有访问都走加锁的包装函数即可）。

> 顺带一提：`compStatusImpl` 里的 `iniSet`（`manager.cpp:98`）让"查询状态"这个函数
> 产生了写副作用，是这处竞态的根源之一。建议把"发现当前版本无效"与"回写 ini"拆开，
> 只在 UI 线程做回写。

### P0-3 PostgreSQL 用户名/密码直接拼接进 SQL 与命令行，可注入

**位置**：`src/manager.cpp:981-982`（改密码）、`996-997`（建用户）、`1010-1011`（删用户）

```cpp
std::wstring cmd = L"-h 127.0.0.1 -p " + pgPort() + L" -U " + pgUser() +
                   L" -d postgres -c \"ALTER USER " + user +
                   L" WITH PASSWORD '" + password + L"';\"";
```

`user` / `password` 直接来自 `IDC_PG_USER` / `IDC_PG_PWD` 输入框（`main.cpp:1166-1184`），
未做任何转义。密码填 `a'; DROP DATABASE appdb; --` 或 `a'; CREATE ROLE evil SUPERUSER; --`
即可执行任意 SQL。本机工具场景危害有限，但任何把 PostgreSQL 端口暴露出去、或多人共用
一台开发机的情况都会中招，且这是"改一行就能修"的问题。

**修复**：用 psql 的变量插值（`psql -v`），`psql` 会对 `:'var'` 做正确的引号转义：

```cpp
std::wstring sql = L"ALTER USER " + safeIdent(user) + L" WITH PASSWORD :'pw';";
RunResult r = runProcessCapture(psql,
    L"-h 127.0.0.1 -p " + port + L" -U " + pgUser() + L" -d postgres -v pw=" + password +
    L" -c \"" + sql + L"\"", ...);
```

同时必须校验标识符（`user`、`pgPort()`）：
- 用户名：白名单 `^[A-Za-z_][A-Za-z0-9_$]*$`
- 端口：`^[0-9]{1,5}$` 且 <= 65535

`pgPort()` 来自 `data/settings.ini`，虽是本机构件，但被改后同样会被拼进
`psql` / `redis-cli` / `pg_dumpall` 的命令行（`manager.cpp:458, 572, 1054`），一并校验。

---

## P1 — 重要（稳定性 / 数据安全 / 副作用）

### P1-4 通过修改本进程环境变量给子进程传参，多线程下会串

**位置**：`src/process.cpp:146-179`

`runProcessCapture` 用 `SetEnvironmentVariableW` 临时改**当前进程**的环境，spawn 后再恢复，
注释说明了这是为了避免构造 env block。但存在并发调用：

- `pm2Worker` 线程 → `nodePm2List` → `runPm2` → 设置 `PATH`（`manager.cpp:285`）
- 用户点"备份数据库" → `pgBackup` → 设置 `PGPASSWORD`（`manager.cpp:1057`）
- `autoStartComponents` 启动时并发 `compStart`

两个线程交错的保存/恢复会让环境变量串味或永久残留
（例如 `PGPASSWORD` 被 `PATH` 的恢复逻辑覆盖成旧值，导致备份认证失败）。

讽刺的是 `process.cpp:66-97` 已经写好了正确的 `buildEnvBlock()`，却**从未被调用，也从未在
头文件声明**（死代码）。

**修复**：改用 `buildEnvBlock(env)`，把结果传给 `CreateProcessW` 的 `lpEnvironment`
（需配合 `CREATE_UNICODE_ENVIRONMENT`），删掉保存/恢复逻辑。这样彻底无副作用。

### P1-5 下载线程持有对话框状态指针，对话框销毁后 use-after-free

**位置**：`src/main.cpp:1431-1442`（线程）、`1556-1559`（`WM_DESTROY` 中 `delete st`）

```cpp
std::thread([hwnd, copy]() {
    DlState* st2 = dlState(hwnd);                 // 取一次指针
    bool ok = pkgsInstall(copy, prog, st2 ? &st2->cancel : nullptr, err);
    ...
}).detach();
```

下载进行中若主窗口退出，下载对话框作为 owned window 会被系统自动销毁 → `WM_DESTROY`
执行 `delete st`。此后下载线程仍在运行，它持有的 `st2->cancel` 是**悬空引用**，
`cancel->load()` 是未定义行为。

另外 `WM_DL_STAGE` / `WM_DL_DONE` 用 `_wcsdup` 传字符串，若目标窗口已销毁，
消息被丢弃 → 内存泄漏（不崩，但泄漏）。

**修复**：把 `cancel` 改成 `shared_ptr<std::atomic<bool>>`，线程捕获拷贝而非裸指针；
或在 `WM_DESTROY` 中先等待线程结束（用一个 `std::atomic<bool>` + 条件变量）。

### P1-6 worker 线程直接 SendMessage 操作 UI 控件

**位置**：`src/main.cpp:234`（`endOp` → `logMsgUi`）、`514/518/521`（`ovAllOp` → `ovLogAppend`）、
`255-258`（`ovRunAsync`）

`logAppendTo` 内部用 `SendMessageW(edit, EM_REPLACESEL, ...)`。跨线程 `SendMessage` 通常会
序列化到 UI 线程执行，可用；但一旦 UI 线程正在 `DestroyWindow`（P1-5 的退出路径）或处于
模态循环（`modal_loop`），就会死锁或访问已销毁的 HWND。

**修复**：日志追加改为 `PostMessage` 投递到主窗口，由 UI 线程统一写入；
或至少在 `WM_DESTROY` 里先用原子标志禁止 worker 再发消息。

### P1-7 nginx 状态检测按进程名匹配，会误伤系统上无关的 nginx 实例

**位置**：`src/manager.cpp:136-149`（`anyNginxRunning`）、`176-185`（`nginxRunningVer`）

```cpp
if (_wcsicmp(pe.szExeFile, L"nginx.exe") == 0) { found = true; break; }
```

只要系统里跑着**任何**一个 `nginx.exe`（用户自己装的、其他集成环境的），
`nginxRunningVer` 就会返回 true，并把那个进程的 PID 写进 LNPP 的 pidfile。
后果：LNPP 显示"运行中"实际没运行；点"停止"会去 kill 别人的 nginx。

**修复**：用 `QueryFullProcessImageNameW` 拿到完整路径，校验它位于
`bin\nginx\<ver>\` 之下再认领。

### P1-8 "测试 redis 配置"会真的启动一个 Redis 并把它留在后台

**位置**：`src/manager.cpp:1067-1082`（`redisTestConfig`）

```cpp
startProcessDetached(exe, L"\"" + conf + L"\"", ...);
WaitForSingleObject(pi.hProcess, 2000);
GetExitCodeProcess(pi.hProcess, &code);
CloseHandle(pi.hProcess);
return code == STILL_ACTIVE || redisRunningVer(ver);
```

2 秒后进程若仍在跑，函数返回 true，**然后直接 CloseHandle 走人**——那个 redis-server
继续驻留。所谓"测试"实为"静默启动"。目前该函数在代码里没有被调用（属于未接线的功能），
一旦接上就是个坑。

**修复**：测试完主动 `killProcessByPid(pi.pid)`，或改用 `redis-server --test-memory` /
解析配置文件的静态校验。

### P1-9 组件下载无完整性校验，packages.conf 可指向任意 URL

**位置**：`src/downloader.cpp:264-326`（`pkgsInstall`）、`143-223`（`pkgsDownload`）

下载 → 解压 → 直接投入使用，全程无 SHA256 / 签名校验。`packages.conf` 是纯文本文件，
任何人改一行就能让管理器下载并运行任意可执行程序（nginx.exe / initdb.exe 随后以用户权限执行）。
`packages.conf` 里的 URL 若被改成 http 明文源，代码也不会拒绝（`crackUrl` 只是据此选端口）。

**修复**（按性价比排序）：
1. `packages.conf` 支持可选的 `名称=URL|sha256` 或 `名称.sha256=...`，下载后校验再解压；
2. 强制要求 https，非 https 直接报错；
3. 下载前弹出 URL 与域名让用户确认。

### P1-10 PostgreSQL 密码明文落盘

**位置**：`src/manager.cpp:968-969`（`iniSet(L"pg.password", ...)`）、`src/main.cpp:1080`

密码以明文写入 `data/settings.ini`，且 PG 页初始化时会把明文回填到密码编辑框
（虽然控件有 `ES_PASSWORD`，但 `SetWindowTextW` 后内容是完整的）。

**修复**：至少用 `CryptProtectData`（DPAPI）加密后存 ini，读取时 `CryptUnprotectData`；
并在 UI 上不要回填明文密码。

### P1-11 nginx 优雅退出被自己的兜底逻辑打断

**位置**：`src/manager.cpp:425-429`

```cpp
RunResult r = runProcessCapture(exe, L"-s quit ...", prefix, 10000);
// fallback: kill by pidfile
DWORD pid = readPid(nginxPidFile(ver));
if (isPidAlive(pid)) killProcessByPid(pid);
```

`-s quit` 是**异步**的优雅关闭（等工作进程处理完请求才退出）。这里发完信号**立刻**检查
PID 存活——此时当然还活着——于是马上 `TerminateProcess`。优雅关闭等于没生效，
正在处理的请求会被硬中断。

**修复**：先轮询等待（比如 3 秒内每 200ms 检查一次），超时后才 kill。

---

## P2 — 一般问题（建议改进）

| # | 位置 | 问题与建议 |
|---|---|---|
| 12 | `downloader.cpp:239-259` | PowerShell 解压 fallback 是死代码：代码注释已自认"env 传参是 no-op"，第一次调用必然失败后走短命令行兜底。且路径含单引号会破坏脚本。**建议**：删掉第一次调用，或把脚本写到临时 `.ps1` 后用 `-File` 执行（彻底避开引号与长度问题）。 |
| 13 | `main.cpp:1497` | `int pct = (int)(done * 100 / total);` — `done` 是 `DWORD`，`done*100` 在超过约 42MB 后**整数溢出**，PostgreSQL 包（~300MB）进度条会乱跳（实测会算成 4%）。**修复**：`int pct = (int)((__int64)done * 100 / total);` |
| 14 | `common.cpp:132-140`、`process.cpp:218-227` | UTF-8 转宽字符失败回退 ACP 时，缓冲区是按 UTF-8 的长度分配的，却写入 ACP 的转换结果，两种编码所需长度不一致时理论上越界写。**修复**：回退分支重新计算长度并重新分配。 |
| 15 | `downloader.cpp:285-311` | 下载/解压失败时未清理临时目录 `%TEMP%\lnpp_dl`（仅复制失败和校验失败时清理）；`progress(L"下载中",0,0)` 未判空（`pkgsDownload` 内部判了，这里没判）。 |
| 16 | `manager.cpp:547-550`、`576-579` | PG 版本迁移：`MoveFileW` 返回值未检查；恢复失败时无回滚，且错误信息**不含备份文件路径**，用户无从找回数据。**建议**：失败时把 `backupFile` 路径拼进 err。 |
| 17 | `manager.cpp:951-961` | initdb 密码临时文件写在固定路径 `%TEMP%\lnpp_pwfile.tmp`，且 `writeFileText` 返回值未检查——写入失败时 initdb 会读到空/旧口令而程序仍报成功。**建议**：用随机文件名 + 检查返回值。 |
| 18 | `common.cpp:144-157` | `writeFileText` 用 `CREATE_ALWAYS` 直接覆盖，无"临时文件 + 原子替换"。写 `settings.ini` 中途崩溃会留下损坏文件，所有配置丢失。 |
| 19 | `manager.cpp:1086-1165` | 手写 JSON 解析 pm2 `jlist`（字符串内转义、嵌套对象都靠数括号），脆弱；`1152` 行 `if (k == npos) k = obj.find(L"\"restart_time\":");` 是重复的死代码。 |
| 20 | `common.cpp:59-76` | `makeDirs` 逐字符创建，遇到 UNC 路径（`\\server\share\a`）会在第一步 `CreateDirectoryW(L"\\")` 失败并返回 false。 |
| 21 | `main.cpp:1421` | `PkgItem item = st->items[sel];` — `sel` 来自 ListView 选择，未校验 `< items.size()`。 |
| 22 | `process.cpp:137` | `si.hStdInput = GetStdHandle(STD_INPUT_HANDLE)` — GUI 子系统程序该句柄为 NULL，子进程 stdin 无效（P1-4 的 `-Command -` 方案失败也与此有关）。 |
| 23 | `common.cpp:42`、`common.h:42` | 自定义 `std::wstring wsprintf(...)` 与 `windows.h` 的 `wsprintf` 宏同宏名冲突，目前靠两边一致才编译过。建议改名 `fmtw` / `wstrfmt` 消除地雷。 |
| 24 | `downloader.cpp:66-97` | `buildEnvBlock()` 未声明、未使用、且注释提到的 `freeEnvBlock` 根本不存在。要么接入（P1-4 推荐），要么删除。 |
| 25 | `main.cpp:1933-1941` | 单实例互斥体未检查 `CreateMutexW` 返回 NULL 的情形；`hSingle` 从不 `CloseHandle`（进程退出时系统回收，影响很小）。 |
| 26 | `manager.cpp:509-518` | 版本迁移时用 `Sleep(2000)` 硬编码等 PostgreSQL 就绪，慢机器上不够、快机器上白等。**建议**：轮询 `pgRunningVer` 或探测端口。 |

---

## 工程化与仓库卫生

**构建**

- `build.bat:25` 未指定运行时库，默认 `/MD` → 依赖 VC 运行库。对一个宣称"绿色便携"的工具，
  拷到没装 VC Redist 的机器上会直接启动失败。**建议**加 `/MT`（体积换可移植性）。
- 警告级别 `/W3` 偏低，建议 `/W4 /permissive-`，并把已知告警清零。上面的 P0-1（未使用的 `br`）
  在 `/W4` 下大概率会被 C4189/C6001 类分析捕获。
- 无 debug 构建目标，建议加一个 `build.bat debug` 产出带 `/Zi /D_DEBUG` 的版本——
  `process.cpp:109-113` 那段 UI 线程阻塞的调试断言只在 `_DEBUG` 下生效，目前构建不了。

**测试**

- `selftest.cpp:60-61` 硬编码 `pgInit(..., L"17", ...)`、`migtest.cpp` 硬编码 `17` / `18`。
  换台机器装的是 16/17 就必然 FAIL。**建议**：改为自动挑选已安装的两个版本。
- 三个测试都要求 80/5432/6379 空闲且会真实启停服务，无法在 CI 跑。
  建议把纯逻辑部分（`renderTemplate`、`pkgsNameToCompVer`、`compVersions` 排序、
  `parsePm2List`、`ini` 读写）抽成不碰进程的单元测试，这部分是可以秒级验证的。
- `test.bat` 的 `for %%T in (...)` 里 `set FAILED=1` 依赖延迟展开——当前写法在循环外判断
  是安全的，但很容易被后续修改破坏，建议显式 `setlocal enabledelayedexpansion`。

**仓库卫生**

- `.gitignore:18` 忽略了 `packages.conf`，但 README 第 78 行要求根目录存在该文件，
  新克隆的仓库**下载功能完全不可用**。建议提交一个 `packages.conf.example` 并在
  README 说明复制改名。
- `.gitignore:19` 只忽略了单个站点 `etc/nginx/vhosts/jwxt.conf`。今后新增任何站点配置
  都会入库（可能含域名、证书路径等）。建议改为：
  ```gitignore
  etc/nginx/vhosts/*.conf
  !etc/nginx/vhosts/_template*.conf
  ```
- 根目录散落着与项目无关的文件：`2026_out.xlsx`、`高三基础成绩核算_新.xlsx`、
  `_run.py`、`_run_inplace.py`、`_sheet1.json`、`build_sheet2.py`、`wrap_iferror.py`、
  `migtest.exe` / `proctest.exe` / `selftest.exe`。xlsx 和 py 属于另一个项目的临时产物，
  建议移出或加入 `.gitignore`（`git status` 里它们是未跟踪状态，容易误 `git add .`）。
- `etc/` 下有三个 `.bak-p1-20260828` 备份文件未清理。

---

## 建议的修复顺序

| 顺序 | 项 | 理由 | 大致改动 |
|---|---|---|---|
| 1 | P0-1 GDI 泄漏 + 颜色失效 | 用户每天都会撞上，改完立刻可见 | 10 行 |
| 2 | P0-2 INI 竞态 | 随机崩溃，最难排查 | 20 行 |
| 3 | P0-3 SQL 注入 | 安全漏洞 | 30 行 + 校验函数 |
| 4 | P1-4 环境变量传参 | 顺带删掉死代码 `buildEnvBlock` 或接入它 | 30 行 |
| 5 | P1-7 nginx 进程误认 | 会杀掉别人的进程，后果严重 | 20 行 |
| 6 | P1-11 nginx 优雅退出 | 一行顺序调整 | 5 行 |
| 7 | P2-13 进度溢出 / P2-19 死代码 / P2-23 命名冲突 | 顺手清理 | 各 1-5 行 |
| 8 | 构建 `/MT` + `/W4`、packages.conf.example、.gitignore vhosts 规则 | 工程化 | 配置文件 |

1~6 项合计约 120 行改动，可以一次做完再统一验证。
