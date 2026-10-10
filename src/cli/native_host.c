/*
 * cdm-native-host: Chrome/Firefox Native Messaging bridge.
 *
 * Reads length-prefixed JSON messages on stdin (uint32 LE + UTF-8),
 * forwards them to the local cdm integration server, and writes
 * length-prefixed JSON replies on stdout.
 *
 * Incoming:
 *   {"url": "https://..."}          queue one URL (Add dialog)
 *   {"urls": ["https://...", ...]}  queue many (Batch dialog)
 *   {"ping": true}                  health check
 * Outgoing:
 *   {"ok": true, ...} / {"error": "..."}
 *
 * Usage: cdm-native-host [port] [api-key]
 *   port defaults to 15151; api key defaults to $CDM_API_KEY (or none).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <fcntl.h>
#include <io.h>
typedef SOCKET sock_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int sock_t;
#endif

static int g_port = 15151;
static char g_key[128] = {0};

static int read_all(void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        size_t r = fread((char *)buf + got, 1, len - got, stdin);
        if (r == 0) return -1;
        got += r;
    }
    return 0;
}

static void write_msg(const char *json) {
    unsigned int len = (unsigned int)strlen(json);
    unsigned char hdr[4];
    hdr[0] = (unsigned char)(len & 0xFF);
    hdr[1] = (unsigned char)((len >> 8) & 0xFF);
    hdr[2] = (unsigned char)((len >> 16) & 0xFF);
    hdr[3] = (unsigned char)((len >> 24) & 0xFF);
    fwrite(hdr, 1, 4, stdout);
    fwrite(json, 1, len, stdout);
    fflush(stdout);
}

/* Minimal HTTP client against 127.0.0.1:g_port. Returns 1 when the
 * response status is 2xx (copies body when body != NULL). */
static int http(const char *method, const char *path, const char *payload,
                char *body, size_t bcap, int *code_out) {
    sock_t s;
    struct sockaddr_in addr;
    char req[5200];
    static char resp[131072];
    size_t total = 0;
    int code = 0;
    const char *p;

    s = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return 0;
#else
    if (s < 0) return 0;
#endif
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)g_port);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return 0;
    }
    {
        char keyhdr[160] = {0};
        if (g_key[0])
            snprintf(keyhdr, sizeof(keyhdr), "X-Api-Key: %s\r\n", g_key);
        snprintf(req, sizeof(req),
                 "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\n%s"
                 "Content-Type: application/json\r\nContent-Length: %lu\r\n"
                 "Connection: close\r\n\r\n%s",
                 method, path, keyhdr,
                 (unsigned long)(payload ? strlen(payload) : 0),
                 payload ? payload : "");
    }
#ifdef _WIN32
    send(s, req, (int)strlen(req), 0);
#else
    send(s, req, strlen(req), 0);
#endif
    for (;;) {
#ifdef _WIN32
        int r = recv(s, resp + total, (int)(sizeof(resp) - 1 - total), 0);
#else
        ssize_t r = recv(s, resp + total, sizeof(resp) - 1 - total, 0);
#endif
        if (r <= 0) break;
        total += (size_t)r;
        if (total >= sizeof(resp) - 1) break;
    }
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    resp[total] = 0;
    if (sscanf(resp, "HTTP/%*d.%*d %d", &code) != 1 &&
        sscanf(resp, "HTTP/%d", &code) != 1)
        code = 0;
    if (code_out) *code_out = code;
    p = strstr(resp, "\r\n\r\n");
    if (body && bcap > 0) {
        if (p)
            snprintf(body, bcap, "%s", p + 4);
        else
            body[0] = 0;
    }
    return code >= 200 && code < 300;
}

/* JSON-escape a string into out (quotes/backslashes/control chars). */
static void json_escape(const char *s, char *out, size_t cap) {
    size_t n = 0;
    for (; *s && n + 6 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c < 0x20) {
            n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c);
        } else {
            out[n++] = (char)c;
        }
    }
    out[n] = 0;
}

