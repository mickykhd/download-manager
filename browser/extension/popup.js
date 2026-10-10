/* popup.html logic: status check + manual send. */
const statusEl = document.getElementById("status");
const urlEl = document.getElementById("url");

function showStatus(text, cls) {
  statusEl.textContent = text;
  statusEl.className = cls || "";
}

async function refresh() {
  showStatus("Checking cdm…", "");
  const r = await chrome.runtime.sendMessage({ type: "cdm-ping" });
  if (r && r.ok) showStatus("Connected to cdm.", "ok");
  else showStatus("Not connected: " + ((r && r.error) || "unknown"), "err");
}

document.getElementById("add").addEventListener("click", async () => {
  const url = urlEl.value.trim();
  if (!url) return;
  const r = await chrome.runtime.sendMessage({ type: "cdm-add", url });
  showStatus(r && r.ok ? "Sent to cdm." : "Failed: " + ((r && r.error) || "?"),
             r && r.ok ? "ok" : "err");
});

document.getElementById("tab").addEventListener("click", async () => {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  if (!tab || !tab.url) return;
  const r = await chrome.runtime.sendMessage({ type: "cdm-add", url: tab.url });
  showStatus(r && r.ok ? "Tab sent to cdm." : "Failed: " + ((r && r.error) || "?"),
             r && r.ok ? "ok" : "err");
});

document.addEventListener("DOMContentLoaded", refresh);
