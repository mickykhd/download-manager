#include "internal.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* Apply user-agent / referer / cookie / extra headers / proxy from cfg.
 * When `slist` is non-NULL, extra headers are appended to *slist (caller
 * still owns it and must apply CURLOPT_HTTPHEADER itself). */
void cdm_apply_conn_opts(CURL *c, const cdm_config *cfg,
                         struct curl_slist **slist) {
    curl_easy_setopt(c, CURLOPT_USERAGENT,
                     (cfg->user_agent && *cfg->user_agent)
                         ? cfg->user_agent : CDM_USER_AGENT);
    if (cfg->referer && *cfg->referer)
        curl_easy_setopt(c, CURLOPT_REFERER, cfg->referer);
    if (cfg->cookie && *cfg->cookie)
        curl_easy_setopt(c, CURLOPT_COOKIE, cfg->cookie);
    if (slist && cfg->extra_headers && cfg->n_extra_headers > 0) {
        for (int i = 0; i < cfg->n_extra_headers; i++) {
            const char *h = cfg->extra_headers[i];
            if (h && *h && strchr(h, ':'))
                *slist = curl_slist_append(*slist, h);
        }
    }

    switch (cfg->proxy_mode) {
    case CDM_PROXY_MANUAL:
        if (cfg->proxy_url && *cfg->proxy_url)
            curl_easy_setopt(c, CURLOPT_PROXY, cfg->proxy_url);
        break;
    case CDM_PROXY_SYSTEM: {
        /* libcurl already honours http_proxy/https_proxy/all_proxy env.
         * On Windows additionally honour the WinINET (IE/Edge) proxy so
         * "use system proxy" works out of the box. */
#ifdef _WIN32
        {
            HKEY hk;
            if (RegOpenKeyExA(HKEY_CURRENT_USER,
                              "Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
                              0, KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
                DWORD en = 0, sz = sizeof(en);
                if (RegQueryValueExA(hk, "ProxyEnable", NULL, NULL,
                                     (LPBYTE)&en, &sz) == ERROR_SUCCESS && en) {
                    char server[512] = {0}, bypass[1024] = {0};
                    DWORD ssz = sizeof(server), bsz = sizeof(bypass);
                    if (RegQueryValueExA(hk, "ProxyServer", NULL, NULL,
                                         (LPBYTE)server, &ssz) == ERROR_SUCCESS && server[0]) {
                        /* "http=h:pt;https=h:pt" | "h:pt" | "http://h:pt" */
                        const char *pick = server;
                        const char *hs = strstr(server, "https=");
                        const char *ht = strstr(server, "http=");
                        if (hs) pick = hs + 6;
                        else if (ht) pick = ht + 5;
                        {
                            char one[512], *semi;
                            snprintf(one, sizeof(one), "%s", pick);
                            semi = strchr(one, ';');
                            if (semi) *semi = 0;
                            if (strncmp(one, "http://", 7) != 0 &&
                                strncmp(one, "https://", 8) != 0 &&
                                strncmp(one, "socks", 5) != 0) {
                                char full[520];
                                snprintf(full, sizeof(full), "http://%s", one);
                                curl_easy_setopt(c, CURLOPT_PROXY, full);
                                /* full is stack: copy it (curl copies strings). */
                            } else {
                                curl_easy_setopt(c, CURLOPT_PROXY, one);
                            }
                        }
                        if (RegQueryValueExA(hk, "ProxyOverride", NULL, NULL,
                                             (LPBYTE)bypass, &bsz) == ERROR_SUCCESS && bypass[0]) {
                            /* "a;b;<local>" -> curl comma list */
                            char nb[1024];
                            size_t k = 0;
                            for (size_t i = 0; bypass[i] && k + 1 < sizeof(nb); i++)
                                nb[k++] = (bypass[i] == ';') ? ',' : bypass[i];
                            nb[k] = 0;
                            curl_easy_setopt(c, CURLOPT_NOPROXY, nb);
                        }
                    }
                }
                RegCloseKey(hk);
            }
        }
#else
        (void)0;
#endif
        break;
    }
    case CDM_PROXY_DIRECT:
    default:
        /* ignore environment proxies entirely */
        curl_easy_setopt(c, CURLOPT_PROXY, "");
        break;
    }
}