/* Extract the first "url" or all "urls[]" entries into out URLs. */
static int extract_urls(const char *js, char out[][2048], int cap) {
    int n = 0;
    const char *p = js;
    /* single {"url": "..."} */
    {
        const char *f = strstr(js, "\"url\"");
        if (f) {
            const char *c = strchr(f + 5, ':');
            if (c) {
                c++;
                while (*c == ' ') c++;
                if (*c == '"') {
                    size_t k = 0;
                    c++;
                    while (*c && *c != '"' && k + 1 < 2048) {
                        if (*c == '\\' && c[1]) c++;
                        out[n][k++] = *c++;
                    }
                    out[n][k] = 0;
                    if (out[n][0]) n++;
                    return n;
                }
            }
        }
    }
    /* {"urls": ["a", "b"]} */
    p = strstr(js, "\"urls\"");
    if (!p) return n;
    p = strchr(p, '[');
    if (!p) return n;
    p++;
    while (*p && n < cap) {
        while (*p && *p != '"') {
            if (*p == ']') return n;
            p++;
        }
        if (*p != '"') break;
        p++;
        {
            size_t k = 0;
            while (*p && *p != '"' && k + 1 < 2048) {
                if (*p == '\\' && p[1]) p++;
                out[n][k++] = *p++;
            }
            out[n][k] = 0;
            if (*p == '"') p++;
            if (out[n][0]) n++;
        }
    }
    return n;
}

int main(int argc, char **argv) {
    static char msg[65536];
    static char urls[16][2048];
    static char resp[2048];
    static char payload[65536];
    static char rbody[65536];
    int code = 0;
#ifdef _WIN32
    WSADATA wd;
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return 1;
#endif
    if (argc > 1) g_port = atoi(argv[1]);
    if (g_port <= 0 || g_port > 65535) g_port = 15151;
    if (argc > 2)
        snprintf(g_key, sizeof(g_key), "%s", argv[2]);
    else {
        const char *env = getenv("CDM_API_KEY");
        if (env) snprintf(g_key, sizeof(g_key), "%s", env);
    }

    for (;;) {
        unsigned char hdr[4];
        unsigned int len;
        int n;
        if (read_all(hdr, 4) != 0) break; /* EOF: browser closed us */
        len = (unsigned int)hdr[0] | ((unsigned int)hdr[1] << 8) |
              ((unsigned int)hdr[2] << 16) | ((unsigned int)hdr[3] << 24);
        if (len == 0 || len >= sizeof(msg)) break;
        if (read_all(msg, len) != 0) break;
        msg[len] = 0;

        if (strstr(msg, "\"ping\"")) {
            if (http("GET", "/ping", NULL, rbody, sizeof(rbody), &code))
                write_msg("{\"ok\":true,\"pong\":true}");
            else
                write_msg("{\"ok\":false,\"error\":\"app not reachable\"}");
            continue;
        }
        n = extract_urls(msg, urls, 16);
        if (n <= 0) {
            write_msg("{\"ok\":false,\"error\":\"no url\"}");
            continue;
        }
        {
            size_t off = 0;
            int i;
            off += (size_t)snprintf(payload + off, sizeof(payload) - off, "[");
            for (i = 0; i < n; i++) {
                static char esc[4096];
                json_escape(urls[i], esc, sizeof(esc));
                off += (size_t)snprintf(payload + off, sizeof(payload) - off,
                                        "%s{\"link\":\"%s\"}", i ? "," : "", esc);
                if (off + 16 >= sizeof(payload)) break;
            }
            snprintf(payload + off, sizeof(payload) - off, "]");
        }
        if (http("POST", "/add", payload, rbody, sizeof(rbody), &code)) {
            snprintf(resp, sizeof(resp), "{\"ok\":true,\"count\":%d}", n);
            write_msg(resp);
        } else {
            write_msg("{\"ok\":false,\"error\":\"app not reachable\"}");
        }
    }
    return 0;
}
