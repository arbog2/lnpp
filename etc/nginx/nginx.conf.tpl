worker_processes  1;

error_log  logs/error.log;
pid        logs/nginx.pid;

events {
    worker_connections  1024;
}

http {
    include       "{{MIME}}";
    default_type  application/octet-stream;
    sendfile        on;
    keepalive_timeout  65;
    access_log logs/access.log;
    error_log  logs/error.log;

    server {
        listen       {{PORT}};
        server_name  localhost;
        location / {
            root   {{WWW_DIR}};
            index  index.html index.htm;
        }
    }

    include vhosts/*.conf;
}
