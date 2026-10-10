/* options.html logic: load/save bridge settings. */
const DEFAULTS = { mode: "rest", port: 15151, apiKey: "", autoCapture: true };

async function load() {
  const s = await chrome.storage.sync.get(DEFAULTS);
  document.getElementById("autoCapture").checked = !!s.autoCapture;
  document.querySelector(`input[name="mode"][value="${s.mode === "native" ? "native" : "rest"}"]`).checked = true;
  document.getElementById("port").value = Number(s.port) || 15151;
  document.getElementById("apiKey").value = s.apiKey || "";
}

document.getElementById("save").addEventListener("click", async () => {
  const mode = document.querySelector('input[name="mode"]:checked').value;
  await chrome.storage.sync.set({
    autoCapture: document.getElementById("autoCapture").checked,
    mode,
    port: Number(document.getElementById("port").value) || 15151,
    apiKey: document.getElementById("apiKey").value.trim(),
  });
  const el = document.getElementById("saved");
  el.textContent = "Saved.";
  setTimeout(() => { el.textContent = ""; }, 1500);
});

document.addEventListener("DOMContentLoaded", load);
