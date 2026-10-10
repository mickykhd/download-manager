#ifndef CDM_TRAY_H
#define CDM_TRAY_H

/* System tray (Windows: Shell_NotifyIcon balloon + menu; elsewhere: stub).
 * The tray lives on its own thread with a message-only window; the main
 * (GLFW) thread only calls the functions below. Menu actions arrive as
 * flags polled by the main loop. */

int cdm_tray_init(void);    /* 0 ok; non-zero => tray unavailable */
void cdm_tray_shutdown(void);
/* tooltip: "cdm - 2 active (1.2 MB/s)" style; pass NULL to reset */
void cdm_tray_tooltip(const char *text);
/* balloon notification (no-op when tray is down) */
void cdm_tray_notify(const char *title, const char *msg);
/* polled by the main loop (auto-cleared on read) */
int cdm_tray_exit_requested(void);
int cdm_tray_show_requested(void);

/* Auto-start on login. Windows: HKCU Run key; Linux: autostart .desktop. */
int cdm_autostart_get(void);
int cdm_autostart_set(int on);

/* Per-user PATH management for the install's bin dir (no length limits,
 * unlike the NSIS installer helper). Windows: HKCU Environment/Path +
 * broadcast; POSIX: ~/.local/bin symlinks. */
int cdm_path_get(void);      /* 1 when the install bin is on the user PATH */
int cdm_path_set(int on);    /* 0 ok, -1 failed */

#endif /* CDM_TRAY_H */
