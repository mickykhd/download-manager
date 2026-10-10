#ifndef CDM_IPC_H
#define CDM_IPC_H

/* Local REST integration server (browser extensions, scripts).
 *
 *   GET  /ping                   -> {"pong":true}
 *   GET  /queues                 -> [{"id":0,"name":"Default"},...]
 *   POST /add                    -> {"link":"url",...} or [{"link":"u1"},...]
 *                                    queues URLs for the Add dialog
 *   POST /start-headless-download -> {"downloadSource":"url","folder":"..",
 *                                     "name":"..","queueId":0}
 *                                    enqueues directly, no dialog
 *
 * Listens on 127.0.0.1 only. When api_key is non-empty, every request
 * must carry header "X-Api-Key: <key>" (else 401).
 */

typedef struct cdm_manager cdm_manager;

/* Start (or restart) the server. port<=0 picks the default (15151).
 * Returns the bound port, or -1 on failure. Safe to call repeatedly;
 * a running server is stopped first. */
int cdm_ipc_start(cdm_manager *m, int port, const char *api_key);
void cdm_ipc_stop(void);
int cdm_ipc_port(void); /* bound port, or -1 when down */

#endif /* CDM_IPC_H */
