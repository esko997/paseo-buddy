#include "net.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define LONG_POLL_S 25
#define CONNECT_TIMEOUT_MS 4000
#define MAX_RESPONSE (256 * 1024)

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
    return s;
}

bool config_load(const char *path, Config *cfg, char *err, size_t errlen) {
    memset(cfg, 0, sizeof *cfg);
    snprintf(cfg->pet_name, sizeof cfg->pet_name, "Tok");
    cfg->rumble = true;
    cfg->screen_timeout_s = 120;
    cfg->awake_on_power = true;
    cfg->volume = 4;
    cfg->quiet_nights = true;
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, errlen, "missing %s", path);
        return false;
    }
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        if (!strcmp(key, "bridge")) {
            if (cfg->n_bridges == MAX_BRIDGES) continue;
            Bridge *b = &cfg->bridges[cfg->n_bridges];
            char hostport[64] = {0}, token[96] = {0};
            if (sscanf(val, "%63s %95s", hostport, token) != 2) {
                snprintf(err, errlen, "line %d: bridge = host:port token", lineno);
                fclose(f);
                return false;
            }
            char *colon = strrchr(hostport, ':');
            b->port = colon ? atoi(colon + 1) : 6780;
            if (colon) *colon = 0;
            snprintf(b->host, sizeof b->host, "%s", hostport);
            snprintf(b->token, sizeof b->token, "%s", token);
            pthread_mutex_init(&b->mu, NULL);
            b->state = LINK_CONNECTING;
            cfg->n_bridges++;
        } else if (!strcmp(key, "pet_name")) {
            snprintf(cfg->pet_name, sizeof cfg->pet_name, "%s", val);
        } else if (!strcmp(key, "rumble")) {
            cfg->rumble = atoi(val) != 0;
        } else if (!strcmp(key, "screen_timeout")) {
            cfg->screen_timeout_s = atoi(val);
        } else if (!strcmp(key, "awake_on_power")) {
            cfg->awake_on_power = atoi(val) != 0;
        } else if (!strcmp(key, "volume")) {
            int v = atoi(val);
            cfg->volume = v < 0 ? 0 : v > 10 ? 10 : v;
        } else if (!strcmp(key, "quiet_nights")) {
            cfg->quiet_nights = atoi(val) != 0;
        }
    }
    fclose(f);
    if (cfg->n_bridges == 0) {
        snprintf(err, errlen, "no bridge = lines in %s", path);
        return false;
    }
    return true;
}

static int connect_to(const char *host, int port, char *err, size_t errlen) {
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    /* IPv4 literals only: a home-LAN pager has no reliable DNS for desktop hostnames. */
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        snprintf(err, errlen, "%s: use an IP address", host);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof addr);
    if (rc < 0 && errno == EINPROGRESS) {
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        rc = poll(&pfd, 1, CONNECT_TIMEOUT_MS);
        if (rc == 1) {
            int soerr = 0;
            socklen_t len = sizeof soerr;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
            errno = soerr;
            rc = soerr ? -1 : 0;
        } else {
            errno = rc == 0 ? ETIMEDOUT : errno;
            rc = -1;
        }
    }
    if (rc < 0) {
        snprintf(err, errlen, "connect: %s", strerror(errno));
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);
    return fd;
}

/* One HTTP/1.0 GET; the bridge closes the connection after each response. */
static bool http_get(Bridge *b, int64_t rev, int wait_s, char **body_out, char *err, size_t errlen) {
    int fd = connect_to(b->host, b->port, err, errlen);
    if (fd < 0) return false;
    struct timeval tv = {.tv_sec = wait_s + 10};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char req[512];
    int n = snprintf(req, sizeof req,
                     "GET /pager?token=%s&rev=%lld&wait=%d HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                     b->token, (long long)rev, wait_s, b->host);
    if (send(fd, req, (size_t)n, 0) != n) {
        snprintf(err, errlen, "send: %s", strerror(errno));
        close(fd);
        return false;
    }
    size_t cap = 16384, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len + 4096 + 1 > cap) {
            if (cap >= MAX_RESPONSE) break;
            cap *= 2;
            buf = realloc(buf, cap);
        }
        ssize_t got = recv(fd, buf + len, cap - len - 1, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) {
            snprintf(err, errlen, "recv: %s", strerror(errno));
            free(buf);
            close(fd);
            return false;
        }
        if (got == 0) break;
        len += (size_t)got;
    }
    close(fd);
    buf[len] = 0;
    int status = 0;
    sscanf(buf, "HTTP/%*s %d", &status);
    char *body = strstr(buf, "\r\n\r\n");
    if (status != 200 || !body) {
        snprintf(err, errlen, status == 401 ? "bad token" : "http %d", status);
        free(buf);
        return false;
    }
    *body_out = strdup(body + 4);
    free(buf);
    return true;
}

static int64_t body_rev(const char *body) {
    const char *p = strstr(body, "\nrev\t");
    return p ? atoll(p + 5) : -1;
}

static void publish(Bridge *b, char *body, LinkState state, const char *err) {
    pthread_mutex_lock(&b->mu);
    if (body) {
        free(b->body);
        b->body = body;
        b->gen++;
    }
    b->state = state;
    snprintf(b->error, sizeof b->error, "%s", err ? err : "");
    pthread_mutex_unlock(&b->mu);
}

static void *worker(void *arg) {
    Bridge *b = arg;
    int64_t rev = -1;
    int backoff = 2;
    for (;;) {
        char *body = NULL, err[64] = {0};
        if (http_get(b, rev, LONG_POLL_S, &body, err, sizeof err)) {
            int64_t next = body_rev(body);
            if (next == rev) {
                free(body); /* long-poll timed out with no change */
                publish(b, NULL, LINK_OK, NULL);
            } else {
                rev = next;
                publish(b, body, LINK_OK, NULL);
            }
            backoff = 2;
        } else {
            publish(b, NULL, LINK_ERROR, err);
            rev = -1;
            sleep((unsigned)backoff);
            if (backoff < 15) backoff *= 2;
        }
    }
    return NULL;
}

void net_start(Config *cfg) {
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < cfg->n_bridges; i++) {
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 128 * 1024);
        pthread_create(&t, &attr, worker, &cfg->bridges[i]);
        pthread_attr_destroy(&attr);
    }
}

char *bridge_take(Bridge *b, uint64_t *gen_seen, LinkState *state, char *err, size_t errlen) {
    char *copy = NULL;
    pthread_mutex_lock(&b->mu);
    if (b->gen != *gen_seen && b->body) {
        copy = strdup(b->body);
        *gen_seen = b->gen;
    }
    *state = b->state;
    snprintf(err, errlen, "%s", b->error);
    pthread_mutex_unlock(&b->mu);
    return copy;
}

bool bridge_fetch_once(Bridge *b, char **body_out, char *err, size_t errlen) {
    return http_get(b, -1, 0, body_out, err, errlen);
}
