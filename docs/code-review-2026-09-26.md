# LNPP 组件管理器 — 代码审查报告（第二轮）

- 审查日期：2026-09-26
- 审查范围：`src/` 全部 6 个模块（约 5,700 行）、`build.bat` / `test.bat` / `app.rc` / `.gitignore` / `etc\` 模板 / `README.md` / `docs\superpowers\specs\`
- 审查基线：`master` @ `b9da283`（v1.5.1.0），工作区干净，与 `origin/master` 同步
- 审查方式：静态代码审查 + **现场取证**（读 `logs\lnpp.log`、`data\settings.ini`、实际生成的 `data\redis\5.0\redis.conf`、统计 `data\postgresql\*.old-*` 与 `backup\`）
- 与上一轮（[code-review-2026-09-01.md](code-review-2026-09-01.md)）的关系：那一轮的 P0/P1 绝大部分已确认修复，本次不重复；本文只记录**当前代码仍然存在的问题**，外加运行期取证发现的、静态看代码看不出来的真实故障

## 总体评价

工程水平明显高于一般同规模项目：四层分离干净，注释记录了大量踩坑经验（pm2 守护进程风暴、nginx 相对路径可移植、DPAPI 口令绑定、句柄继承白名单、pidfile 进程镜像校验），`/MT /W4 /permissive-` + 原子写 + 全局 INI 锁这些"对的东西"都做了。

本轮的问题集中在三处：

1. **一个已经在生产日志里反复出现的并发写失败**（P0-1，有 3 天连续实证），根因是原子写用了固定临时文件名；
2. **一个栈缓冲区溢出**（P0-2，`CB_GETLBTEXT` 无长度校验）；
3. **测试套件本身会破坏用户环境**（P1-2），以及长期运行留下的资源堆积（P1-4，磁盘上已经 3.9 GB）。

---

## P0 — 必修

### P0-1 `writeFileText` 固定临时名 + 独占打开，并发写同一目标必失败（**生产日志已复现 3 次**）

**位置**：`src/common.cpp:166-188`（`writeFileText`，关键是 171-174 行）；触发链 `src/manager.cpp:1215-1231`（`genRedisConfig`）← `manager.cpp:780-788`（`compStart(Nodejs)` 顺带启动 Redis）← `src/main.cpp:589-598`（`autoStartComponents`）

**现场证据**——`logs\lnpp.log` 里有连续三天、每次都在管理器启动时刻的同一条错误：

```
2026-09-23 05:41:11 [pm2] 启动 Node.js 前自动启动 Redis 失败: 生成 redis 配置失败
2026-09-24 16:09:27 [pm2] 启动 Node.js 前自动启动 Redis 失败: 生成 redis 配置失败
2026-09-25 05:40:55 [pm2] 启动 Node.js 前自动启动 Redis 失败: 生成 redis 配置失败
```

**为什么是问题**：

- `writeFileText` 的"临时文件 + 原子替换"实现是 `tmp = path + L".tmp"`，然后 `CreateFileW(tmp, GENERIC_WRITE, /*dwShareMode=*/0, ...)`。**share mode = 0 意味着独占打开**。
- `autoStartComponents()` 给 4 个组件各起一个 `detach` 线程并发 `compStart`；其中 Nodejs 线程在 `compStart` 内部又调用 `compStatus(Redis)` + `compStart(Redis)`，**绕过了 `g_ui[i].busy` 的互斥**，与 Redis 自己的自启线程并发进入 `genRedisConfig`。
- 两个线程写同一个 `data\redis\5.0\redis.conf`，临时名都是 `redis.conf.tmp`：先到的线程持有独占句柄写文件，后到的 `CreateFileW` 直接 `ERROR_SHARING_VIOLATION` → `writeFileText` 返回 false → `compStart(Redis)` 报"生成 redis 配置失败"。（杀毒软件扫描新建文件同样会造成这个 sharing violation，是同一类根因。）
- 只有 Nodejs 那条路径会 `logMsg`（`manager.cpp:786`），Redis 自己的自启线程失败只进 UI 日志框，所以日志里只看得到这一条 —— 这也解释了为什么"只有这一种操作报错"。

**影响**：开机自启时 Redis 经常起不来（错误只有一行日志，UI 上也可能一闪而过）；同样的模式在 `genNginxConfig`（`manager.cpp:1200`）和 `genPgConfig`（`manager.cpp:1244`）上成立，只是目前调用时序不容易撞上。

**修复**（三处都要，缺一个还会复发）：

1. `writeFileText` 的临时名唯一化：

```cpp
std::wstring dir = dirOf(path);
std::wstring tmp;
if (!GetTempFileNameW(dir.c_str(), L"lnp", 0, tmpBuf)) return false;  // 与目标同目录，保证同卷原子 rename
std::wstring tmp(tmpBuf);
```

2. 遇 `ERROR_SHARING_VIOLATION` / `ERROR_ACCESS_DENIED` 重试 2~3 次（每次换新随机名），并把 `GetLastError()` 带进返回值——现在错误被完全吞掉，无法定位；
3. `compStart(Nodejs)` 里"顺带启动 Redis"要走组件级互斥（和 `runAsync` 同一把锁），不要直接递归调 `compStart(Comp::Redis)`。

> 顺带：`genRedisConfig` / `genNginxConfig` 建议加一个进程级"配置生成"互斥量，成本极低。

### P0-2 版本下拉框 `CB_GETLBTEXT` 无长度校验写进 128 字节栈缓冲

**位置**：`src/main.cpp:489-493`

```cpp
int idx = (int)SendMessageW(ui.verCombo, CB_GETCURSEL, 0, 0);
if (idx < 0) return;
wchar_t buf[128];
SendMessageW(ui.verCombo, CB_GETLBTEXT, idx, (LPARAM)buf);   // 控件不知道 buf 有多大
std::wstring ver = buf;
```

`CB_GETLBTEXT` 按调用方"保证缓冲区够大"来拷贝，**不检查目标容量**。条目文本来自 `bin\<组件>\<目录名>`（`compVersions` 扫目录得到），只要磁盘上存在一个名字 ≥128 字符的版本目录，点"切换版本"就是栈缓冲区溢出。

**修复**：

```cpp
int n = (int)SendMessageW(ui.verCombo, CB_GETLBTEXTLEN, idx, 0);
if (n < 0) return;
std::vector<wchar_t> buf((size_t)n + 1);
SendMessageW(ui.verCombo, CB_GETLBTEXT, idx, (LPARAM)buf.data());
std::wstring ver = buf.data();
```

同类调用（`ListView_GetItemText`、`GetDlgItemTextW`）都带 `nMaxCount`，是安全的；只有这一处漏了。**审计结论：全项目仅此一处无界拷贝。**

---

## P1 — 重要

### P1-1 组件操作名 `ui.opName` 存在数据竞争

**位置**：`src/main.cpp:243-244`（`beginOp` 写）、`252-258`（`endOp` 读）、`263`（`runAsync` 判 `busy`）

```cpp
static void endOp(Comp c, bool ok, const std::wstring& msg) {
    ui.busy = false;                                            // ← 先放开闸门
    logMsgUi(c, ok ? (L"==> " + ui.opName + L" 完成") : ...);  // ← 才读 opName
```

工作线程先把 `busy` 置 false，UI 线程随即可能在定时器/按钮路径里通过 `runAsync` 的 `if (ui.busy) return;` 检查，进入 `beginOp` 写 `ui.opName`（`std::wstring` 赋值）。两个线程对同一个 `std::wstring` 一读一写 = 未定义行为，堆损坏风险。窗口窄的时候很容易撞上（1ms 级窗口）。

**修复**：`endOp` 开头先取快照 `std::wstring name = ui.opName;` 再用；更彻底的做法是 `runAsync` 在起线程时把 `name` 按值捕获，`endOp` 完全不再碰 `ui.opName`。同时把 `ui.busy = false` 从 `endOp` 里删掉，统一由 UI 线程的 `WM_OP_DONE`（`main.cpp:1926`）来清——现在两个地方各清一次，语义重复。

### P1-2 `test.bat` 默认会重建生产数据库、停掉生产应用

**位置**：`test.bat:66`（默认跑全套）、`src/migtest.cpp`、`src/proctest.cpp`、`src/selftest.cpp`

三个集成测试都不是"测试"，而是对**用户真实环境**做破坏性操作：

| 测试 | 实际做的事 |
|---|---|
| `migtest` | 对真实 `data\postgresql\<ver>` 做 dump → initdb → restore 的**版本迁移**（不可逆），跑完把 PG 停在 stopped |
| `proctest` | `pm2 resurrect` 拉起用户的真实应用，最后 `compStop(Nodejs)` = `pm2 kill`，**把 jwxt / jwpk 停掉** |
| `selftest` | 把 nginx 切到另一个版本且**不还原**（收尾时是 stopped） |

`backup\` 目录里已经能看到测试留下的痕迹：09-22 一天之内 6 份 8.9 MB 的 dump，其中两对（`065605`/`065652`、`071017`/`071017`）只隔 47 秒——就是反复跑迁移测试留下的。

**修复**：

- `test.bat` 默认只跑 `unittests`（无副作用），破坏性测试挪到 `test.bat destructive`；
- 破坏性测试在开始前自动 `pgBackup` 一份并在结束时提示恢复路径；
- `selftest` 记下原始版本，收尾时切回去（现在是"切过去就不回来了"）；
- README「测试」一节加粗警告：**不要在有生产数据的机器上直接 `test.bat all`**。

### P1-3 `migtest` 断言了它自己从不创建的数据，且"跳过"被报成 PASS

**位置**：`src/migtest.cpp:29-33`、`:42-43`

```cpp
if (vers.size() < 2) {
    wprintf(L"...跳过。\n");
    wprintf(L"FAILURES: 0\n");
    return 0;          // ← test.bat 判定为 [PASS] migtest.exe
}
```

一个字节都没测的测试报 PASS，属于最坏的假通过：CI/脚本全绿，问题却从没被测过。

`:42` 更直接：`check(L"seed data present", queryVer(vOld, L"appdb", L"SELECT v FROM t WHERE id=1;", out) && ...)` —— 测试假设 `appdb.t(id=1,'hello')` 已经存在，但**代码里从没有任何地方创建它**。换一台机器（没有这个库）第一条断言就 FAIL，报错还完全指不到真因。

**修复**：让测试自己 `CREATE TABLE IF NOT EXISTS` + `INSERT ... ON CONFLICT DO NOTHING` 造种子数据；"跳过"用独立退出码（如 `return 2`），`test.bat` 里 `[SKIP]` 而非 `[PASS]`。

### P1-4 迁移/备份产物无限堆积（磁盘上已经 3.9 GB）

**位置**：`src/manager.cpp:972-979`（`MoveFileW(newData, newData + L".old-" + stamp)`）、`:946-947`（每次迁移都 `pgBackup`）

现场统计：

- `data\postgresql\` 下有 **32 个 `.old-<时间戳>` 目录，合计 3.90 GB**（最早 2026-08-19，最新 2026-09-22）；
- `backup\` 下每次迁移留一份全量 dump，09-22 一天就 6 份。

每做一次 PG 版本切换就 +一份完整数据副本（~120 MB）+一份全量 dump（~9 MB），**没有任何清理策略**，会无限增长。

**修复**：

- 迁移成功后只保留最近 1~2 份 `.old-*`，更老的移到"待清理"或直接删（并在确认新库可用之后）；
- `backup\` 同样加"保留最近 N 份"策略；
- 或者退一步：在 `data\postgresql` 加一个 `keep N` 设置 + 启动时提示"可释放 X GB"。

### P1-5 生成的 redis 配置监听 0.0.0.0 且无口令

**位置**：`etc\redis\redis.conf.tpl`（9 行）、`src/manager.cpp:1115-1124`（`DEFAULT_REDIS_CONF`）

模板和内置默认值**都没有 `bind` 指令**。Redis 5.0 在没有 `bind` 时默认绑定所有网络接口。实测 `data\redis\5.0\redis.conf`：

```
port 6379
pidfile ../../../data/redis/5.0/redis.pid
logfile ../../../logs/redis-5.0.log
dir ../../../data/redis/5.0
appendonly yes
save 900 1 ...
```

即 **0.0.0.0:6379 + [::]:6379，无 `requirepass`**。这台机器在 192.168.10.3 的局域网里，同网段任何人可以直连——未授权 Redis 是教科书级的接管漏洞（`CONFIG SET dir/dbfilename` 写 SSH 公钥、加载模块、写计划任务）。

**修复**（一行）：模板加 `bind 127.0.0.1`。`jwxt` / `jwpk` 的 `REDIS_URL` 都是 `127.0.0.1`，加了不影响任何东西。顺带把 `dir` 单独一行、日志路径确认可写。

### P1-6 读取用户 PATH 失败时，`加入用户 PATH` 会把 PATH 覆盖成只剩 node 目录

**位置**：`src/main.cpp:632-651`（`pathHasNode`）、`:653-685`（`pathAddNode`）

```cpp
LONG r = RegQueryValueExW(hk, L"Path", nullptr, &type, (LPBYTE)buf, &size);
if (r != ERROR_SUCCESS) { RegCloseKey(hk); path.clear(); return true; }   // ← 失败被当成"空 PATH"
```

返回 `true` 表示"读到了"，但 `path` 是空的。`pathAddNode` 于是执行 `np = "" ; np += nodeDir; RegSetValueExW(..., "Path", np)` —— **用户的整个 PATH 被替换成一个条目**。

触发条件：PATH 超过 32768 字符（`ERROR_MORE_DATA`）、值类型异常、或读取被策略拦截。概率不高，但后果是"命令行里所有程序都找不到"的灾难级，而且发生在写路径的按钮上。

**修复**：把"键不存在"和"读取失败"分开返回（`enum class PathResult { Ok, Missing, Failed }`），`Failed` 时**不写**，只提示"读取用户 PATH 失败，未做修改"。

### P1-7 UI 线程上调用 `compStatus()`，会同步拉起 helper 进程导致界面冻结

**位置**：`src/main.cpp:494`（`actSwitch`）、`:1224`（`pgOpInit`）、`:1788`（`IDC_BTN_DATA`）

`compStatus()` → `compStatusImpl(c, /*quick=*/false)` → `compIsRunning()`，其中 redis 走 `redisRunningVer`（可能 `redis-cli` 探测）、nginx 走 pidfile 修复、nodejs 走 pm2 探测。这些都可能进 `runProcessCapture`，默认超时 60s。

项目自己已经在 `src/proc.h:21-24` 和 `process.cpp:117-121` 写了 `isUiThread()` 的 debug 断言来防这个坑，注释也写明 "Synchronous component ops like pgBackup / compStart can take many seconds and should never run on this thread"。**这三处正是漏网的调用点**。

**修复**：这三处改用 `compStatusQuick()`（不 spawn 进程），或把整个动作丢到 `runAsync` 里做。

### P1-8 `pkgsNameToCompVer` 不校验版本号 → `..` 路径穿越；下载不强制 https

**位置**：`src/downloader.cpp:89-97`、`:147-161`、`:325-334`

```cpp
size_t dash = name.find(L'-');
comp = lowerStr(name.substr(0, dash));
ver  = name.substr(dash + 1);          // ← 没有任何白名单
if (ver.empty()) return false;
return comp == L"nginx" || ... ;
```

`nginx-../../evil` 完全合法：`comp=nginx`、`ver="../../evil"`，于是

- `target = bin\nginx\..\..\evil` → `shCopyDir` 把解压内容写到**运行根目录之外**；
- `zipFile = %TEMP%\lnpp_dl\nginx-../../evil.zip` → 下载落到 `%TEMP%\evil.zip`。

同时 `crackUrl()` 解析出的 `secure` 只用来选端口，**没有任何地方拒绝 `http://`**；而 sha256 校验是可选的，`packages.conf.example` 里**一行 sha256 都没有**。合起来的效果是：改一行 `packages.conf` 就能让管理器下载一个任意来源的 zip、解压到任意目录、随后以当前用户权限执行其中的 `nginx.exe` / `initdb.exe`。

**修复**（三处都便宜）：

1. `ver` 白名单：`^[0-9A-Za-z][0-9A-Za-z._+-]*$` 且显式拒绝含 `..` / `\` / `/` / `:` 的值；`item.name` 同理；
2. `pkgsDownload` 里 `if (!secure) { err = L"仅支持 https 下载源"; return false; }`；
3. `packages.conf.example` 至少给一个条目补上 `.sha256=` 示例（示例文件的作用就是示范最佳实践）。

### P1-9 `selftest` 在只装了一个 nginx 版本时必然误报失败

**位置**：`src/selftest.cpp:36-38`

```cpp
if (target.empty()) target = st.currentVersion;   // 只装了一个版本
check(tname.c_str(), compSwitchVersion(Comp::Nginx, target, err), err);
```

`compSwitchVersion` 第一件事就是 `if (st.currentVersion == ver) { err = L"已是当前版本"; return false; }`（`manager.cpp:903`）——**必然 FAIL**，而且错误信息是"已是当前版本"，会让人以为测试逻辑坏了。这里应该是"跳过"，不是"失败"。`proctest.cpp:32-36` 的同类分支就写对了（打印 skipping 后正常返回），两处风格不统一。

---

## P2 — 一般建议

| # | 位置 | 问题与建议 |
|---|---|---|
| 1 | `manager.cpp:457-476` + `518-563` | `nodePm2Cmd` 的 `pm2.js` 回退分支**永远失败**：把 `.js` 路径当 exe 传给 `CreateProcessW` → `ERROR_BAD_EXE_FORMAT`，用户只会看到"创建进程失败 (错误 193)"。要么改成 `node.exe <pm2.js> <args>`，要么删掉这个分支并在注释里说明只支持 `pm2.cmd`。 |
| 2 | `manager.cpp:432` | `g_pm2DownUntil` 是跨线程读写的裸 `DWORD`（pm2 轮询线程 + 各操作线程）。x86 上实际无害，但形式上是数据竞争，建议 `std::atomic<DWORD>`。 |
| 3 | `process.cpp:250-262`、`common.cpp:154-162` | UTF-8 → UTF-16 的"失败回退 ACP"是**死代码**：`MultiByteToWideChar(cp, 0, ...)` 不带 `MB_ERR_INVALID_CHARS`，遇到非法序列会替换成 `'?'` 而不是返回 0，`len <= 0` 永远不成立。要么加 `MB_ERR_INVALID_CHARS` 让回退真正生效，要么删掉回退分支。 |
| 4 | `manager.cpp:868` | `redisPort()` 直接拼进 `redis-cli` 命令行，**没有 `validPort()` 校验**——而同一文件里 `pgPort()` 在 4 处都校验了。不一致，建议统一。 |
| 5 | `manager.cpp:1640-1657` | `pgInit` 不校验密码非空，空口令会创建一个无口令的 superuser（配合 `--auth scram-sha-256` 仍然是无口令账号）。建议 `password.empty()` 直接拒绝。 |
| 6 | `downloader.cpp:332` | 每次安装都先 `shDeleteTree(%TEMP%\lnpp_dl)`：固定可预测路径、并发安装互相删除、（本地）junction 劫持可让删除落到别处。建议改成每次运行唯一的子目录 `lnpp_dl\<pid>-<tick>`。 |
| 7 | `downloader.cpp:299-301` | PowerShell 回退把 zip 路径用单引号拼进 `.ps1`（`Expand-Archive -LiteralPath '<path>'`）。Windows 用户名是允许 `'` 的（O'Brien），会直接语法错。建议改用 `-LiteralPath` + 参数化或对单引号做转义。 |
| 8 | `downloader.cpp:212-233` | 下载无大小上限，也无"剩余磁盘空间"预检。300 MB 的包下到一半磁盘满，只留一句"写入临时文件失败（磁盘空间不足？）"。 |
| 9 | `main.cpp:143-149` | `logAppendRaw` 对日志框只追加不裁剪。托盘常驻数周、频繁操作时编辑框内容无限增长（单进程内），建议超过 N 行时裁掉开头。 |
| 10 | `app.rc` | **没有嵌入 manifest**：既没有 `<dpiAware>`/Per-Monitor，125%/150% 缩放下整个界面被系统位图拉伸发虚；也没有 comctl32 v6 声明，ListView / Tab / Progress 会是 Win95 老式外观。建议加一个 `app.manifest`（`dpiAware=true` + `dependency comctl32 v6`）并在 `app.rc` 里 `1 24 "app.manifest"`。 |
| 11 | `main.cpp:1597`、`1603` | `swprintf(buf, 64, ...)` 不是 `_snwprintf_s`，靠 MSVC 的截断行为兜底。`st->curName` 来自 `packages.conf`（长度不受控），建议 `_snwprintf_s(buf, _countof(buf), _TRUNCATE, ...)`。 |
| 12 | `manager.cpp:1351` | `nginxListVHosts` 用 `content.find(L"root")` 找根目录，会命中 `document_root` 之类的子串。建议先找 `"\n    root"` 或跳过 `_template` 之外再匹配。 |
| 13 | `docs/superpowers/specs/2026-08-19-lnpp-manager-design.md` | 文档与代码已漂移：§5 说停止用 `GenerateConsoleCtrlEvent`（代码没有）、§6 说迁移用 `pg_dump -Fc` + `pg_restore`（实际是 `pg_dumpall` + `psql -f`）、§2/§9 说"软件不负责下载"（已有下载器，且有第二份设计文档）。建议在这份文档顶部加一行"已实现细节见 README"或直接更新。 |
| 14 | `README.md` 「说明」 | 补一条：每次 PostgreSQL 版本切换会在 `data\postgresql\<版本>` 旁边留下 `.old-<时间戳>` 副本（当前已累积 3.9 GB），可手工清理。 |
| 15 | `packages.conf.example` | 全文没有一行 `.sha256=` 示例，新克隆仓库照抄就等于默认关闭完整性校验。建议至少示范一条。 |
| 16 | `scripts/check-cert.ps1:58-59` | nginx 路径硬编码 `bin\nginx\1.30` / `data\nginx\1.30`，而本机实际装的是 **1.30.4** → `if (Test-Path $nginxExe)` 直接为假，**证书脚本里的 `nginx -t` 校验从来没有真正跑过**（日志里 `nginxTest=True` 是默认值而不是检测结果）。整份脚本还硬编码 `D:\code\lnpp`，与"绿色便携"冲突。 |
| 17 | `scripts/check-cert.ps1:69-72` + `lnpp.exe` | 告警写到 `logs\cert-alert.txt` 和控制台，但 **lnpp.exe 里没有任何代码读这个文件**——脚本的告警到不了用户眼前，只能靠计划任务的窗口。建议要么让 lnpp 轮询这个哨兵文件弹气泡，要么改用 `msg`/BurntToast。另外当前证书 2026-10-13 到期（距今 17 天），已经快进入 14 天告警窗口。 |

---

## 工程化与仓库卫生（复核结论）

| 项 | 结论 |
|---|---|
| 仓库卫生 | ✅ 干净。`git status` 无改动，33 个跟踪文件，`bin/ data/ backup/ logs/ ssl/ *.exe packages.conf .workbuddy/` 全部正确忽略；`etc/nginx/vhosts/*.conf` 的"忽略 + 反向包含模板"规则有效（`jwxt.conf`/`pk.conf` 未入库） |
| 构建 | ✅ `build.bat` 有 release/debug 双目标、`/MT` 静态 CRT、`/W4 /permissive- /utf-8`、`errorlevel` 检查完整、有 `LNK1104` 的友好提示 |
| 测试 | ⚠️ 见 P1-2 / P1-3 / P1-9。`unittests` 质量不错（覆盖 `renderTemplate` / `naturalGt` / `parsePm2List` / INI 文本往返，且断言真实有效），但**`pkgsParseConf` 完全没有测试**——而它是整个下载器里最容易写错的一段（分组、`---`、`.sha256` 伴随项的先后顺序） |
| 文档 | ⚠️ README 与代码基本一致（版本号 1.5.1、`/MT`、日志轮转阈值、DPAPI 说明都对得上），见 P2-13 |

### 建议补充的测试（按性价比排序）

1. **`pkgsParseConf`**——先做和 `parseIniText` 一样的拆分（`pkgsParseConfText(text, err)` + 读文件的薄壳），然后覆盖：无 `---`、连续两个 `---`、空组、`.sha256` 写在包**之前**和**之后**两种顺序、同名 sha256 覆盖、注释里带 `=` 的行、URL 里带 `=`（`?fileid=1259297` 这种）。这是投入产出比最高的一条。
2. **`writeFileText` 并发/失败**——写一个"两个线程同时写同一个目标"的用例（现在必挂），以及目标目录不存在、目标是只读文件、磁盘满三种失败路径的返回值断言。
3. **`dpProtect` / `dpUnprotect` 往返**——含中文、空串、含 `=`/换行（base64 边界）、以及"密文尾部带 0x00 的历史格式仍可解"这条兼容路径。
4. **`naturalGt` 边界**——`1.30` vs `1.30.0`、`v1.2` vs `1.2`、空串、纯数字超长。
5. **`renderTemplate` 边界**——`{{}}`、嵌套 `{{ {{X}} }}`、值里含 `{{`、模板里有 lone `{`。
6. **`readFileText` 编码**——纯 ASCII、UTF-8 中文、UTF-8 BOM（现在会把 BOM 当内容，`parseIniText` 的第一个 key 会带上 `\ufeff` 前缀，值得单独测一下）。

---

## 建议的修复顺序

| 顺序 | 项 | 理由 | 大致改动 |
|---|---|---|---|
| 1 | P0-1 `writeFileText` 临时名唯一化 + 重试 + 记错误码 | 生产日志里天天在发生，直接影响开机自启 | `common.cpp` 约 20 行 + 一把互斥量 |
| 2 | P0-2 `CB_GETLBTEXT` 长度校验 | 栈溢出，改动最小 | `main.cpp` 约 5 行 |
| 3 | P1-5 redis `bind 127.0.0.1` | 一行，堵住局域网未授权访问 | 模板 1 行 + `DEFAULT_REDIS_CONF` 1 行 |
| 4 | P1-1 `opName` 竞争 | 随机堆损坏 | `main.cpp` 约 5 行 |
| 5 | P1-2 / P1-3 测试不破坏环境 + 不假通过 | 用户会真的在生产机上跑 `test.bat` | `test.bat` + 3 个测试文件 |
| 6 | P1-4 清理 `.old-*` / `backup` 策略 | 磁盘已经 3.9 GB | `manager.cpp` 约 30 行 |
| 7 | P1-6 / P1-7 / P1-8 PATH 覆盖、UI 冻结、路径穿越 | 三个都是"改几行"的正确性/安全修复 | 各 5~20 行 |
| 8 | P1-4 之外的 P2 + manifest + pkgsParseConf 测试 | 长期质量 | 见上 |

1~4 项合计约 30 行改动，可以一次做完再统一验证。
