#!/usr/bin/env node
// Read-only bridge: Paseo daemon (WebSocket SDK) -> tiny long-poll text feed for the Miyoo pager.
// It never sends prompts, permission answers, or any mutating RPC; it only lists agents.
import { createPaseoClient } from "@getpaseo/client";
import { createServer } from "node:http";
import { randomBytes, timingSafeEqual } from "node:crypto";
import { mkdirSync, readFileSync, realpathSync, renameSync, writeFileSync, existsSync } from "node:fs";
import { homedir, hostname } from "node:os";
import { dirname, join } from "node:path";

const CONFIG_DIR = process.env.PAGER_CONFIG_DIR ?? join(homedir(), ".config", "paseo-pager");
const DAEMON_URL = process.env.PAGER_DAEMON_URL ?? "ws://127.0.0.1:6767/ws";
const LISTEN = process.env.PAGER_LISTEN ?? "0.0.0.0:6780";
const LEDGER_PATH = join(CONFIG_DIR, "ledger.json");
const TOKEN_PATH = join(CONFIG_DIR, "token");
const MAX_AGENTS = 32;
const MAX_WAIT_S = 50;
const REFRESH_MS = 20_000;

const log = (...args) => console.log(new Date().toISOString(), ...args);

function loadToken() {
  if (process.env.PAGER_TOKEN) return process.env.PAGER_TOKEN;
  if (existsSync(TOKEN_PATH)) return readFileSync(TOKEN_PATH, "utf8").trim();
  mkdirSync(CONFIG_DIR, { recursive: true });
  const token = randomBytes(12).toString("hex");
  writeFileSync(TOKEN_PATH, token + "\n", { mode: 0o600 });
  log(`generated pager token in ${TOKEN_PATH}`);
  return token;
}

function writeJsonAtomic(path, value) {
  mkdirSync(dirname(path), { recursive: true });
  const tmp = `${path}.tmp`;
  writeFileSync(tmp, JSON.stringify(value, null, 2));
  renameSync(tmp, path);
}

// ---- Feed ledger: monotonic token totals the pet eats -------------------------------------
// Per-agent lastUsage is cumulative per session; we keep the last value seen per agent and add
// positive deltas. The very first snapshot after the ledger is created only sets baselines, so
// pre-existing history does not count. Agents first seen later count from zero.
// `today` accumulates the same deltas (output tokens, and cost in millicents) for the bridge-local
// day and starts over at local midnight.
const FAMILIES = ["claude", "gpt", "gemini", "other"];

function familyOf(model) {
  const m = (model ?? "").toLowerCase();
  if (m.includes("claude")) return "claude";
  if (/(^|\/)(gpt|o\d|codex)/.test(m) || m.includes("openai")) return "gpt";
  if (m.includes("gemini")) return "gemini";
  return "other";
}

// Bridge-local day number: floor((unix seconds + UTC offset) / 86400), the same day the device's pet uses.
function localDay(date = new Date()) {
  return Math.floor((date.getTime() / 1000 - date.getTimezoneOffset() * 60) / 86400);
}

const freshToday = () => ({ day: localDay(), out: 0, costMc: 0 });

function loadLedger() {
  if (existsSync(LEDGER_PATH)) {
    const saved = JSON.parse(readFileSync(LEDGER_PATH, "utf8"));
    saved.today ??= freshToday();
    return saved;
  }
  // `id` names this ledger; the device rebaselines its feed counters when it changes.
  return {
    id: randomBytes(6).toString("hex"),
    seeded: false,
    out: Object.fromEntries(FAMILIES.map((f) => [f, 0])),
    turns: 0,
    errors: 0,
    today: freshToday(),
    agents: {},
  };
}

const ledger = loadLedger();
let ledgerDirty = false;

// Returns true if the local day changed (today's totals start over).
function rollDay() {
  const day = localDay();
  if (ledger.today.day === day) return false;
  ledger.today = { day, out: 0, costMc: 0 };
  ledgerDirty = true;
  return true;
}

// A smaller value than last time means the provider session restarted its counter.
const growth = (cur, prev) => (cur >= prev ? cur - prev : cur);

