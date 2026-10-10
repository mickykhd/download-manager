/* Unit tests for ../background.js with mocked browser APIs.
 * Run: node --test test/
 */
"use strict";
const test = require("node:test");
const assert = require("node:assert");

function makeChrome(settings) {
  const handlers = { message: null, menuClick: null, filename: null };
  const calls = { fetch: [], cancel: [], erase: [], badge: [], native: [] };
  const chrome = {
    storage: {
      sync: {
        get: async (defs) => ({ ...defs, ...settings }),
        set: async (obj) => Object.assign(settings, obj),
      },
    },
    action: {
      setBadgeText: async (o) => calls.badge.push(["text", o.text]),
      setBadgeBackgroundColor: async (o) => calls.badge.push(["color", o.color]),
    },
    runtime: {
      onInstalled: { addListener: () => {} },
      onMessage: { addListener: (fn) => { handlers.message = fn; } },
      sendNativeMessage: async (host, msg) => {
        calls.native.push([host, msg]);
        return { ok: true };
      },
    },
    contextMenus: {
      create: () => {},
      onClicked: { addListener: (fn) => { handlers.menuClick = fn; } },
    },
    downloads: {
      onDeterminingFilename: { addListener: (fn) => { handlers.filename = fn; } },
      cancel: async (id) => { calls.cancel.push(id); },
      erase: async (q) => { calls.erase.push(q); },
    },
  };
  return { chrome, handlers, calls };
}

function loadBackground(chromeMock, fetchMock) {
  global.chrome = chromeMock;
  global.fetch = fetchMock;
  delete require.cache[require.resolve("../background.js")];
  require("../background.js");
}

function sendMessage(handlers, msg) {
  return new Promise((resolve) => handlers.message(msg, {}, resolve));
}

test("REST add posts link JSON", async () => {
  const { chrome, handlers, calls } = makeChrome({});
  let posted = null;
  loadBackground(chrome, async (url, opts) => {
    posted = { url, opts };
    return { ok: true, json: async () => ({}) };
  });
  const r = await sendMessage(handlers, { type: "cdm-add", url: "https://x/f.zip" });
  assert.equal(r.ok, true);
  assert.match(posted.url, /127\.0\.0\.1:15151\/add/);
  assert.deepEqual(JSON.parse(posted.opts.body), { link: "https://x/f.zip" });
  assert.equal(calls.badge.filter((b) => b[0] === "text" && b[1] === "!").length, 0);
});

test("REST failure reports error + badge", async () => {
  const { chrome, handlers, calls } = makeChrome({});
  loadBackground(chrome, async () => { throw new Error("conn refused"); });
  const r = await sendMessage(handlers, { type: "cdm-add", url: "https://x/f.zip" });
  assert.equal(r.ok, false);
  assert.match(r.error, /conn refused/);
  assert.ok(calls.badge.some((b) => b[0] === "text" && b[1] === "!"));
});

test("ping checks server", async () => {
  const { chrome, handlers } = makeChrome({});
  let pinged = null;
  loadBackground(chrome, async (url) => {
    pinged = url;
    return { ok: true };
  });
  const r = await sendMessage(handlers, { type: "cdm-ping" });
  assert.equal(r.ok, true);
  assert.match(pinged, /\/ping/);
});

test("API key header is sent when configured", async () => {
  const { chrome, handlers } = makeChrome({ apiKey: "s3cr3t" });
  let headers = null;
  loadBackground(chrome, async (url, opts) => {
    headers = opts.headers;
    return { ok: true, json: async () => ({}) };
  });
  await sendMessage(handlers, { type: "cdm-add", url: "https://x/f.zip" });
  assert.equal(headers["X-Api-Key"], "s3cr3t");
});

test("context menu sends link", async () => {
  const { chrome, handlers } = makeChrome({});
  let bodies = [];
  loadBackground(chrome, async (url, opts) => {
    bodies.push(opts.body);
    return { ok: true, json: async () => ({}) };
  });
  await handlers.menuClick(
    { menuItemId: "cdm-link", linkUrl: "https://x/a.zip" },
    { url: "https://x/page" }
  );
  await new Promise((r) => setTimeout(r, 20));
  assert.equal(bodies.length, 1);
  assert.deepEqual(JSON.parse(bodies[0]), { link: "https://x/a.zip", downloadPage: "https://x/page" });
});

test("auto-capture cancels browser download and forwards", async () => {
  const { chrome, handlers, calls } = makeChrome({ autoCapture: true });
  let bodies = [];
  loadBackground(chrome, async (url, opts) => {
    bodies.push(opts.body);
    return { ok: true, json: async () => ({}) };
  });
  await handlers.filename({ id: 7, url: "https://x/b.zip", referrer: "" }, () => {});
  await new Promise((r) => setTimeout(r, 20));
  assert.deepEqual(calls.cancel, [7]);
  assert.equal(bodies.length, 1);
});

test("auto-capture off leaves downloads alone", async () => {
  const { chrome, handlers, calls } = makeChrome({ autoCapture: false });
  let fetched = 0;
  loadBackground(chrome, async () => {
    fetched++;
    return { ok: true, json: async () => ({}) };
  });
  await handlers.filename({ id: 7, url: "https://x/b.zip" }, () => {});
  await new Promise((r) => setTimeout(r, 20));
  assert.equal(calls.cancel.length, 0);
  assert.equal(fetched, 0);
});

test("native mode uses sendNativeMessage", async () => {
  const { chrome, handlers, calls } = makeChrome({ mode: "native" });
  let fetched = 0;
  loadBackground(chrome, async () => {
    fetched++;
    return { ok: true, json: async () => ({}) };
  });
  const r = await sendMessage(handlers, { type: "cdm-add", url: "https://x/f.zip" });
  assert.equal(r.ok, true);
  assert.equal(fetched, 0);
  assert.deepEqual(calls.native, [["com.cdm.manager", { url: "https://x/f.zip" }]]);
});
