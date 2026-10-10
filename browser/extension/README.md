# cdm Browser Bridge (extension)

MV3 extension (Chrome + Firefox) that sends downloads to the cdm desktop
app. REST-first: it talks to cdm's local server, so there is nothing to
register — just enable the server and go.

## Try it unpacked (development)

1. In cdm: **Network** dialog → tick *Browser integration server* →
   **Apply server** (note the port, default `15151`).
2. Chrome: `chrome://extensions` → enable **Developer mode** →
   **Load unpacked** → pick this folder.
   Firefox: `about:debugging#/runtime/this-firefox` → **Load Temporary
   Add-on** → pick `manifest.json`.
3. Click the cdm toolbar icon → it should say *Connected to cdm.*
4. Right-click any link → **Download with cdm** → the Add dialog opens
   in cdm. Toggle auto-capture in the extension options to intercept
   normal downloads too.

## Native messaging mode (optional)

For setups where localhost HTTP is blocked, switch **Connection** to
*Native messaging* in the extension options, then on the computer run:

```powershell
cdm --register-native-host all
```

After the extension is published, replace `__CDM_EXTENSION_ID__` in the
generated host manifests (`cdm --register-native-host` prints their
paths) with the real extension id and re-run the command.

## Tests

```sh
node --test test/
```
