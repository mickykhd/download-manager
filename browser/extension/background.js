/* cdm Browser Bridge - background service worker (MV3).
 *
 * Two routes into the desktop app (see ../../docs/browser-integration.md):
 *   1. REST (recommended): POST http://127.0.0.1:<port>/add - just needs
 *      the server enabled in cdm (Network dialog). No registry, no ids.
 *   2. Native messaging: chrome.runtime.sendNativeMessage("com.cdm.manager")
 *      - needs `cdm --register-native-host` plus the published extension id
 *      in the host manifest's allowed_origins.
 */

const DEFAULTS = {
  mode: "rest", // "rest" | "native"
  port: 15151,
  apiKey: "",
  autoCapture: true,
};

async function getSettings() {
  const s = await chrome.storage.sync.get(DEFAULTS);
  s.port = Number(s.port) || 15151;
  if (s.mode !== "native") s.mode = "rest";
  return s;
}

function setBadge(text, color) {
  chrome.action.setBadgeText({ text: text || "" }).catch(() => {});
  if (text && color) chrome.action.setBadgeBackgroundColor({ color }).catch(() => {});
}

/* ---------- routes into cdm ---------- */

async function sendRest(url, referrer) {
  const s = await getSettings();
  const headers = { "Content-Type": "application/json" };
  if (s.apiKey) headers["X-Api-Key"] = s.apiKey;
  const body = JSON.stringify(
    referrer ? { link: url, downloadPage: referrer } : { link: url }
  );
  const res = await fetch(`http://127.0.0.1:${s.port}/add`, {
    method: "POST",
    headers,
    body,
  });
  if (!res.ok) throw new Error(`cdm server replied ${res.status}`);
  return res.json().catch(() => ({}));
}

function sendNative(url) {
  return chrome.runtime.sendNativeMessage("com.cdm.manager", { url });
}

async function sendToCdm(url, referrer) {
  const s = await getSettings();
  try {
    if (s.mode === "native") await sendNative(url);
    else await sendRest(url, referrer);
    setBadge("");
    return { ok: true };
  } catch (e) {
    setBadge("!", "#c0392b");
    return { ok: false, error: String((e && e.message) || e) };
  }
}

/* ---------- capture points ---------- */

chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.create({
    id: "cdm-link",
    title: "Download with cdm",
    contexts: ["link"],
  });
  chrome.contextMenus.create({
    id: "cdm-page",
    title: "Download page with cdm",
    contexts: ["page"],
  });
});

chrome.contextMenus.onClicked.addListener((info, tab) => {
  if (info.menuItemId === "cdm-link" && info.linkUrl) {
    sendToCdm(info.linkUrl, tab && tab.url);
  } else if (info.menuItemId === "cdm-page" && tab && tab.url) {
    sendToCdm(tab.url, null);
  }
});

// Auto-capture browser downloads: cancel ours, let cdm fetch it instead.
chrome.downloads.onDeterminingFilename.addListener((item, suggest) => {
  (async () => {
    const s = await getSettings();
    if (!s.autoCapture || !item.url || item.url.startsWith("blob:")) return;
    try {
      await chrome.downloads.cancel(item.id);
    } catch (e) {
      return; // too late to intercept; let the browser finish it
    }
    try {
      await chrome.downloads.erase({ id: item.id });
    } catch (e) {
      /* best effort */
    }
    sendToCdm(item.url, item.referrer || null);
  })();
});

// Messages from popup.html.
chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  (async () => {
    if (msg && msg.type === "cdm-add" && msg.url) {
      sendResponse(await sendToCdm(msg.url, null));
    } else if (msg && msg.type === "cdm-ping") {
      const s = await getSettings();
      try {
        if (s.mode === "native") {
          await chrome.runtime.sendNativeMessage("com.cdm.manager", { ping: true });
          sendResponse({ ok: true });
        } else {
          const headers = {};
          if (s.apiKey) headers["X-Api-Key"] = s.apiKey;
          const res = await fetch(`http://127.0.0.1:${s.port}/ping`, { headers });
          sendResponse({ ok: res.ok });
        }
      } catch (e) {
        sendResponse({ ok: false, error: String((e && e.message) || e) });
      }
    } else {
      sendResponse({ ok: false, error: "unknown message" });
    }
  })();
  return true; // async response
});
