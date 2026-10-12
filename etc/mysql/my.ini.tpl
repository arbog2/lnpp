# MySQL 8.0 配置模板。LNPP 在每次启动 MySQL 前把本文件渲染成
# data\mysql\<版本>\my.ini 并用 --defaults-file 指定它，所以：
#   - 手改这个 data\ 下的 my.ini 会被下次启动覆盖；
#   - 自己的设置请放到 data\mysql\<版本>\conf.d\ 下的 .cnf 文件里（见文件末尾
#     的 !includedir），那里不会被覆盖，也不需要改这里的模板。
#
# 默认只监听 127.0.0.1 且不设 root 远程访问——和 Redis 同样的理由：
# 这台机器上的应用（jwxt / jwpk）都连本机。要跨机访问，请在 conf.d\ 里加
# bind-address = 0.0.0.0 并单独建一个带强密码的账号，别放开 root。
# 如果在 conf.d\ 里覆盖端口，也要同步改 data\settings.ini 的 mysql.port，
# 否则管理器的客户端命令会连错端口。
[mysqld]
# 版本根目录（bin\mysql\<版本>）。mysqld 靠它找 share\ 下的错误消息文件。
basedir = "{{BASEDIR}}"
# 数据目录。刻意与 my.ini 分开放：--initialize-insecure 要求数据目录为空，
# 而 my.ini 每次启动都会重新生成。
datadir = "{{DATADIR}}"
port = {{PORT}}
bind-address = 127.0.0.1
log-error = "{{LOGFILE}}"
pid-file = "{{PIDFILE}}"
character-set-server = utf8mb4
collation-server = utf8mb4_general_ci
# 8.0 默认认证插件是 caching_sha2_password，老客户端（PHP mysqlnd、旧版
# Navicat/DBeaver、部分老驱动）连不上。这里保持 mysql_native_password，
# 新建账号默认走它；确需 caching_sha2 可在 conf.d\ 改掉。
default_authentication_plugin = mysql_native_password
max_connections = 100
# 用户自己的配置放这里，LNPP 不碰。!includedir 是 MySQL 官方的选项文件指令，
# 每个 .cnf 按字母顺序并入。
!includedir "{{CONFDIR}}"
