# LNPP 组件管理器 v1.2

Windows 原生 C++ (Win32) 桌面工具，管理 nodejs / nginx / postgresql / redis 的启动、停止、版本切换、配置。

## 目录结构

```
lnpp.exe
├── bin\              # 组件二进制（每个组件下按版本分子目录）
│   ├── nodejs\v24\   # 手动拷贝 node.exe、npm 等
│   ├── nginx\1.30\   # 手动拷贝 nginx.exe、conf 等
│   ├── postgresql\17\# 手动拷贝整个 zip 解压目录（含 bin\）
│   └── redis\5.0\    # 手动拷贝 redis-server.exe、redis-cli.exe
├── etc\              # 配置模板（程序启动时据此生成运行时配置）
├── data\             # 运行时数据（postgresql 数据库目录按版本分目录）
├── backup\           # 数据库备份（pg_dumpall 输出，时间戳命名）
├── logs\             # 运行日志
└── www\              # nginx 站点根目录
```

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

- 各组件启动/停止，状态灯实时显示
- 版本切换（自动重启 + 重新生成配置）
- nginx：可视化添加/删除虚拟站点（写 etc\nginx\vhosts\），www\ 下自动建目录
- PostgreSQL：初始化、改密码、创建/删除用户、备份（pg_dumpall）
- Node.js：pm2 进程列表实时监控，支持重启/停止
- 组件下载：首启（bin 为空）自动弹出，或在总览页点「下载组件」；地址读 `packages.conf`（仓库提供 `packages.conf.example`，复制改名后按需增删），下载完成后自动解压到 bin\<组件>\<版本>
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
```

输出 `lnpp.exe` 到仓库根目录（exe 所在目录即运行时根目录）。

## 测试

`test.bat` 会编译并运行三个集成测试（`selftest` / `proctest` / `migtest`）。测试会实际启停组件、切换版本、做 PostgreSQL 17→18→17 数据迁移，因此运行时请确保 80 / 5432 / 6379 端口未被其他程序占用。

```
test.bat build    # 只编译测试程序到仓库根目录
test.bat run      # 只运行已编译的测试程序
test.bat all      # 编译并运行（默认）
```

## 说明

- 所有路径基于 exe 所在位置推导，绿色便携
- PostgreSQL 默认端口 5432，redis 6379，nginx 80；在 `data\settings.ini` 中可改
- 首次使用 PostgreSQL：切到对应页签点「初始化数据库」
- 组件下载源：根目录 `packages.conf` 按组列出（`# 标题` `---` 分隔，条目 `名称=URL`），程序只认 nginx/nodejs/postgresql/redis 组件
- 可选完整性校验：在 URL 条目下方加一行 `名称.sha256=64位hex`（用 `certutil -hashfile <文件> SHA256` 生成），下载后自动比对，不匹配则拒绝安装；没有该行时跳过校验
