# Redis 5.x binds to every interface unless told otherwise, and this template
# ships no password: an unauthenticated Redis on 0.0.0.0:6379 is a remote
# takeover on any shared network. Both of the apps this stack serves
# (jwxt / jwpk) connect to 127.0.0.1, so binding loopback costs nothing.
bind 127.0.0.1
port {{PORT}}
daemonize no
pidfile {{PIDFILE}}
logfile {{LOGFILE}}
dir {{DIR}}
appendonly yes
save 900 1
save 300 10
save 60 10000
