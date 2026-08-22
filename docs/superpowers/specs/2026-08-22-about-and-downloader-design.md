# 设计：关于对话框 + 组件下载器

日期：2026-08-22
状态：已确认（用户口头批准，"按推荐"+"开始实施"）

## 需求

1. 总览页右下角显示版本号 +「关于」链接；点击弹出关于对话框：
   作者 arbog、GitHub 地址（可点击）、版本号。
2. 首次运行（bin 不存在或四个组件目录全空）自动弹出下载界面；总览页提供手动入口按钮。
3. 下载源来自根目录 packages.conf（`---` 分段、`#` 注释为标题、条目 `名称=URL`）。
   下载完成后自动解压安装到 `bin\<组件>\<版本>\`。

## 技术决策（已确认）

- 下载引擎：WinHTTP 原生 API（自动跟随重定向，EDB getfile.jsp 会 302；进度回调）。
- 解压：系统 `tar.exe`（Win10 1803+ 自带，bsdtar 原生读 zip）；缺失时回退 PowerShell
  `Expand-Archive`。
- 包名→目录映射：条目名第一个 `-` 前部分小写化作组件名；别名 `node`→`nodejs`；
  仅接受 nginx/nodejs/postgresql/redis 四个已知组件（其余条目忽略）。
- zip 内部结构归位：解压到临时目录后，若只有一个顶层子目录且无散文件（nginx/node/pgsql
  均如此）则取该子目录内容；否则直接用临时目录内容（redis 散文件结构）。
- 目标版本目录已存在 → 报错中止（不覆盖）。
- 临时目录与安装目标可能跨卷（%TEMP% 在 C:，程序在 D:)，复制用 SHFileOperationW(FO_COPY)，
  清理用 SHFileOperationW(FO_DELETE)，不使用 MoveFile。

## UI

### 关于对话框（模态，自绘窗口 ~340x190）
- 标题「关于 LNPP」；正文：LNPP 组件管理器 / 版本 v<运行时读 VERSIONINFO> /
  作者：arbog / GitHub 链接（蓝色下划线 SS_NOTIFY，点击 ShellExecuteW 打开）/ 确定按钮。
- 版本号读取 exe 的 VERSIONINFO（GetFileVersionInfoW），失败回退常量 L"1.0.0.0"。

### 总览页右下角
- 底部一行右侧：`v<x.y.z.w>    关于`（「关于」同款链接控件）。

### 下载器对话框（模态，~560x420）
- ListView [组件 | 名称 | 状态]，数据 = packages.conf 解析 + bin 现状（已装标「已安装」）。
- 「下载」按钮下载选中项；PROGRESS_CLASS 进度条 + 状态文字行。
- 下载/解压/安装在工作线程执行，进度经 WM_APP 消息回 UI 线程；期间禁用界面；
  支持取消（分块间检查原子标志，取消即删临时文件）。
- 完成后刷新状态列并通知主窗口刷新版本下拉（WM_OP_DONE 复用）。

## 触发时机

- 首次自动：wWinMain 显示主窗口后检测 pkgsNeedSetup()，成立则以主窗口为 owner 弹出。
- 手动：总览页 PATH 按钮旁新增「下载组件」按钮。

## 文件改动

| 文件 | 内容 |
|---|---|
| src/downloader.h 新增 | PkgItem/PkgSection 结构与 API 声明 |
| src/downloader.cpp 新增 | parsePackagesConf / pkgNameToCompVer / pkgsNeedSetup / downloadFileTo(WinHTTP) / extractZipTo(tar→PowerShell 回退) / installPackage 编排 |
| src/main.cpp | 版本助手、右下角控件、关于框、下载器对话框、首次检测、手动按钮 |
| packages.conf | 补 Redis 段 |
| build.bat / test.bat | 源文件清单加 downloader.cpp，链 winhttp.lib |

## 验收

- test.bat 全量回归仍通过。
- 删除 bin 后首启自动弹下载器；从 conf 下载 nginx 小包成功落位 bin\nginx\<ver> 并出现在版本下拉。
- 关于框信息正确、链接可点。