function updateLedger(agents) {
  const seeding = !ledger.seeded;
  rollDay();
  for (const a of agents) {
    const out = a.lastUsage?.outputTokens ?? 0;
    const costMc = Math.round((a.lastUsage?.totalCostUsd ?? 0) * 100_000);
    const prev = ledger.agents[a.id];
    const fam = familyOf(a.runtimeInfo?.model ?? a.model);
    if (!prev) {
      if (!seeding) {
        ledger.out[fam] += out;
        ledger.today.out += out;
        ledger.today.costMc += costMc;
      }
    } else {
      const delta = growth(out, prev.out);
      ledger.out[fam] += delta;
      ledger.today.out += delta;
      // Ledgers written before cost tracking have no baseline: the first sighting only sets one.
      if (prev.costMc !== undefined) ledger.today.costMc += growth(costMc, prev.costMc);
      if (!seeding && prev.status === "running" && a.status === "idle") ledger.turns += 1;
      if (!seeding && prev.status !== "error" && a.status === "error") ledger.errors += 1;
    }
    if (!prev || prev.out !== out || prev.costMc !== costMc || prev.status !== a.status) ledgerDirty = true;
    ledger.agents[a.id] = { out, costMc, status: a.status };
  }
  if (seeding) {
    ledger.seeded = true;
    ledgerDirty = true;
  }
  if (ledgerDirty) {
    writeJsonAtomic(LEDGER_PATH, ledger);
    ledgerDirty = false;
  }
}

// ---- Rendering --------------------------------------------------------------------------
const BUCKET_PRIORITY = { needs_input: 0, failed: 1, running: 2, attention: 3, done: 4 };

function bucketOf(a) {
  if (a.pendingPermissions.length > 0 || a.attentionReason === "permission") return "needs_input";
  if (a.status === "error" || a.attentionReason === "error") return "failed";
  if (a.status === "running" || a.status === "initializing") return "running";
  if (a.requiresAttention) return "attention";
  return "done";
}

