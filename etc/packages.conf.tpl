# LNPP 组件下载源示例 — 复制为 packages.conf 后按需增删
#
# 格式：按组列出，`# 标题` 后跟 `---` 分隔符，条目为 `名称=URL`。
# 程序只认 nginx / nodejs / postgresql / redis / php 五种组件
# （node 目录别名 nodejs）。
#
# 安全要求：
# - 只接受 https:// 开头的 URL，http 会被直接拒绝。下载物是随后以当前用户
#   权限执行的 nginx.exe / initdb.exe / redis-server.exe / php-cgi.exe，
#   明文传输等于不设防。
# - 名称的 `-` 之后是版本号，只能包含字母/数字/`.`/`_`/`-`/`+`；名字里不能有
#   路径分隔符，也不能是 `.` / `..`（该值会用作 bin\<组件>\<版本> 目录名）。
#
# 可选完整性校验：在 URL 条目下方加一行 `名称.sha256=64位hex`，
# 下载后用系统 certutil 计算 SHA256 比对，不匹配则拒绝安装。
# 生成方式：certutil -hashfile <下载的zip> SHA256
# （没有 sha256 条目时跳过校验，向后兼容 —— 但请尽量都补上）

# Web Servers
---
Nginx-1.29.5=https://nginx.org/download/nginx-1.29.5.zip
Nginx-1.30.4=https://nginx.org/download/nginx-1.30.4.zip
# Nginx-1.30.4.sha256=<64位hex>
---

# Node.js
# https://nodejs.org/en/download/prebuilt-binaries/current
---
node-22.21=https://nodejs.org/dist/v22.21.1/node-v22.21.1-win-x64.zip
node-24.12=https://nodejs.org/dist/v24.12.0/node-v24.12.0-win-x64.zip
# node-24.12.sha256=<64位hex>
---

# PostgreSQL（便携版 zip）
# https://www.enterprisedb.com/download-postgresql-binaries
---
postgresql-16.6=https://sbp.enterprisedb.com/getfile.jsp?fileid=1259297
postgresql-17.2=https://sbp.enterprisedb.com/getfile.jsp?fileid=1259294
---

# Redis
---
Redis-5.0.14=https://github.com/tporadowski/redis/releases/download/v5.0.14.1/Redis-x64-5.0.14.1.zip
---

# PHP（Windows NTS x64）
# https://windows.php.net/downloads/releases/
#
# 列了 8.1 ~ 8.5 各一个当前补丁版——PHP 组件是**多版本共存**的，几个项目分别
# 要不同 PHP 版本时各装一个即可，启动时各占一个 FastCGI 端口，站点可以分别
# 指向不同版本。8.0 已停止安全维护，未列入；需要的话按同样格式加一行
# （URL 把版本号换掉，SHA256 用 certutil -hashfile <下载的zip> SHA256 自算）。
#
# - 选 **NTS**（Non Thread Safe）：本管理器把 PHP 当 FastCGI 后端跑
#   （php-cgi.exe -b 127.0.0.1:<端口>），NTS 正是这个场景的常规选择；
#   TS 是给多线程 Web API 用的。
# - vs16 = 用 Visual Studio 2019 编译，vs17 = VS2022（8.4 起官方只出 vs17）。
#   **两者都要求安装「Microsoft Visual C++ 2015-2022 可再发行组件包」(x64)**，
#   缺了 php-cgi.exe 会在启动时直接退出，页面上表现为端口一直不监听：
#   https://aka.ms/vs/17/release/vc_redist.x64.exe
# - 压缩包是「散文件」结构：解压后 bin\php\<版本>\ 下直接就是 php-cgi.exe、
#   ext\、php.ini-production，不套一层目录。
# - SHA256 是 2026-10-01 从上述官方地址下载后实算的。PHP 官方只在
#   https://windows.php.net/download/ 上公布当前最新版的校验和，旧补丁版需自算；
#   其中 8.5.11 与官方公布值逐字符比对通过，8.3.35 重下两次哈希可复现。
php-8.1.34=https://windows.php.net/downloads/releases/php-8.1.34-nts-Win32-vs16-x64.zip
php-8.1.34.sha256=9cfe246cb144076c16f5913a3ef88a474c3dd7e60f0f0c8bb95faf68674016cc
php-8.2.34=https://windows.php.net/downloads/releases/php-8.2.34-nts-Win32-vs16-x64.zip
php-8.2.34.sha256=03249b5c9414c6dbe30276f4a7598bd9d2a7417ee81f709b06b99e9c4a2aff4f
php-8.3.35=https://windows.php.net/downloads/releases/php-8.3.35-nts-Win32-vs16-x64.zip
php-8.3.35.sha256=25a8e2ac9ff30f1d768d1447c09a600617fa6e6082729f6e95f008b59c91fe45
php-8.4.26=https://windows.php.net/downloads/releases/php-8.4.26-nts-Win32-vs17-x64.zip
php-8.4.26.sha256=da68394f9193b7f6b89d0c76861a4034ae10efee7fd55a7255d8118c2acf70d7
php-8.5.11=https://windows.php.net/downloads/releases/php-8.5.11-nts-Win32-vs17-x64.zip
php-8.5.11.sha256=0ea96e0d2b9b737a6036f05cf4e95c49313faa6d0f27bd97edb2742503f0c043
---
