/*
 * Integration-server tests: real HTTP round-trips against 127.0.0.1 on a
 * fixed test port. No external network.
 */
#include "manager.h"
#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int sock_t;
#endif

#define TEST_PORT 18099

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else         { fprintf(stderr, "ok:   %s\n", msg); } \
} while (0)

/* send a raw request, capture status code + body */
static int call(const char *req, char *body, size_t bcap) {
    sock_t s;
    struct sockaddr_in addr;
    static char resp[262144];
    size_t total = 0;
    int code = 0;
    const char *p;
    s = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return -1;
#else
    if (s < 0) return -1;
#endif
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(TEST_PORT);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return -1;
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
    sscanf(resp, "HTTP/%*d.%*d %d", &code);
    p = strstr(resp, "\r\n\r\n");
    if (body && bcap > 0)
        snprintf(body, bcap, "%s", p ? p + 4 : "");
    return code;
}

static void req(char *out, size_t cap, const char *method, const char *path,
                const char *key, const char *payload) {
    char kh[160] = {0};
    if (key)
        snprintf(kh, sizeof(kh), "X-Api-Key: %s\r\n", key);
    snprintf(out, cap,
             "%s %s HTTP/1.0\r\nHost: x\r\n%sContent-Length: %lu\r\n\r\n%s",
             method, path, kh,
             (unsigned long)(payload ? strlen(payload) : 0),
             payload ? payload : "");
}

int main(void) {
#ifdef _WIN32
    {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
    }
#endif
    cdm_manager *m = cdm_manager_create();
    static char r[8192], b[8192];
    int code;
    int port = cdm_ipc_start(m, TEST_PORT, "testkey123");
    CHECK(port == TEST_PORT, "server binds test port");
    CHECK(cdm_ipc_port() == TEST_PORT, "port reported");

    req(r, sizeof(r), "GET", "/ping", NULL, NULL);
    CHECK(call(r, b, sizeof(b)) == 401, "ping without key rejected");

    req(r, sizeof(r), "GET", "/ping", "wrong", NULL);
    CHECK(call(r, b, sizeof(b)) == 401, "ping with bad key rejected");

    req(r, sizeof(r), "GET", "/ping", "testkey123", NULL);
    code = call(r, b, sizeof(b));
    CHECK(code == 200 && strstr(b, "pong") != NULL, "ping+pong");

    req(r, sizeof(r), "GET", "/queues", "testkey123", NULL);
    code = call(r, b, sizeof(b));
    CHECK(code == 200 && strstr(b, "Default") != NULL, "queues list");

    req(r, sizeof(r), "GET", "/nope", "testkey123", NULL);
    CHECK(call(r, b, sizeof(b)) == 404, "unknown endpoint 404");

    req(r, sizeof(r), "POST", "/add", "testkey123",
        "[{\"link\":\"http://127.0.0.1:9/a.bin\"},{\"link\":\"http://127.0.0.1:9/b.bin\"}]");
    code = call(r, b, sizeof(b));
    CHECK(code == 200, "add accepted");
    {
        static char urls[16][2048];
        int n = cdm_manager_poll_urls(m, urls, 16);
        CHECK(n == 2, "two URLs land in inbox");
        CHECK(n == 2 && strcmp(urls[0], "http://127.0.0.1:9/a.bin") == 0,
              "first URL intact");
    }

    req(r, sizeof(r), "POST", "/start-headless-download", "testkey123",
        "{\"downloadSource\":\"http://127.0.0.1:9/c.bin\",\"queueId\":0}");
    code = call(r, b, sizeof(b));
    CHECK(code == 200 && strstr(b, "\"id\"") != NULL, "headless enqueue ok");
    {
        cdm_job *j = cdm_manager_find(m, m->next_id - 1);
        CHECK(j && j->state == JOB_QUEUED, "headless job queued");
    }

    cdm_ipc_stop();
    CHECK(cdm_ipc_port() == -1, "server stopped");
    cdm_manager_destroy(m);

    if (failures) { fprintf(stderr, "%d test(s) failed\n", failures); return 1; }
    fprintf(stderr, "all ipc tests passed\n");
    return 0;
}
