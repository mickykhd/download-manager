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
