#ifndef CDM_REGISTER_H
#define CDM_REGISTER_H

/* Install/remove browser native-messaging manifests for cdm-native-host.
 * `which`: "chrome", "firefox", "edge" or "all". Returns 0 on success. */
int cdm_register_native_host(const char *which);
int cdm_unregister_native_host(const char *which);

#endif /* CDM_REGISTER_H */
