#include "register.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define MKDIR(d) _mkdir(d)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define MKDIR(d) mkdir((d), 0755)
#endif

#define CDM_NMH_NAME "com.cdm.manager"
/* Placeholder origin: replaced with the real extension id on publish. */
#define CDM_NMH_CHROME_ORIGIN "chrome-extension://__CDM_EXTENSION_ID__/"

static int exe_dir(char *out, size_t cap) {
#ifdef _WIN32
    char path[2048];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof(path));
    char *s;
    if (n == 0 || n >= sizeof(path)) return -1;
    s = strrchr(path, '\\');
    if (!s) return -1;
    *s = 0;
    snprintf(out, cap, "%s", path);
    return 0;
#else
    char path[2048];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    char *s;
    if (n <= 0) return -1;
    path[n] = 0;
    s = strrchr(path, '/');
    if (!s) return -1;
    *s = 0;
    snprintf(out, cap, "%s", path);
    return 0;
#endif
}

static int write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return 0;
}

#ifdef _WIN32
static int reg_set(const char *subkey, const char *value) {
    HKEY hk;
    LONG rc = RegCreateKeyExA(HKEY_CURRENT_USER, subkey, 0, NULL, 0,
                              KEY_SET_VALUE, NULL, &hk, NULL);
    if (rc != ERROR_SUCCESS) return -1;
    rc = RegSetValueExA(hk, CDM_NMH_NAME, 0, REG_SZ,
                        (const BYTE *)value, (DWORD)(strlen(value) + 1));
    RegCloseKey(hk);
    return rc == ERROR_SUCCESS ? 0 : -1;
}

static int reg_del(const char *subkey) {
    HKEY hk;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, subkey, 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
        return 0; /* nothing there */
    RegDeleteValueA(hk, CDM_NMH_NAME);
    RegCloseKey(hk);
    return 0;
}
#endif

