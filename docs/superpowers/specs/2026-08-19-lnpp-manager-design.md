# LNPP 组件管理器设计文档

日期：2026-08-19
状态：已实现（细节已与代码漂移，见下方说明）

> **以 README 为准。** 本文是最初的设计稿，有三处描述与现在的实现不一致：
>
> - §5「停止：优先 `GenerateConsoleCtrlEvent`」—— 代码没有用控制台事件，一律
>   `TerminateProcess`（PG 走 `pg_ctl stop -m fast`，nginx 先 `-s quit` 再轮询，超时才强杀）。
> - §6「`pg_dump -Fc` 全量导出 → `pg_restore`」—— 实际是 `pg_dumpall`（纯 SQL）+
>   `psql -f`，备份文件是 `.sql` 不是 `.dump`。
> - §2/§9「软件不负责下载」—— 已有组件下载器，设计见
>   [2026-08-22-about-and-downloader-design.md](2026-08-22-about-and-downloader-design.md)。
>
> 另外 PG 迁移现在会在成功后把旧的 `data\postgresql\<版本>.old-*` 副本移入回收站
> （默认保留最近 2 份），§6 第 6 步「旧 data 目录保留」已不再无限保留。
>
> §4 描述的「主配置里那个 `server_name localhost` 的 www 站点」已被一对
> `default_server` 兜底块取代（拒绝 IP 直连与未配置域名，80 返 500、443 握手拒绝），
> 见 README「拒绝 IP 直连与未配置域名」与 `settings.ini` 的 `nginx.block_unknown_host`。
>
> §3 的目录树与 §7 的 nginx 行仍写着 `etc\nginx\vhosts\<域名>.conf`，这也是旧布局：
> v1.5.3 起 `etc\` 只放模板，站点配置在 `data\nginx\vhosts\`，见 README「目录结构」。
> §4 的 postgresql 行也少了 v1.5.2 起的「还原数据库」按钮。

## 1. 目标

开发一个 Windows 原生 C++（Win32 API）桌面软件，用于管理 nodejs、nginx、postgresql、redis 四个组件的安装、启动/停止、版本切换、配置管理。

## 2. 约束

- 仅 Windows 运行
- 占用资源极小（目标内存 < 20MB，单 exe，无第三方依赖）
- 不追求美观度，使用 Win32 原生控件
- 绿色免安装，所有路径基于主 exe 相对位置推导
- 组件二进制文件由用户手动拷贝到 bin 目录，软件不负责下载

## 3. 目录结构

```
lnpp.exe
├── bin\
│   ├── nodejs\
│   │   ├── v18\
│   │   └── v24\
│   ├── nginx\
│   │   └── 1.26\
│   ├── postgresql\
│   │   └── 16\
│   └── redis\
│       └── 7.2\
├── etc\              # 配置模板
│   ├── nginx\        #   nginx.conf 模板 + vhosts 站点配置
│   ├── postgresql\   #   postgresql.conf / pg_hba.conf 模板
│   └── redis\
├── data\             # 按版本分目录
│   └── postgresql\
│       └── 16\       #   initdb 生成的数据库目录
├── backup\           # 数据库 dump 存档（时间戳命名）
├── logs\             # 运行日志
└── www\              # nginx 站点根目录
    └── <站点名>\index.html
```

## 4. UI 布局（Win32 原生控件）

- 主窗口左侧 4 个组件页签（Tab Control）：nginx / postgresql / redis / nodejs
- 每个页签：
  - 版本下拉框 + 切换按钮
  - 状态灯（红/绿）+ 启动/停止按钮
  - 配置按钮（打开该组件的**运行时**配置目录：nginx 是 `data\nginx\vhosts\` 站点配置，
    PostgreSQL / Redis 是当前版本的数据目录，Node.js 是 `data\nodejs\`。本条最初写的是
    「打开 etc 模板目录」，但 v1.5.3 之后 `etc\` 只放模板，点开全是不会被读取的文件；
    Node.js 更是根本没有 `etc\nodejs\`，那是个不存在的目录）
  - 底部日志输出框（实时滚动）
- nginx 页签附加：虚拟站点列表 + 添加/删除按钮、域名/端口/根目录输入框
- nodejs 页签附加：pm2 项目列表（状态、重启/停止按钮），定时轮询 `pm2 jlist`
- postgresql 页签附加：初始化/改密码/改用户按钮（弹出输入框）

## 5. 进程管理（子进程方式）

- 使用 `CreateProcess` 启动组件，记录 PID
- 停止：优先 `GenerateConsoleCtrlEvent`，失败则 `TerminateProcess`
- nginx 启动前先 `nginx -t` 校验配置
- 主程序退出时自动清理所有子进程
- 状态检测：`OpenProcess` 轮询进程存活

## 6. 版本切换流程

### 通用流程

1. 停止当前组件进程
2. 重新生成配置（指向新版本路径）
3. 启动新版本

### PostgreSQL 特殊流程

1. 旧版本 `pg_dump -Fc` 全量导出到 `backup\<时间戳>.dump`
2. 新版本 `initdb` 初始化 `data\postgresql\<新版本>`
3. 配置模板复制 + 参数替换（端口、数据目录、密码）
4. `pg_restore` 导入 dump
5. 启动新版本
6. 旧 `data` 目录保留

## 7. 组件功能

| 组件 | 功能 |
|---|---|
| nginx | 添加/删除虚拟站点：写 `etc\nginx\vhosts\<域名>.conf`（server_name/root/port），创建 `www\<站点名>`，`nginx -t` 校验后 reload；端口冲突检测 |
| postgresql | 初始化（initdb）、改密码（ALTER USER ... PASSWORD）、改用户（创建/删除用户）、启动/停止 |
| redis | 启动/停止、版本切换 |
| nodejs | pm2 监控（轮询 `pm2 jlist` 显示项目状态）、重启/停止项目；版本切换后重启 pm2 daemon 并恢复进程（`pm2 resurrect`） |

## 8. 技术要点

- 单 exe，纯 Win32 API，无第三方依赖
- 配置以「模板 + 参数替换」方式生成，`etc\` 下可自定义模板
- 日志写到 `logs\` 目录 + UI 实时显示
- 所有路径从 exe 相对位置推导

## 9. 组件文件来源

各组件二进制文件（bin 下各版本目录）由用户手动拷贝，软件不下载。实现过程中如需要具体文件，会提醒用户拷贝。