# LNPP 组件下载源示例 — 复制为 packages.conf 后按需增删
#
# 格式：按组列出，`# 标题` 后跟 `---` 分隔符，条目为 `名称=URL`。
# 程序只认 nginx / nodejs / postgresql / redis 四种组件（node 目录别名 nodejs）。
#
# 安全要求：
# - 只接受 https:// 开头的 URL，http 会被直接拒绝。下载物是随后以当前用户
#   权限执行的 nginx.exe / initdb.exe / redis-server.exe，明文传输等于不设防。
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
