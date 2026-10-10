#include "ipc.h"
#include "manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define SOCK_CLOSE(s) closesocket(s)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <strings.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
#define SOCK_CLOSE(s) close(s)
#endif

#ifdef _WIN32
#define cdm_strnicmp _strnicmp
#else
#define cdm_strnicmp strncasecmp
#endif

#ifndef CDM_IPC_DEFAULT_PORT
#define CDM_IPC_DEFAULT_PORT 15151
#endif
#define CDM_IPC_MAX_BODY (256 * 1024)

typedef struct {
    cdm_manager *mgr;
    int port;
    char api_key[128];
    volatile int stop;
    sock_t listen_fd;
} ipc_ctx;

static ipc_ctx g_ipc;
static int g_ipc_started = 0;

/* ---------- tiny JSON helpers (well-formed input) ---------- */

/* find "key" and return pointer to the value start (after ':') */
static const char *json_val(const char *js, const char *key) {
    char pat[128];
    const char *p, *c;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(js, pat);
    if (!p) return NULL;
    c = strchr(p + strlen(pat), ':');
    return c ? c + 1 : NULL;
}

/* extract a JSON string value into out (handles \" and \\). Returns 1 ok. */
static int json_string(const char *js, const char *key, char *out, size_t cap) {
    const char *v = json_val(js, key);
    size_t n = 0;
    if (!v || cap == 0) return 0;
    while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
    if (*v != '"') return 0;
    v++;
    while (*v && *v != '"' && n + 1 < cap) {
        if (*v == '\\' && v[1]) {
            char e = v[1];
            if (e == 'n') e = '\n';
            else if (e == 't') e = '\t';
            else if (e == 'r') e = '\r';
            out[n++] = e;
            v += 2;
        } else {
            out[n++] = *v++;
        }
    }
    out[n] = 0;
    return 1;
}

static int json_int(const char *js, const char *key, long *out) {
    const char *v = json_val(js, key);
    char *end;
    long x;
    if (!v) return 0;
    x = strtol(v, &end, 10);
    if (end == v) return 0;
    *out = x;
    return 1;
}

/* collect every "link" string in a (possibly array-wrapped) body */
static int json_links(const char *body, char out[][2048], int cap) {
    int n = 0;
    const char *p = body;
    while (n < cap) {
        const char *found = strstr(p, "\"link\"");
        const char *c;
        char tmp[2048];
        size_t k = 0;
        if (!found) break;
        c = strchr(found + 6, ':');
        if (!c) break;
        c++;
        while (*c == ' ' || *c == '\t') c++;
        if (*c != '"') { p = c + 1; continue; }
        c++;
        while (*c && *c != '"' && k + 1 < sizeof(tmp)) {
            if (*c == '\\' && c[1]) {
                char e = c[1];
                if (e == 'n') e = '\n';
                tmp[k++] = e;
                c += 2;
            } else {
                tmp[k++] = *c++;
            }
        }
        tmp[k] = 0;
        p = *c ? c + 1 : c;
        if (tmp[0]) {
            snprintf(out[n], 2048, "%s", tmp);
            n++;
        }
    }
    return n;
}

/* ---------- HTTP ---------- */

static int send_all(sock_t fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int w = send(fd, buf + sent, (int)(len - sent), 0);
#else
        ssize_t w = send(fd, buf + sent, len - sent, 0);
#endif
        if (w <= 0) return -1;
        sent += (size_t)w;
    }
    return 0;
}

static void send_json(sock_t fd, int code, const char *status, const char *body) {
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.0 %d %s\r\nContent-Type: application/json\r\n"
             "Content-Length: %lu\r\nConnection: close\r\n\r\n",
             code, status, (unsigned long)strlen(body));
    send_all(fd, hdr, strlen(hdr));
    send_all(fd, body, strlen(body));
}