static int do_one(const char *which, int unreg) {
    char dir[2048], host[2200], man[2200];
    const char *appdata;
    int is_firefox = which && strcmp(which, "firefox") == 0;
    if (exe_dir(dir, sizeof(dir)) != 0) {
        fprintf(stderr, "cdm: cannot locate own install dir\n");
        return -1;
    }
#ifdef _WIN32
    snprintf(host, sizeof(host), "%s\\cdm-native-host.exe", dir);
#else
    snprintf(host, sizeof(host), "%s/cdm-native-host", dir);
#endif
    /* manifests live next to the binaries when writable, else user config */
    snprintf(man, sizeof(man), "%s/%s-%s.json", dir, CDM_NMH_NAME, which);
    {
        FILE *probe = fopen(man, "wb");
        if (!probe) {
#ifdef _WIN32
            appdata = getenv("LOCALAPPDATA");
            if (!appdata || !*appdata) appdata = getenv("APPDATA");
            if (!appdata || !*appdata) {
                fprintf(stderr, "cdm: no writable manifest location\n");
                return -1;
            }
            snprintf(man, sizeof(man), "%s\\cdm\\%s-%s.json",
                     appdata, CDM_NMH_NAME, which);
            {
                char parent[2200];
                snprintf(parent, sizeof(parent), "%s", man);
                {
                    char *s = strrchr(parent, '\\');
                    if (s) {
                        *s = 0;
                        MKDIR(parent);
                    }
                }
            }
#else
            const char *home = getenv("HOME");
            if (!home || !*home) {
                fprintf(stderr, "cdm: no writable manifest location\n");
                return -1;
            }
            if (is_firefox)
                snprintf(man, sizeof(man), "%s/.mozilla/native-messaging-hosts/%s.json",
                         home, CDM_NMH_NAME);
            else
                snprintf(man, sizeof(man), "%s/.config/google-chrome/NativeMessagingHosts/%s-%s.json",
                         home, CDM_NMH_NAME, which);
#endif
        } else {
            fclose(probe);
            remove(man);
        }
    }

    if (unreg) {
#ifdef _WIN32
        if (strcmp(which, "chrome") == 0)
            reg_del("SOFTWARE\\Google\\Chrome\\NativeMessagingHosts");
        else if (strcmp(which, "edge") == 0)
            reg_del("SOFTWARE\\Microsoft\\Edge\\NativeMessagingHosts");
        else if (is_firefox)
            reg_del("SOFTWARE\\Mozilla\\NativeMessagingHosts");
        remove(man);
#else
        remove(man);
#endif
        fprintf(stderr, "cdm: native-messaging host removed for %s\n", which);
        return 0;
    }

    /* escape backslashes for JSON */
    {
        char jhost[4400];
        size_t k = 0;
        for (size_t i = 0; host[i] && k + 2 < sizeof(jhost); i++) {
            if (host[i] == '\\') {
                jhost[k++] = '\\';
                jhost[k++] = '\\';
            } else {
                jhost[k++] = host[i];
            }
        }
        jhost[k] = 0;
        if (is_firefox) {
            char doc[4800];
            snprintf(doc, sizeof(doc),
                     "{\n"
                     "  \"name\": \"%s\",\n"
                     "  \"description\": \"cdm Download Manager bridge\",\n"
                     "  \"path\": \"%s\",\n"
                     "  \"type\": \"stdio\",\n"
                     "  \"allowed_extensions\": [\"cdm-native@example\"]\n"
                     "}\n",
                     CDM_NMH_NAME, jhost);
            if (write_file(man, doc) != 0) {
                fprintf(stderr, "cdm: cannot write %s\n", man);
                return -1;
            }
        } else {
            char doc[4800];
            snprintf(doc, sizeof(doc),
                     "{\n"
                     "  \"name\": \"%s\",\n"
                     "  \"description\": \"cdm Download Manager bridge\",\n"
                     "  \"path\": \"%s\",\n"
                     "  \"type\": \"stdio\",\n"
                     "  \"allowed_origins\": [\"%s\"]\n"
                     "}\n",
                     CDM_NMH_NAME, jhost, CDM_NMH_CHROME_ORIGIN);
            if (write_file(man, doc) != 0) {
                fprintf(stderr, "cdm: cannot write %s\n", man);
                return -1;
            }
        }
    }
#ifdef _WIN32
    {
        const char *sub = NULL;
        if (strcmp(which, "chrome") == 0)
            sub = "SOFTWARE\\Google\\Chrome\\NativeMessagingHosts";
        else if (strcmp(which, "edge") == 0)
            sub = "SOFTWARE\\Microsoft\\Edge\\NativeMessagingHosts";
        else if (is_firefox)
            sub = "SOFTWARE\\Mozilla\\NativeMessagingHosts";
        if (sub && reg_set(sub, man) != 0) {
            fprintf(stderr, "cdm: cannot write registry for %s\n", which);
            return -1;
        }
    }
#else
    (void)appdata;
#endif
    fprintf(stderr, "cdm: native-messaging host registered for %s\n  %s\n",
            which, man);
    return 0;
}

static int want(const char *which, const char *name) {
    return strcmp(which, "all") == 0 || strcmp(which, name) == 0;
}

int cdm_register_native_host(const char *which) {
    int rc = 0;
    if (!which) which = "all";
    if (want(which, "chrome") && do_one("chrome", 0) != 0) rc = -1;
    if (want(which, "edge") && do_one("edge", 0) != 0) rc = -1;
    if (want(which, "firefox") && do_one("firefox", 0) != 0) rc = -1;
    if (!want(which, "chrome") && !want(which, "edge") &&
        !want(which, "firefox") && strcmp(which, "all") != 0) {
        fprintf(stderr, "cdm: unknown browser '%s' (chrome|edge|firefox|all)\n",
                which);
        return -1;
    }
    return rc;
}

int cdm_unregister_native_host(const char *which) {
    int rc = 0;
    if (!which) which = "all";
    if (want(which, "chrome") && do_one("chrome", 1) != 0) rc = -1;
    if (want(which, "edge") && do_one("edge", 1) != 0) rc = -1;
    if (want(which, "firefox") && do_one("firefox", 1) != 0) rc = -1;
    return rc;
}
