# ---- build stage: full toolchain ----
FROM alpine:3.20 AS build
RUN apk add --no-cache build-base
WORKDIR /src
COPY . .
RUN make clean all

# ---- runtime stage: just the binary and the docroot ----
FROM alpine:3.20
RUN adduser -D -H httpd
COPY --from=build /src/httpd /usr/local/bin/httpd
COPY www /srv/www
# Run as an unprivileged user: a container security basic.
USER httpd
EXPOSE 8080
# Exec form (JSON array) makes httpd PID 1, so `docker stop`'s SIGTERM reaches it
# directly and the graceful shutdown runs instead of a forced kill after 10 s.
ENTRYPOINT ["/usr/local/bin/httpd", "-p", "8080", "-r", "/srv/www"]