/* read until end of headers; then body per Content-Length. Returns 1 ok. */
static int read_request(sock_t fd, char *method, size_t mcap, char *path,
                        size_t pcap, char *body, size_t bcap, size_t *bodylen,
                        char *api_hdr, size_t acap) {
    static char buf[CDM_IPC_MAX_BODY + 8192];
    size_t total = 0;
    char *hend, *cl;
    long clen = 0;
    size_t hlen;
    *bodylen = 0;
    if (api_hdr) api_hdr[0] = 0;
    for (;;) {
#ifdef _WIN32
        int r = recv(fd, buf + total, (int)(sizeof(buf) - 1 - total), 0);
#else
        ssize_t r = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
#endif
        if (r <= 0) return 0;
        total += (size_t)r;
        buf[total] = 0;
        hend = strstr(buf, "\r\n\r\n");
        if (hend) break;
        if (total >= sizeof(buf) - 1) return 0;
    }
    hlen = (size_t)(hend - buf);
    buf[total] = 0;
    /* request line (sizes enforced by the format caps) */
    (void)mcap;
    (void)pcap;
    if (sscanf(buf, "%7s %1023s", method, path) != 2) return 0;
    cl = strstr(buf, "Content-Length:");
    if (!cl) cl = strstr(buf, "content-length:");
    if (cl) clen = strtol(cl + 15, NULL, 10);
    if (clen < 0 || clen > CDM_IPC_MAX_BODY) return 0;
    /* X-Api-Key header (case-insensitive name match, simple scan) */
    {
        const char *p = buf;
        while ((p = strstr(p, "\r\n")) != NULL) {
            p += 2;
            if (p >= hend) break;
            if (cdm_strnicmp(p, "X-Api-Key:", 10) == 0) {
                const char *v = p + 10;
                size_t k = 0;
                while (*v == ' ' || *v == '\t') v++;
                while (v[k] && v[k] != '\r' && v[k] != '\n' && k + 1 < acap) {
                    api_hdr[k] = v[k];
                    k++;
                }
                api_hdr[k] = 0;
                break;
            }
        }
    }
    /* body may already be buffered past headers */
    {
        size_t have = total - (hlen + 4);
        size_t need = (size_t)clen;
        if (need >= bcap) return 0;
        if (have > 0) {
            size_t cp = have < need ? have : need;
            memcpy(body, hend + 4, cp);
            *bodylen = cp;
        }
        while (*bodylen < need) {
#ifdef _WIN32
            int r = recv(fd, body + *bodylen, (int)(need - *bodylen), 0);
#else
            ssize_t r = recv(fd, body + *bodylen, need - *bodylen, 0);
#endif
            if (r <= 0) return 0;
            *bodylen += (size_t)r;
        }
        body[*bodylen] = 0;
    }
    return 1;
}

static void serve(sock_t fd, ipc_ctx *cx) {
    char method[8] = {0}, path[1024] = {0};
    static char body[CDM_IPC_MAX_BODY + 1];
    size_t bodylen = 0;
    char api_hdr[256] = {0};
    char *q;

    if (!read_request(fd, method, sizeof(method), path, sizeof(path), body,
                      sizeof(body), &bodylen, api_hdr, sizeof(api_hdr))) {
        return;
    }
    /* strip query string */
    q = strchr(path, '?');
    if (q) *q = 0;

    if (cx->api_key[0] && strcmp(api_hdr, cx->api_key) != 0) {
        send_json(fd, 401, "Unauthorized", "{\"error\":\"bad api key\"}");
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/ping") == 0) {
        send_json(fd, 200, "OK", "{\"pong\":true}");
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/queues") == 0) {
        static char out[2048];
        size_t off = 0;
        int i;
        off += (size_t)snprintf(out + off, sizeof(out) - off, "[");
        cdm_mutex_lock(cx->mgr->mtx);
        for (i = 0; i < cx->mgr->n_queues; i++) {
            off += (size_t)snprintf(out + off, sizeof(out) - off,
                                    "%s{\"id\":%d,\"name\":\"%s\"}",
                                    i ? "," : "", i, cx->mgr->queues[i].name);
            if (off + 64 >= sizeof(out)) break;
        }
        cdm_mutex_unlock(cx->mgr->mtx);
        snprintf(out + off, sizeof(out) - off, "]");
        send_json(fd, 200, "OK", out);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/add") == 0) {
        static char links[16][2048];
        int n = json_links(body, links, 16);
        int i;
        if (n <= 0) {
            send_json(fd, 400, "Bad Request", "{\"error\":\"no link\"}");
            return;
        }
        for (i = 0; i < n; i++)
            cdm_manager_push_url(cx->mgr, links[i]);
        send_json(fd, 200, "OK", "{\"ok\":true}");
    } else if (strcmp(method, "POST") == 0 &&
               strcmp(path, "/start-headless-download") == 0) {
        char url[2048] = {0}, folder[2048] = {0}, name[1024] = {0};
        long qid = -1;
        cdm_config cfg;
        char outpath[2200];
        int id;
        if (!json_string(body, "downloadSource", url, sizeof(url))) {
            send_json(fd, 400, "Bad Request",
                      "{\"error\":\"downloadSource required\"}");
            return;
        }
        json_string(body, "folder", folder, sizeof(folder));
        json_string(body, "name", name, sizeof(name));
        json_int(body, "queueId", &qid);
        if (name[0] && folder[0])
            snprintf(outpath, sizeof(outpath), "%.1500s/%.600s", folder, name);
        else if (folder[0])
            snprintf(outpath, sizeof(outpath), "%.2047s", folder);
        else
            outpath[0] = 0;
        cdm_config_default(&cfg);
        cfg.quiet = 1;
        if (qid >= 0)
            id = cdm_manager_add_to_queue_idx(cx->mgr, url,
                                              outpath[0] ? outpath : NULL,
                                              &cfg, (int)qid, 0);
        else
            id = cdm_manager_add(cx->mgr, url, outpath[0] ? outpath : NULL, &cfg);
        if (id < 0) {
            send_json(fd, 400, "Bad Request", "{\"error\":\"rejected\"}");
        } else {
            char resp[64];
            snprintf(resp, sizeof(resp), "{\"ok\":true,\"id\":%d}", id);
            send_json(fd, 200, "OK", resp);
        }
    } else {
        send_json(fd, 404, "Not Found", "{\"error\":\"unknown endpoint\"}");
    }
}

