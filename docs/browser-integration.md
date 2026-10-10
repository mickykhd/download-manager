# Browser integration

cdm exposes a local REST server plus a Native Messaging host bridge, so
browser extensions (or scripts) can send links to the running app without
touching its windows.

## 1. Enable the server

GUI: **Network** dialog → check *Browser integration server*, set a port
(`0` = default `15151`) and an optional API key → **Apply server**.

Or seed the settings file (`%APPDATA%/cdm/settings.conf` on Windows,
`~/.config/cdm/settings.conf` elsewhere):

```
CDM-SETTINGS1
api_enabled 1
api_port 15151
api_key s3cr3t
```

The server listens on `127.0.0.1` only. When a key is set, every request
must carry header `X-Api-Key: <key>` (otherwise HTTP 401).

## 2. REST API

- `GET /ping` → `{"pong":true}` (health check, still key-gated)
- `GET /queues` → `[{"id":0,"name":"Default"},...]`
- `POST /add` — body `{"link":"https://..."}`
  or `[{"link":"https://a"}, {"link":"https://b"}]` →
  `{"ok":true}`. URLs land in the app inbox; the GUI opens the
  Add dialog (one URL) or the Batch dialog (many).
- `POST /start-headless-download` — body
  `{"downloadSource":"https://...","folder":"...","name":"...",
  "queueId":0}` → `{"ok":true,"id":7}`. Enqueues with no dialog
  (`queueId` omitted = download immediately).

Example:

```powershell
Invoke-RestMethod http://127.0.0.1:15151/queues -Headers @{"X-Api-Key"="s3cr3t"}
```

## 3. Native Messaging host (for extension authors)

`cdm-native-host[.exe]` bridges Chrome/Firefox native messaging
(uint32-LE length + JSON over stdio) to the REST server above.

Incoming messages:

```json
{"url": "https://example.com/file.zip"}
{"urls": ["https://example.com/a.zip", "https://example.com/b.zip"]}
{"ping": true}
```

Replies: `{"ok":true,...}` or `{"ok":false,"error":"..."}`.
Port override: first argv (`cdm-native-host 15200`); API key via
second argv or the `CDM_API_KEY` environment variable.

Install the manifests (writes the JSON files and, on Windows, the
`HKCU\...\NativeMessagingHosts\com.cdm.manager` registry values):

```powershell
cdm --register-native-host all        # chrome|edge|firefox|all
cdm --unregister-native-host all
```

Manifests reference the installed `cdm-native-host` binary, so run the
command from the installed `cdm` (or reinstall after moving it).

> Chrome/Edge manifests ship with
> `"allowed_origins": ["chrome-extension://__CDM_EXTENSION_ID__/"]`.
> Replace `__CDM_EXTENSION_ID__` with your extension id after publishing
> it. Firefox uses `"allowed_extensions": ["cdm-native@example"]` —
> match this id in your extension manifest.
