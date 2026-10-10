#if !defined(_WIN32)

#include "tray.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* POSIX stub: tray/notifications need a desktop toolkit integration
 * (libappindicator / dbus). Auto-start via .desktop file is implemented. */

int cdm_tray_init(void) { return -1; }
void cdm_tray_shutdown(void) {}
void cdm_tray_tooltip(const char *text) { (void)text; }
void cdm_tray_notify(const char *title, const char *msg) {
    (void)title;
    (void)msg;
}
int cdm_tray_exit_requested(void) { return 0; }
int cdm_tray_show_requested(void) { return 0; }
int cdm_tray_add_url_requested(void) { return 0; }
int cdm_tray_clipboard_requested(char *out, size_t cap) {
    (void)out;
    (void)cap;
    return 0;
}
int cdm_tray_pause_all_requested(void) { return 0; }
int cdm_tray_settings_requested(void) { return 0; }

int cdm_autostart_get(void) {
    const char *home = getenv("HOME");
    char path[1024];
    FILE *f;
    if (!home || !*home) return 0;
    snprintf(path, sizeof(path), "%s/.config/autostart/cdm.desktop", home);
    f = fopen(path, "rb");
    if (f) fclose(f);
    return f ? 1 : 0;
}

static int localbin(char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    snprintf(out, cap, "%s/.local/bin", home);
    return 0;
}

static int exedir(char *out, size_t cap) {
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
}

int cdm_path_get(void) {
    char dir[1024], link[1150], target[2048];
    ssize_t n;
    if (localbin(dir, sizeof(dir)) != 0) return 0;
    snprintf(link, sizeof(link), "%s/cdm", dir);
    n = readlink(link, target, sizeof(target) - 1);
    return n > 0 ? 1 : 0;
}

int cdm_path_set(int on) {
    char dir[1024], exe[2048], link[1150], link2[1150];
    if (localbin(dir, sizeof(dir)) != 0) return -1;
    if (exedir(exe, sizeof(exe)) != 0) return -1;
    mkdir(dir, 0755);
    snprintf(link, sizeof(link), "%s/cdm", dir);
    snprintf(link2, sizeof(link2), "%s/cdm-gui", dir);
    if (on) {
        char t1[2200], t2[2200];
        snprintf(t1, sizeof(t1), "%s/cdm", exe);
        snprintf(t2, sizeof(t2), "%s/cdm-gui", exe);
        remove(link);
        remove(link2);
        if (symlink(t1, link) != 0) return -1;
        symlink(t2, link2); /* gui optional */
        return 0;
    }
    remove(link);
    remove(link2);
    return 0;
}

int cdm_autostart_set(int on) {
    const char *home = getenv("HOME");
    char dir[1024], path[1100], exe[2048];
    ssize_t n;
    if (!home || !*home) return -1;
    snprintf(dir, sizeof(dir), "%s/.config/autostart", home);
    snprintf(path, sizeof(path), "%s/cdm.desktop", dir);
    if (!on) {
        remove(path);
        return 0;
    }
    mkdir(dir, 0755);
    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return -1;
    exe[n] = 0;
    {
        FILE *f = fopen(path, "wb");
        if (!f) return -1;
        fprintf(f,
                "[Desktop Entry]\nType=Application\nName=cdm Download Manager\n"
                "Exec=%s --minimized\nX-GNOME-Autostart-enabled=true\n",
                exe);
        fclose(f);
    }
    return 0;
}

#endif /* !_WIN32 */