static void *ipc_thread(void *arg) {
    ipc_ctx *cx = (ipc_ctx *)arg;
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = (socklen_t)sizeof(peer);
        sock_t fd;
        fd_set rfds;
        struct timeval tv;
        if (cx->stop) break;
        FD_ZERO(&rfds);
        FD_SET(cx->listen_fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000;
#ifdef _WIN32
        {
            int r = select(0, &rfds, NULL, NULL, &tv);
            if (r <= 0) continue;
        }
#else
        {
            int r = select((int)cx->listen_fd + 1, &rfds, NULL, NULL, &tv);
            if (r <= 0) continue;
        }
#endif
        fd = accept(cx->listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd == SOCK_INVALID) {
            if (cx->stop) break;
            continue;
        }
        /* loopback only, even though we bound 127.0.0.1 */
        {
            unsigned long ip = (unsigned long)peer.sin_addr.s_addr;
            if ((ip & 0xFF) != 127 && ip != 0x0100007FUL) {
                SOCK_CLOSE(fd);
                continue;
            }
        }
        serve(fd, cx);
        SOCK_CLOSE(fd);
    }
    return NULL;
}

int cdm_ipc_start(cdm_manager *m, int port, const char *api_key) {
    sock_t ls;
    struct sockaddr_in addr;
    int one = 1;
    int bound = -1;
    if (!m) return -1;
    cdm_ipc_stop();
    if (port <= 0) port = CDM_IPC_DEFAULT_PORT;
#ifdef _WIN32
    {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return -1;
    }
#endif
    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == SOCK_INVALID) return -1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        SOCK_CLOSE(ls);
        return -1;
    }
    if (listen(ls, 8) != 0) {
        SOCK_CLOSE(ls);
        return -1;
    }
    /* discover the bound port (also fine when port was explicit) */
    {
        struct sockaddr_in got;
        socklen_t glen = (socklen_t)sizeof(got);
        if (getsockname(ls, (struct sockaddr *)&got, &glen) == 0)
            bound = (int)ntohs(got.sin_port);
        else
            bound = port;
    }
    memset(&g_ipc, 0, sizeof(g_ipc));
    g_ipc.mgr = m;
    g_ipc.port = bound;
    if (api_key && *api_key)
        snprintf(g_ipc.api_key, sizeof(g_ipc.api_key), "%s", api_key);
    g_ipc.stop = 0;
    g_ipc.listen_fd = ls;
    if (cdm_thread_spawn_detached(ipc_thread, &g_ipc) != 0) {
        SOCK_CLOSE(ls);
        return -1;
    }
    g_ipc_started = 1;
    return bound;
}

void cdm_ipc_stop(void) {
    if (!g_ipc_started) return;
    g_ipc.stop = 1;
    SOCK_CLOSE(g_ipc.listen_fd);
    g_ipc_started = 0;
    /* the detached thread exits on its next select slice */
}

int cdm_ipc_port(void) {
    return g_ipc_started ? g_ipc.port : -1;
}