const TRANSLIT = { "\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"', "\u2013": "-", "\u2014": "-", "\u2026": "..." };

// The device font is printable ASCII; fields are tab-separated, so tabs/newlines must go.
function clean(s, max = 200) {
  const ascii = (s ?? "")
    .normalize("NFKD")
    .replace(/[\u0300-\u036f]/g, "")
    .replace(/[\u2018\u2019\u201c\u201d\u2013\u2014\u2026]/g, (c) => TRANSLIT[c])
    .replace(/\s+/g, " ")
    .replace(/[^\x20-\x7e]/g, "?")
    .trim();
  return (ascii || "-").slice(0, max);
}

// Fedora Atomic (Bazzite) symlinks /home -> /var/home, so agents may report either spelling.
const HOMES = [...new Set([homedir(), realpathSync(homedir())])];

function shortCwd(cwd) {
  const home = HOMES.find((h) => cwd === h || cwd?.startsWith(h + "/"));
  return home ? "~" + cwd.slice(home.length) : cwd;
}

const epoch = (iso) => (iso ? Math.floor(Date.parse(iso) / 1000) : 0);
const HOST = clean(hostname(), 32).replace(/ /g, "_");

let agents = [];
let daemonUp = false;
let body = "";
let rev = 0;
const waiters = new Set();

function render() {
  rollDay();
  const visible = agents
    .filter((a) => !a.archivedAt)
    .map((a) => ({ a, bucket: bucketOf(a) }))
    .sort(
      (x, y) =>
        BUCKET_PRIORITY[x.bucket] - BUCKET_PRIORITY[y.bucket] ||
        epoch(y.a.updatedAt) - epoch(x.a.updatedAt),
    )
    .slice(0, MAX_AGENTS);
  const lines = [
    "PAGER 1",
    `server\t${ledger.id}\t${HOST}`,
    `daemon\t${daemonUp ? "up" : "down"}`,
    `feed\t${FAMILIES.map((f) => ledger.out[f]).join("\t")}\t${ledger.turns}\t${ledger.errors}`,
    `today\t${ledger.today.day}\t${ledger.today.out}\t${ledger.today.costMc}`,
  ];
  for (const { a, bucket } of visible) {
    const u = a.lastUsage ?? {};
    const model = a.runtimeInfo?.model ?? a.model ?? a.provider;
    const ctxPct = u.contextWindowMaxTokens
      ? Math.round((100 * (u.contextWindowUsedTokens ?? 0)) / u.contextWindowMaxTokens)
      : 0;
    lines.push(
      [
        "agent",
        a.id.slice(0, 7),
        bucket,
        a.status,
        a.attentionReason ?? "-",
        u.outputTokens ?? 0,
        Math.round((u.totalCostUsd ?? 0) * 100),
        ctxPct,
        epoch(a.updatedAt),
        familyOf(model),
        clean(model, 48),
        clean(shortCwd(a.cwd), 64),
        clean(a.title),
      ].join("\t"),
    );
  }
  const next = lines.join("\n") + "\n";
  if (next !== body) {
    body = next;
    rev += 1;
    for (const wake of waiters) wake();
    waiters.clear();
  }
}

// ---- Paseo connection (read-only) -------------------------------------------------------
const client = createPaseoClient({
  url: DAEMON_URL,
  password: process.env.PASEO_PASSWORD,
  clientId: "paseo-pager-bridge",
  appVersion: "paseo-pager/0.1",
  reconnect: { enabled: true, baseDelayMs: 1000, maxDelayMs: 15_000 },
});

let refreshTimer = null;
let refreshing = false;
let refreshAgain = false;

async function refresh(subscribe = false) {
  if (refreshing) {
    refreshAgain = true;
    return;
  }
  refreshing = true;
  try {
    const result = await client.agents.list(subscribe ? { subscribe: {} } : {});
    agents = result.entries.map((e) => e.agent);
    daemonUp = true;
    updateLedger(agents);
  } catch (err) {
    daemonUp = false;
    log("list failed:", err?.message ?? err);
  } finally {
    refreshing = false;
    render();
  }
  if (refreshAgain) {
    refreshAgain = false;
    scheduleRefresh();
  }
}

function scheduleRefresh() {
  if (refreshTimer) return;
  refreshTimer = setTimeout(() => {
    refreshTimer = null;
    refresh();
  }, 400);
}

client.agents.subscribe(() => scheduleRefresh());

async function connectLoop() {
  for (;;) {
    try {
      await client.connect();
    } catch (err) {
      if (!daemonUp) log("connect failed:", err?.message ?? err);
    }
    if (client.getConnectionState().status === "connected") {
      await refresh(true);
      return;
    }
    daemonUp = false;
    render();
    await new Promise((r) => setTimeout(r, 5000));
  }
}

let lastConn = "idle";
setInterval(() => {
  const state = client.getConnectionState().status;
  if (state !== lastConn) {
    log("daemon connection:", state);
    if (state === "connected" && lastConn !== "idle") refresh(true);
    lastConn = state;
  }
  if (state === "connected") refresh();
  else {
    // render() also starts a new day at local midnight while the daemon is away.
    daemonUp = false;
    render();
  }
}, REFRESH_MS);

// ---- HTTP long-poll feed ----------------------------------------------------------------
const TOKEN = loadToken();
const tokenBuf = Buffer.from(TOKEN);

function authorized(given) {
  const buf = Buffer.from(given ?? "");
  return buf.length === tokenBuf.length && timingSafeEqual(buf, tokenBuf);
}

const server = createServer((req, res) => {
  const url = new URL(req.url, "http://pager");
  if (req.method !== "GET") {
    res.writeHead(405).end();
    return;
  }
  if (url.pathname === "/healthz") {
    res.writeHead(200, { "content-type": "text/plain" }).end(daemonUp ? "ok\n" : "daemon down\n");
    return;
  }
  if (url.pathname !== "/pager") {
    res.writeHead(404).end();
    return;
  }
  if (!authorized(url.searchParams.get("token"))) {
    res.writeHead(401, { "content-type": "text/plain" }).end("bad token\n");
    return;
  }
  const since = Number(url.searchParams.get("rev") ?? -1);
  const wait = Math.min(MAX_WAIT_S, Math.max(0, Number(url.searchParams.get("wait") ?? 0)));
  const send = () => {
    const now = new Date();
    const head = `rev\t${rev}\nnow\t${Math.floor(now.getTime() / 1000)}\t${-now.getTimezoneOffset() * 60}\n`;
    const [first, ...rest] = body.split("\n");
    res.writeHead(200, { "content-type": "text/plain; charset=us-ascii", "cache-control": "no-store" });
    res.end([first, head.trimEnd(), ...rest].join("\n"));
  };
  if (since !== rev || wait === 0) {
    send();
    return;
  }
  let done = false;
  const finish = () => {
    if (done) return;
    done = true;
    clearTimeout(timer);
    waiters.delete(finish);
    send();
  };
  const timer = setTimeout(finish, wait * 1000);
  waiters.add(finish);
  // res 'close' means the socket went away (or we already answered); req 'close' can fire as soon
  // as the bodyless GET has been read, which would orphan the waiter.
  res.on("close", () => {
    done = true;
    clearTimeout(timer);
    waiters.delete(finish);
  });
});

const [listenHost, listenPort] = LISTEN.includes(":") ? LISTEN.split(":") : ["0.0.0.0", LISTEN];
render();
server.listen(Number(listenPort), listenHost, () => {
  log(`pager feed on http://${listenHost}:${listenPort}/pager (daemon ${DAEMON_URL})`);
});
connectLoop();

for (const sig of ["SIGINT", "SIGTERM"]) {
  process.on(sig, async () => {
    server.close();
    await client.close().catch(() => {});
    process.exit(0);
  });
}
