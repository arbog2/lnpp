# LNPP 组件管理器 v1.5.3

Windows 原生 C++ (Win32) 桌面工具，管理 nodejs / nginx / postgresql / redis 的启动、停止、版本切换、配置。

## 目录结构

**`etc\` 只放模板，`data\` 放一切运行时状态。** 这是本项目的硬性约定：凡是需要用户
编辑、或由程序写出的文件，一律在 `data\`；`etc\` 里的东西随发布包分发、只读。

```
lnpp.exe
├── bin\                      # 组件二进制（每个组件下按版本分子目录）
│   ├── nodejs\<版本>\         # 手动拷贝 node.exe、npm 等
│   ├── nginx\<版本>\          # 手动拷贝 nginx.exe、conf\mime.types 等
│   ├── postgresql\<版本>\     # 手动拷贝整个 zip 解压目录（含 bin\）
│   └── redis\<版本>\          # 手动拷贝 redis-server.exe、redis-cli.exe
├── etc\                      # ← 只放模板，随包分发、只读
│   ├── nginx\nginx.conf.tpl
│   ├── nginx\vhosts\_template.conf
│   ├── nginx\vhosts\_template_https.conf
│   ├── postgresql\postgresql.conf.append
│   ├── redis\redis.conf.tpl
│   └── packages.conf.tpl     # 组件下载源模板
├── data\                     # ← 全部运行时状态，缺失时首次运行自动创建
│   ├── settings.ini          # 版本选择、端口、DPAPI 加密的库口令
│   ├── packages.conf         # 组件下载列表（首次运行由 etc 模板生成，可编辑）
│   ├── nginx\
│   │   ├── vhosts\*.conf     # 站点配置（用户数据；在总览/站点页增删）
│   │   └── <版本>\           # 该版本的运行配置 conf\nginx.conf + conf\vhosts\
│   ├── postgresql\<版本>\    # 数据目录 + postgresql.conf
│   └── redis\<版本>\         # redis.conf + 数据
├── backup\                   # 数据库备份（pg_dumpall 输出，时间戳命名）
├── logs\                     # 运行日志
├── ssl\                      # TLS 证书与私钥
└── www\                      # nginx 站点根目录
```

**首次运行**若 `data\` 不存在会自动创建，同时：

1. 从 `etc\packages.conf.tpl` 生成 `data\packages.conf`（组件下载源列表，可直接改）；
2. 把旧的 `etc\nginx\vhosts\*.conf` 站点配置迁到 `data\nginx\vhosts\`（v1.5.3 之前
   站点配置和模板混在 `etc\` 里）。迁移完成后 `etc\` 只剩模板，文件内容逐字节保留。

站点配置的**源目录**是 `data\nginx\vhosts\`，启动时再复制到每个版本的运行前缀
（`data\nginx\<版本>\conf\vhosts\`）——因为 nginx 的 `include vhosts/*.conf` 是相对
`-p` 前缀解析的。手工改配置请改 `data\nginx\vhosts\` 下的文件。

## 手动拷贝组件

程序不负责下载组件。你需要把各组件便携版拷贝到 `bin\`：

| 组件 | 期望目录 | 关键文件 |
|---|---|---|
| nodejs | `bin\nodejs\<版本>\` | node.exe, npm.cmd, (可选 pm2.cmd) |
| nginx | `bin\nginx\<版本>\` | nginx.exe, conf\mime.types |
| postgresql | `bin\postgresql\<版本>\` | bin\pg_ctl.exe, bin\initdb.exe, bin\psql.exe, bin\pg_dumpall.exe |
| redis | `bin\redis\<版本>\` | redis-server.exe, redis-cli.exe |

多个版本目录并存即可在下拉框中切换版本。PostgreSQL 版本切换会自动做数据迁移（pg_dumpall 备份 → initdb → 恢复）。

## 功能

- 总览页：三态状态灯（灰=未安装 / 红=已停止 / 绿=运行中）、单组件启动/停止、全部启动/重启/停止
- 各组件启动/停止，状态灯实时显示
- 版本切换（自动重启 + 重新生成配置）
- nginx：可视化添加/删除虚拟站点（写 data\nginx\vhosts\），支持 HTTPS（证书 + key），www\ 下自动建目录
- nginx：**拒绝 IP 直连与未配置域名**——主配置里有一对 `default_server` 兜底块（见下）
- PostgreSQL：初始化、改密码、创建/删除用户、备份（pg_dumpall）
- Node.js：pm2 进程列表实时监控，支持重启/停止；一键将当前 Node 版本加入/移除用户 PATH
- pm2 守护进程加固：所有 pm2 操作先校验守护进程存活（校验 `pm2.pid` 的进程镜像，避免 PID 复用误判），只读命令在守护进程不在时不调用 pm2；写入命令遇到 `rpc.sock` 握手失败（`EPERM`/`EPIPE`，通常由守护进程被强制结束、`pm2.pid` 残留引起）会自动等待并重试一次，仍失败时给出可操作提示而不是原始堆栈
- 组件下载：首启（bin 为空）自动弹出，或在总览页点「下载组件」；地址读 `data\packages.conf`（首次运行由 `etc\packages.conf.tpl` 生成，可直接改），下载完成后自动解压到 bin\<组件>\<版本>
- 「配置」按钮：打开该组件的**运行时**配置目录（`data\` 下）。nginx 是 `data\nginx\vhosts\`（站点配置源，**不是**每版本的运行副本——那份每次启动都会被覆盖）；PostgreSQL / Redis 是当前版本的数据目录；Node.js 是 `data\nodejs\`，并提示 pm2 自己的文件在 `%USERPROFILE%\.pm2`
- 常驻托盘：关闭按钮只最小化到托盘（组件继续运行），退出请用托盘右键菜单「退出」
- 随管理器自动启动（总开关 + 每组件勾选）、随 Windows 开机启动（`--hidden` 直接最小化到托盘）
- 总览页右下角：版本号 + 「关于」弹窗（作者、可点击 GitHub 链接）

## 配置模板

`etc\` 下的 `.tpl` / `.append` 文件是配置模板，支持 `{{VAR}}` 占位符：

- `etc\nginx\nginx.conf.tpl` — {{PORT}} {{WWW_DIR}} {{MIME}}
- `etc\nginx\vhosts\_template.conf` — 虚拟站点模板 {{PORT}} {{DOMAIN}} {{ROOT}}
- `etc\redis\redis.conf.tpl` — {{PORT}} {{PIDFILE}} {{LOGFILE}} {{DIR}}
- `etc\postgresql\postgresql.conf.append` — 追加到 postgresql.conf 的覆盖项 {{PORT}}

## 构建

需要 Visual Studio Build Tools (C++ 工作负载)。运行：

```
build.bat
build.bat debug      # 产出 lnpp_dbg.exe（/Od /Zi /D_DEBUG），用于调试
```

输出 `lnpp.exe` 到仓库根目录（exe 所在目录即运行时根目录）。

## 发布打包

发布说明存档在 [`docs\releases\`](docs\releases/README.md)，与 GitHub Releases 上的正文一致。

```
package.bat              # 编译 + 打包，版本号自动取自 app.rc 的 FILEVERSION
package.bat 1.5.3        # 指定版本号（不改 app.rc）
package.bat /nobuild     # 不编译，用现有 lnpp.exe
```

产出 `dist\LNPP-<版本>.zip`（约 291 KB），内含：

```
LNPP-1.5.3/
  lnpp.exe
  README.md
  etc\nginx\nginx.conf.tpl
  etc\nginx\vhosts\_template.conf
  etc\nginx\vhosts\_template_https.conf
  etc\packages.conf.tpl
  etc\postgresql\postgresql.conf.append
  etc\redis\redis.conf.tpl
  www\.gitkeep
```

包里的 `etc\` **只有模板**——`etc\nginx\vhosts\` 里连用户的站点配置都没有，因此打包时
不可能误带内网域名、绝对路径或证书。

**按设计不包含**：`bin\`（组件二进制，几 GB，由用户自带或用下载器装）、`data\`
（运行配置、`settings.ini` 里的 DPAPI 加密库口令、站点配置）、`logs\`、`backup\`
（数据库 dump）、`ssl\`（私钥）。

打包走白名单逐个拷贝，压缩前再核验一遍暂存目录：发现上述任一项就中止报错。删除暂存
目录前会检查目录内的 `.lnpp-stage` 标记，标记不对就拒绝删除。

解压即用：`lnpp.exe` 只依赖 11 个系统 DLL（`/MT` 静态 CRT，**不需要**装 VC 运行库），
所有路径相对 exe 推导，绿色免安装。首次启动时 `bin\` 为空会自动弹出组件下载器——
首次运行会从 `etc\packages.conf.tpl` 自动生成 `data\packages.conf`，改那里的 https 源即可。

## 测试

测试分两类，**请先看清再跑**：

| 命令 | 跑什么 | 是否动真实环境 |
|---|---|---|
| `test.bat run` / `test.bat all` | 只跑 `unittests`（纯逻辑，不碰进程和端口） | 否 |
| `test.bat selftest` | 真实启停 nginx / PG / Redis，**切换 nginx 版本并在结束时还原** | 是 |
| `test.bat proctest` | `pm2 resurrect` 后再 `pm2 kill`——**会把你的应用停掉** | 是 |
| `test.bat migtest` | 对**真实数据库**做版本迁移（pg_dumpall → initdb → 恢复） | 是（不可逆） |
| `test.bat destructive` | 上面三个全跑 | 是 |
| `test.bat build` | 只编译四个测试程序到仓库根目录 | 否 |

> ⚠️ `migtest` 会重建 `data\postgresql\<版本>`。在有真实数据的机器上跑之前请先备份。
> 退出码：`0` 通过 / `1` 失败 / `2` 跳过——**跳过不会被算作通过**。

`unittests` 覆盖 `renderTemplate`、`packages.conf` 语法、`pkgsNameToCompVer`（含路径穿越
拒绝）、版本排序、pm2 `jlist` 解析、INI 文本往返、`writeFileText`（含 8 线程并发写同一
文件）、DPAPI 加解密往返、文件编码。所有临时文件都在 `%TEMP%` 并自动清理。

## 拒绝 IP 直连与未配置域名

多个域名共存时，浏览器直接输 IP、或输一个没配置的域名，请求原本会落到「端口上第一个
server 块」——也就是某个业务站点。现在由主配置里的一对兜底块接管：

```nginx
server {                                  # :80  未匹配的 Host
    listen       80 default_server;
    server_name  _;
    return 500;
}
server {                                  # :443 未匹配的 SNI
    listen       443 ssl default_server;
    server_name  _;
    ssl_reject_handshake on;              # 握手阶段拒绝，不需要证书
}
```

- **站点永远不要写 `default_server`**。它会让未知域名命中该站点，而且和上面的兜底块
  冲突，nginx 会直接报 `a duplicate default server for 0.0.0.0:443` 而起不来。
  模板 `_template_https.conf` 已去掉该关键字；管理器每次生成配置时会扫描
  `data\nginx\vhosts\*.conf`，发现 `default_server` 就在 `logs\lnpp.log` 里点名是哪个文件第几行。
- **443 上拿不到 HTTP 500**，这是有意的：`ssl_reject_handshake` 在 TLS 握手阶段就拒绝，
  不需要给兜底块配证书。真给兜底块挂证书，浏览器只会先弹证书域名不匹配，点「继续」之后
  才看到 500——比干脆利落地拒绝更糟。curl 上表现为连接失败（`%{http_code}` = 000）。
- 兜底块固定占用 80 / 443，但这**不算端口冲突**：站点和兜底块靠 Host / SNI 区分，
  站点照常可以放在 80 / 443 上。
- **一键回退**：在 `data\settings.ini` 里写 `nginx.block_unknown_host=false`，重启管理器即
  恢复成「只保留 `http://localhost/` 本机站点」的老行为。
- 自定义过 `etc\nginx\nginx.conf.tpl` 的话，模板里必须有一行 `{{DENY_UNKNOWN}}`（放在
  `http { }` 内、`include vhosts/*.conf;` 之前）。没有这一行兜底块不会生成，而且不会
  报错——管理器会在 `logs\lnpp.log` 里明确提示。

## 说明

- 所有路径基于 exe 所在位置推导，绿色便携
- PostgreSQL 默认端口 5432，redis 6379，nginx 80；在 `data\settings.ini` 中可改
- PostgreSQL 密码用 DPAPI（绑定当前 Windows 用户）加密后存 `data\settings.ini` 的 `pg.password.enc`；加密串换机器或换用户后无法解密，需重新填写密码
- 日志轮转：管理器日志 `logs\lnpp.log` 超过 4MB 自动轮转为 `lnpp.log.1`；nginx 的 `access.log` / `error.log`（`data\nginx\<版本>\logs\`）与 `logs\postgresql-<版本>.log`、`logs\redis-<版本>.log` 超过 8MB 时在**组件下次启动前**轮转为 `.1`/`.2`（保留 2 份）。Windows 下运行中的服务会独占日志文件，无法改名，所以要等组件停止时才能轮转；pm2 自己的日志在 `%USERPROFILE%\.pm2\logs`，由 pm2 管理
- 首次使用 PostgreSQL：切到对应页签点「初始化数据库」（密码不能为空）
- 组件下载源：根目录 `packages.conf` 按组列出（`# 标题` `---` 分隔，条目 `名称=URL`），程序只认 nginx/nodejs/postgresql/redis 组件
  - **只接受 `https://`**，明文源会被直接拒绝
  - 名称中 `-` 之后是版本号，只允许字母/数字/`.`/`_`/`-`/`+`（该值用作 `bin\<组件>\<版本>` 目录名）
- 可选完整性校验：在 URL 条目下方加一行 `名称.sha256=64位hex`（用 `certutil -hashfile <文件> SHA256` 生成），下载后自动比对，不匹配则拒绝安装；没有该行时跳过校验
- **PostgreSQL 版本切换会留下旧数据副本**：每次切换在 `data\postgresql\<版本>` 旁生成
  `.old-<时间戳>`（约 120MB/份），并在 `backup\` 留一份全量 dump。管理器现在自动把它们
  **移入回收站**（可撤销），只保留最近若干份。保留数量可在 `data\settings.ini` 调整：
  - `keep.datacopies=2` — 保留最近 N 份旧数据目录（默认 2）
  - `keep.backups=10` — 保留最近 N 个 `backup\*.sql`（默认 10）
  手工清理也可以：确认新集群正常后，直接删 `data\postgresql\*.old-*` 与旧 dump。
- Redis 默认只监听 `127.0.0.1`（`etc\redis\redis.conf.tpl` 里的 `bind`）。要跨机访问请
  自行改模板并加 `requirepass`
