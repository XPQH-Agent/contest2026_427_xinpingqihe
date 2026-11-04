#!/usr/bin/env node
/**
 * tekon-usage-scan v1.0
 * Cross-agent token usage scanner. Read-only: never mutates source session stores.
 */
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { readDshEvents, readJsonlFile, walk, parseJsonl } from './dsh-codec.mjs';
import { pathToFileURL } from 'node:url';

const VERSION = '1.1.0';
const CMD = process.argv[2] || 'help';
const ARGV = process.argv.slice(3);

function opt(name, fallback) {
  const key = '--' + name;
  const i = ARGV.indexOf(key);
  if (i >= 0 && i + 1 < ARGV.length) return ARGV[i + 1];
  const pref = key + '=';
  const hit = ARGV.find(function (a) { return a.startsWith(pref); });
  if (hit) return hit.slice(pref.length);
  return fallback;
}
function flag(name) { return ARGV.includes('--' + name); }
function ensureDir(p) { fs.mkdirSync(p, { recursive: true }); }
function nowIso() { return new Date().toISOString(); }
function readJson(p, fallback) {
  try { return JSON.parse(fs.readFileSync(p, 'utf8')); } catch { return fallback; }
}
function writeJson(p, v) {
  ensureDir(path.dirname(p));
  fs.writeFileSync(p, JSON.stringify(v, null, 2), 'utf8');
}
function num(v) { const n = Number(v); return Number.isFinite(n) ? n : 0; }
function clamp0(n) { return n > 0 ? n : 0; }
function toMs(v) {
  if (v === null || v === undefined || v === '') return null;
  if (typeof v === 'number') return v > 1e12 ? Math.round(v) : Math.round(v * 1000);
  const t = Date.parse(String(v));
  return Number.isFinite(t) ? t : null;
}
function iso(ms) { return ms ? new Date(ms).toISOString() : null; }
function textOf(content) {
  if (typeof content === 'string') return content;
  if (Array.isArray(content)) {
    return content.map(function (x) {
      if (typeof x === 'string') return x;
      if (x && typeof x === 'object') return x.text || x.thinking || '';
      return '';
    }).join('\n');
  }
  if (content && typeof content === 'object') return content.text || content.content || '';
  return '';
}
function cleanPrompt(s) {
  let t = String(s || '');
  const q = t.match(/<user_query>([\s\S]*?)<\/user_query>/i);
  if (q) t = q[1];
  t = t.replace(/<system-reminder[\s\S]*?<\/system-reminder>/gi, ' ');
  t = t.replace(/<[^>]+>/g, ' ').replace(/\s+/g, ' ').trim();
  if (!t) return '';
  if (t.startsWith('The approval policy')) return '';
  return t.slice(0, 500);
}
function normPath(p) {
  if (!p) return '';
  let s = String(p).replace(/\\/g, '/');
  while (s.length > 1 && s.endsWith('/')) s = s.slice(0, -1);
  return s.toLowerCase();
}
function under(cwd, root) {
  const c = normPath(cwd);
  const r = normPath(root);
  if (!c || !r) return false;
  return c === r || c.startsWith(r + '/');
}
function sourceDefaults(source, index) {
  const id = source.id || ('source-' + index);
  return {
    id: id,
    type: source.type,
    enabled: source.enabled !== false,
    path: source.path || '',
    agent_id: source.agent_id || id,
    agent_name: source.agent_name || id,
    platform: source.platform || process.platform,
    host: source.host || os.hostname(),
    official: source.official === true,
    official_tool: source.official_tool === true,
    collection_mode: source.collection_mode || (source.official ? 'official-logs' : 'local-session-store'),
    login: source.login || '',
    team_id: source.team_id || ''
  };
}
function baseSession(source, sid, file) {
  return {
    source_id: source.id,
    agent_id: source.agent_id,
    agent_name: source.agent_name,
    platform: source.platform,
    host: source.host,
    type: source.type,
    official: source.official === true,
    official_tool: source.official_tool === true,
    collection_mode: source.collection_mode,
    session_id: sid,
    parent_session_id: null,
    title: '',
    cwd: '',
    started_at: null,
    last_event_at: null,
    requests: 0,
    input_tokens: 0,
    cache_read_tokens: 0,
    cache_write_tokens: 0,
    prompt_tokens: 0,
    output_tokens: 0,
    total_tokens: 0,
    models: {},
    first_user: '',
    session_key: source.id + ':' + sid,
    fingerprint: '',
    score: 0,
    hits: {},
    suggested: 'review',
    decision: 'review',
    included_in_total: false,
    duplicate_requests: 0,
    notes: '',
    source_file: file
  };
}
function addModel(map, model) {
  if (!model) model = 'unknown';
  map[model] = (map[model] || 0) + 1;
}
function addUsage(s, u) {
  s.input_tokens += num(u.input_tokens);
  s.cache_read_tokens += num(u.cache_read_tokens);
  s.cache_write_tokens += num(u.cache_write_tokens);
  s.output_tokens += num(u.output_tokens);
}

// ---------- DSH ----------
function dshSessionFiles(root) {
  const out = [];
  const st = function (p) { try { return fs.statSync(p); } catch { return null; } };
  const direct = st(root);
  if (direct && direct.isFile()) return [root];
  const names = function (dir) {
    let fsx = [];
    try { fsx = fs.readdirSync(dir); } catch { return []; }
    return fsx.filter(function (f) {
      return /^session(\.v[0-9]+)?\.jsonl(\.zstd|\.zst)?$/.test(f);
    }).sort(function (a, b) {
      const rank = function (x) { return x.includes('.v4.') ? 4 : x.includes('.v3.') ? 3 : 2; };
      return rank(b) - rank(a);
    });
  };
  if (names(root).length) {
    out.push(path.join(root, names(root)[0]));
    return out;
  }
  let sessionsDir = root;
  if (!/sessions$/i.test(root.replace(/\\/g, '/'))) sessionsDir = path.join(root, 'sessions');
  if (!st(sessionsDir)) return out;
  for (const slug of fs.readdirSync(sessionsDir)) {
    const sd = path.join(sessionsDir, slug);
    if (!st(sd) || !st(sd).isDirectory()) continue;
    const n = names(sd);
    if (n.length) { out.push(path.join(sd, n[0])); continue; }
    for (const id of fs.readdirSync(sd)) {
      const idDir = path.join(sd, id);
      const ist = st(idDir);
      if (!ist || !ist.isDirectory()) continue;
      const n2 = names(idDir);
      if (n2.length) out.push(path.join(idDir, n2[0]));
    }
  }
  return out;
}

export function dshDebug(root) {
  const st = function (p) { try { return fs.statSync(p); } catch (e) { return null; } };
  const names = function (dir) {
    let fsx = [];
    try { fsx = fs.readdirSync(dir); } catch (e) { return []; }
    return fsx.filter(function (f) { return /^session(\.v[0-9]+)?\.jsonl(\.zstd|\.zst)?$/.test(f); });
  };
  const sessionsDir = path.join(root, 'sessions');
  const out = { root: root, sessionsDir: sessionsDir, exists: !!st(sessionsDir), slugs: [], hits: [] };
  if (!st(sessionsDir)) return out;
  for (const slug of fs.readdirSync(sessionsDir)) {
    const sd = path.join(sessionsDir, slug);
    const s = st(sd);
    out.slugs.push({ slug: slug, isDir: s ? s.isDirectory() : null, names: names(sd) });
  }
  return out;
}

function scanDshFile(file, source) {
  const events = readDshEvents(file);
  const sidHint = path.basename(path.dirname(file));
  const s = baseSession(source, sidHint, file);
  const msgs = [];
  let meta = {};
  let title = '';
  let firstUser = '';
  let userText = '';
  let started = null;
  let last = null;
  let idx = 0;
  for (const e of events) {
    const t = toMs(e.time || e.timestamp || (e.data && (e.data.time || e.data.createdAt)));
    if (t) { started = started === null ? t : Math.min(started, t); last = last === null ? t : Math.max(last, t); }
    if (e.type === 'session') {
      meta = Object.assign({}, meta, e, e.data || {});
      if (e.cwd) s.cwd = e.cwd;
      if (e.id) s.session_id = e.id;
      if (e.parentSession) s.parent_session_id = e.parentSession;
    }
    if (typeof e.type === 'string' && e.type.indexOf('title') >= 0) {
      const d = e.data || {};
      title = d.title || d.text || d.value || title;
    }
    if (e.type === 'user/message') {
      const d = e.data || {};
      const txt = textOf((d.message && d.message.content) || d.content);
      if (userText.length < 8000) userText += ' ' + txt;
      if (!firstUser) { const c = cleanPrompt(txt); if (c) firstUser = c; }
    }
    if (e.type === 'assistant/message') {
      idx += 1;
      const d = e.data || {};
      const m = d.message || {};
      const u = d.usage || m.usage || {};
      const src = m.source || {};
      const model = src.model ? ((src.provider ? src.provider + '/' : '') + src.model) : '';
      const id = m.id || d.messageId || (source.id + ':a' + idx);
      const rec = {
        id: id,
        input_tokens: num(u.inputTokens || u.input_tokens),
        cache_read_tokens: num(u.cacheReadTokens || u.cache_read_input_tokens),
        cache_write_tokens: num(u.cacheWriteTokens || u.cache_creation_input_tokens),
        output_tokens: num(u.outputTokens || u.output_tokens)
      };
      rec.prompt_tokens = rec.input_tokens + rec.cache_read_tokens + rec.cache_write_tokens;
      rec.total_tokens = rec.prompt_tokens + rec.output_tokens;
      msgs.push(rec);
      addUsage(s, rec);
      addModel(s.models, model);
    }
  }
  if (meta.id) s.session_id = meta.id;
  if (meta.cwd) s.cwd = meta.cwd;
  if (meta.parentSession) s.parent_session_id = meta.parentSession;
  if (meta.parentSessionId) s.parent_session_id = meta.parentSessionId;
  s.title = title || firstUser || s.session_id;
  s.first_user = firstUser;
  s.user_text = userText.slice(0, 8000);
  s.started_at = iso(started) || iso(toMs(meta.createdAt)) || null;
  s.last_event_at = iso(last);
  s.requests = msgs.length;
  s.fingerprint = msgs.length ? msgs[0].id : s.session_id;
  s._messages = msgs;
  s.prompt_tokens = s.input_tokens + s.cache_read_tokens + s.cache_write_tokens;
  s.total_tokens = s.prompt_tokens + s.output_tokens;
  return s;
}

function dedupeDsh(sessions) {
  const group = sessions.filter(function (s) { return s.type === 'dsh'; });
  group.sort(function (a, b) { return String(a.started_at || '').localeCompare(String(b.started_at || '')); });
  const seen = new Set();
  for (const s of group) {
    const uniq = [];
    let dup = 0;
    for (const m of (s._messages || [])) {
      if (seen.has(m.id)) { dup += 1; continue; }
      seen.add(m.id);
      uniq.push(m);
    }
    s.input_tokens = 0; s.cache_read_tokens = 0; s.cache_write_tokens = 0; s.output_tokens = 0;
    for (const m of uniq) addUsage(s, m);
    s.prompt_tokens = s.input_tokens + s.cache_read_tokens + s.cache_write_tokens;
    s.total_tokens = s.prompt_tokens + s.output_tokens;
    s.requests = uniq.length;
    s.duplicate_requests = dup;
    if (dup > 0) s.notes = (s.notes ? s.notes + '; ' : '') + 'deduped ' + dup + ' copied assistant messages';
    delete s._messages;
  }
}

// ---------- WorkBuddy ----------
let DatabaseSync = null;
try { ({ DatabaseSync } = await import('node:sqlite')); } catch { /* optional */ }

function scanWorkbuddySource(source) {
  const root = source.path;
  const sessions = [];
  let dbRows = {};
  const dbFile = path.join(root, 'workbuddy.db');
  if (DatabaseSync && fs.existsSync(dbFile)) {
    try {
      const db = new DatabaseSync(dbFile, { readOnly: true });
      for (const r of db.prepare('select id,title,cwd,model,created_at,updated_at from sessions').all()) dbRows[r.id] = r;
      db.close();
    } catch (e) { /* transcript-only mode */ }
  }
  const projectDir = path.join(root, 'projects');
  const files = walk(projectDir, function (f) { return f.endsWith('.jsonl'); }, 0, 6);
  for (const file of files) {
    const sid = path.basename(file, '.jsonl');
    const db = dbRows[sid] || {};
    const s = baseSession(source, sid, file);
    const seen = new Set();
    let started = null;
    let last = null;
    let firstUser = '';
    let firstMid = '';
    let title = db.title || '';
    let cwd = db.cwd || '';
    let userText = '';
    for (const e of readJsonlFile(file)) {
      const t = toMs(e.timestamp || e.createdAt || e.time);
      if (t) { started = started === null ? t : Math.min(started, t); last = last === null ? t : Math.max(last, t); }
      if (!cwd && e.cwd) cwd = e.cwd;
      const type = e.type || '';
      const role = e.role || (e.message && e.message.role) || '';
      if ((type === 'message' || type === 'user') && role === 'user') {
        const rawText = textOf((e.message && e.message.content) || e.content);
        if (userText.length < 8000) userText += ' ' + rawText;
        if (!firstUser) { const c = cleanPrompt(rawText); if (c) firstUser = c; }
      }
      const pd = e.providerData || (e.data && e.data.providerData) || {};
      const ru = pd.rawUsage || pd.raw_usage || null;
      const mid = pd.messageId || e.messageId || '';
      if (!ru || !mid || seen.has(mid)) continue;
      seen.add(mid);
      if (!firstMid) firstMid = mid;
      const prompt = num(ru.prompt_tokens) || (num(ru.prompt_cache_hit_tokens) + num(ru.prompt_cache_miss_tokens));
      const cache = num(ru.prompt_cache_hit_tokens) || 0;
      const output = num(ru.completion_tokens);
      const rec = {
        input_tokens: clamp0(prompt - cache),
        cache_read_tokens: cache,
        cache_write_tokens: 0,
        output_tokens: output
      };
      rec.prompt_tokens = prompt;
      rec.total_tokens = prompt + output;
      s.input_tokens += rec.input_tokens;
      s.cache_read_tokens += rec.cache_read_tokens;
      s.output_tokens += rec.output_tokens;
      addModel(s.models, pd.model || pd.requestModelName || db.model || 'unknown');
    }
    s.prompt_tokens = s.input_tokens + s.cache_read_tokens;
    s.total_tokens = s.prompt_tokens + s.output_tokens;
    s.requests = seen.size;
    s.cwd = cwd;
    s.title = title || firstUser || sid;
    s.first_user = firstUser;
    s.user_text = userText.slice(0, 8000);
    s.started_at = iso(started);
    s.last_event_at = iso(last);
    s.fingerprint = firstMid || sid;
    if (!DatabaseSync) s.notes = 'node:sqlite unavailable; transcript-only';
    sessions.push(s);
  }
  return sessions;
}

// ---------- Claude Code ----------
function scanClaudeSource(source) {
  const root = source.path;
  const projectDir = path.join(root, 'projects');
  const files = walk(projectDir, function (f) { return f.endsWith('.jsonl'); }, 0, 6);
  const sessions = [];
  for (const file of files) {
    const sid = path.basename(file, '.jsonl');
    const s = baseSession(source, sid, file);
    s.official_tool = true;
    s.collection_mode = 'local-transcript';
    const seen = new Set();
    let started = null, last = null, firstUser = '', idx = 0, firstMid = '', cwd = '';
    for (const e of readJsonlFile(file)) {
      const t = toMs(e.timestamp || e.time);
      if (t) { started = started === null ? t : Math.min(started, t); last = last === null ? t : Math.max(last, t); }
      if (e.cwd) cwd = e.cwd;
      const m = e.message || {};
      if (e.type === 'user' && !firstUser) {
        const c = cleanPrompt(textOf(m.content || e.content));
        if (c) firstUser = c;
      }
      if (e.type !== 'assistant' || !m.usage) continue;
      const usage = m.usage;
      const mid = m.id || e.uuid || (sid + ':a' + (++idx));
      if (seen.has(mid)) continue;
      seen.add(mid);
      idx += 1;
      if (!firstMid) firstMid = mid;
      const rec = {
        input_tokens: num(usage.input_tokens),
        cache_read_tokens: num(usage.cache_read_input_tokens),
        cache_write_tokens: num(usage.cache_creation_input_tokens),
        output_tokens: num(usage.output_tokens)
      };
      rec.prompt_tokens = rec.input_tokens + rec.cache_read_tokens + rec.cache_write_tokens;
      rec.total_tokens = rec.prompt_tokens + rec.output_tokens;
      s.input_tokens += rec.input_tokens;
      s.cache_read_tokens += rec.cache_read_tokens;
      s.cache_write_tokens += rec.cache_write_tokens;
      s.output_tokens += rec.output_tokens;
      addModel(s.models, m.model || 'claude');
    }
    s.prompt_tokens = s.input_tokens + s.cache_read_tokens + s.cache_write_tokens;
    s.total_tokens = s.prompt_tokens + s.output_tokens;
    s.requests = seen.size;
    s.cwd = cwd;
    s.title = firstUser || sid;
    s.first_user = firstUser;
    s.started_at = iso(started);
    s.last_event_at = iso(last);
    s.fingerprint = firstMid || sid;
    sessions.push(s);
  }
  return sessions;
}

// ---------- Codex ----------
function scanCodexSource(source) {
  const root = source.path;
  const dir = path.join(root, 'sessions');
  const files = walk(dir, function (f) { return f.endsWith('.jsonl'); }, 0, 8);
  const sessions = [];
  for (const file of files) {
    const sid = path.basename(file, '.jsonl');
    const s = baseSession(source, sid, file);
    s.official_tool = true;
    s.collection_mode = 'local-transcript';
    let started = null, last = null, cwd = '', model = '', best = null, turns = 0, firstUser = '';
    for (const e of readJsonlFile(file)) {
      const t = toMs(e.timestamp || e.time);
      if (t) { started = started === null ? t : Math.min(started, t); last = last === null ? t : Math.max(last, t); }
      const p = e.payload || {};
      if (e.type === 'session_meta') {
        if (p.cwd) cwd = p.cwd;
        if (p.id) s.session_id = p.id;
      }
      if (e.type === 'turn_context') { turns += 1; if (p.model) model = p.model; }
      if (e.type === 'event_msg' && p.type === 'user_message' && !firstUser) {
        const c = cleanPrompt(p.message || p.text);
        if (c) firstUser = c;
      }
      if (e.type === 'event_msg' && p.type === 'token_count') {
        const u = (p.info && p.info.total_token_usage) || p.total_token_usage || null;
        if (u && (!best || num(u.total_tokens) >= num(best.total_tokens))) best = u;
      }
    }
    if (best) {
      const input = num(best.input_tokens);
      const cached = num(best.cached_input_tokens);
      s.input_tokens = clamp0(input - cached);
      s.cache_read_tokens = cached;
      s.output_tokens = num(best.output_tokens);
      s.prompt_tokens = input;
      s.total_tokens = input + s.output_tokens;
      addModel(s.models, model || 'codex');
    }
    s.requests = turns || (best ? 1 : 0);
    s.cwd = cwd;
    s.title = firstUser || sid;
    s.first_user = firstUser;
    s.started_at = iso(started);
    s.last_event_at = iso(last);
    s.fingerprint = sid;
    sessions.push(s);
  }
  return sessions;
}

// ---------- Official openvela logs ----------
function scanOfficialSource(source) {
  const root = source.path;
  const files = walk(root, function (f) { return f.endsWith('.jsonl'); }, 0, 4);
  const manifest = readJson(path.join(root, 'manifest.json'), null);
  const byId = {};
  if (manifest && Array.isArray(manifest.sessions)) {
    for (const x of manifest.sessions) byId[x.session_id] = x;
  }
  const audit = [];
  const sessions = [];
  for (const file of files) {
    const base = path.basename(file, '.jsonl');
    const ix = base.indexOf('__');
    const tool = ix >= 0 ? base.slice(0, ix) : 'unknown';
    const sid = ix >= 0 ? base.slice(ix + 2) : base;
    const man = byId[sid] || {};
    let events = 0, asst = 0, turns = 0, tin = 0, tout = 0, first = null, last = null, prevTin = null;
    const models = {};
    for (const e of readJsonlFile(file)) {
      events += 1;
      const t = toMs(e.ts || e.time || e.timestamp);
      if (t) { first = first === null ? t : Math.min(first, t); last = last === null ? t : Math.max(last, t); }
      const role = e.role || (e.message && e.message.role) || (e.data && e.data.message && e.data.message.role);
      const isAsst = e.type === 'assistant' || role === 'assistant' || (typeof e.type === 'string' && e.type.indexOf('assistant') >= 0);
      const u = e.usage || (e.message && e.message.usage) || {};
      if (isAsst) {
        asst += 1;
        const tinEv = num(e.tokens_in) || num(e.tokensIn) || num(u.input_tokens);
        const toutEv = num(e.tokens_out) || num(e.tokensOut) || num(u.output_tokens);
        // Claude Code emits one API response as several assistant events (thinking/text/tool).
        // They repeat tokens_in; count the prompt once per consecutive identical block.
        if (prevTin === null || tinEv !== prevTin) { tin += tinEv; turns += 1; }
        tout += toutEv;
        prevTin = tinEv;
      } else {
        prevTin = null;
      }
      if (e.model) addModel(models, e.model);
      if (u.model) addModel(models, u.model);
    }
    const ms = first ? Math.round(first / 1000) : null;
    const ml = last ? Math.round(last / 1000) : null;
    const mstart = man.started_at ? Math.round(Date.parse(man.started_at) / 1000) : null;
    const mlast = man.last_event_at ? Math.round(Date.parse(man.last_event_at) / 1000) : null;
    const delta = Math.max(
      ms !== null && mstart !== null ? Math.abs(ms - mstart) : 0,
      ml !== null && mlast !== null ? Math.abs(ml - mlast) : 0
    );
    const auditRow = {
      file: path.relative(root, file).replace(/\\/g, '/'),
      tool: tool,
      session_id: sid,
      events: events,
      assistant_events: asst,
      tokens_in: tin,
      tokens_out: tout,
      models: Object.keys(models).join('; '),
      first_ts: iso(first),
      last_ts: iso(last),
      manifest_started_at: man.started_at || '',
      manifest_last_event_at: man.last_event_at || '',
      ts_delta_sec: delta,
      manifest_event_count: man.event_count === undefined ? '' : man.event_count,
      event_count_match: man.event_count === undefined ? '' : (Number(man.event_count) === events ? 'yes' : 'no'),
      health: man.health || '',
      collection_mode: man.collection_mode || '',
      file_path: file
    };
    audit.push(auditRow);
    const s = baseSession(source, sid, file);
    s.agent_id = source.agent_id || ('official-' + tool);
    s.agent_name = source.agent_name || ('官方 LOGS / ' + tool);
    s.type = 'official_logs';
    s.official = true;
    s.official_tool = true;
    s.collection_mode = 'official-logs';
    s.title = tool + ' / ' + sid;
    s.requests = turns || asst;
    s.input_tokens = tin;
    s.cache_read_tokens = 0;
    s.output_tokens = tout;
    s.prompt_tokens = tin;
    s.total_tokens = tin + tout;
    s.started_at = iso(first);
    s.last_event_at = iso(last);
    s.models = models;
    s.fingerprint = tool + ':' + sid;
    s.suggested = 'exclude';
    s.notes = 'official evidence only; excluded from supplementary total';
    sessions.push(s);
  }
  return { sessions: sessions, audit: audit, manifest: manifest };
}

// ---------- classification / output ----------
function countOf(hay, needle) {
  let n = 0, pos = 0;
  while (needle && true) {
    const i = hay.indexOf(needle, pos);
    if (i < 0) break;
    n += 1; pos = i + needle.length;
  }
  return n;
}

function classify(s, cfg) {
  const p = cfg.project || {};
  const roots = p.roots || [];
  const titleText = String(s.title || '').toLowerCase();
  const promptText = String(s.first_user || '').toLowerCase();
  const bodyText = String(s.user_text || '').toLowerCase();
  const text = [s.title, s.first_user, s.cwd, s.user_text].join(' ').toLowerCase();
  let score = 0;
  const hits = {};
  if (s.cwd && roots.some(function (r) { return under(s.cwd, r); })) score += 40;
  for (const kw of (p.keywords || [])) {
    const k = String(kw).toLowerCase();
    if (!k) continue;
    const nt = countOf(titleText, k);
    const nf = countOf(promptText, k);
    const nb = countOf(bodyText, k);
    const n = nt + nf + nb;
    if (n) {
      hits[kw] = n;
      // A keyword in the session title or opening prompt is strong evidence;
      // later-message hits are weaker because long sessions drift off-topic.
      score += Math.min(50, nt * 50 + nf * 30 + Math.min(20, nb * 5));
    }
  }
  score = Math.min(score, 150);
  for (const kw of (p.exclude_keywords || [])) {
    if (String(kw) && text.indexOf(String(kw).toLowerCase()) >= 0) { score -= 100; hits['EXCLUDE:' + kw] = 1; }
  }
  let reason = '';
  const t = s.started_at ? Date.parse(s.started_at) : null;
  if (p.since && t !== null && t < Date.parse(p.since)) { score = -100; reason = 'before since'; }
  if (p.until && t !== null && t > Date.parse(p.until)) { score = -100; reason = 'after until'; }
  s.score = score;
  s.hits = hits;
  if (s.type === 'official_logs') { s.suggested = 'exclude'; s.decision = 'exclude'; return s; }
  if (score >= num(p.include_score === undefined ? 50 : p.include_score)) s.suggested = 'include';
  else if (score <= num(p.exclude_score === undefined ? 0 : p.exclude_score)) s.suggested = 'exclude';
  else s.suggested = 'review';
  s.decision = s.suggested;
  if (reason) s.notes = (s.notes ? s.notes + '; ' : '') + reason;
  return s;
}

function applyAgentRules(s, cfg) {
  const rules = cfg.agent_rules || [];
  if (!rules.length) return;
  const hay = [s.title, s.first_user, s.cwd, s.agent_name].join(' ').toLowerCase();
  for (const rule of rules) {
    const m = String(rule.match || '').toLowerCase();
    if (!m || hay.indexOf(m) < 0) continue;
    if (rule.agent_id) s.agent_id = rule.agent_id;
    if (rule.agent_name) s.agent_name = rule.agent_name;
    if (rule.platform) s.platform = rule.platform;
    s.notes = (s.notes ? s.notes + '; ' : '') + 'agent rule: ' + rule.match;
    break;
  }
}

function applyDecisions(sessions, decisions) {
  for (const s of sessions) {
    if (s.type === 'official_logs') { s.decision = 'exclude'; s.included_in_total = false; continue; }
    const d = decisions[s.session_key];
    if (d === 'include' || d === 'exclude') s.decision = d;
    s.included_in_total = s.decision === 'include';
  }
}

function csvCell(v) {
  const s = String(v === null || v === undefined ? '' : v);
  if (/[",\r\n]/.test(s)) return '"' + s.replace(/"/g, '""') + '"';
  return s;
}
function writeCsv(file, rows, cols, bom) {
  const lines = [cols.join(',')];
  for (const r of rows) lines.push(cols.map(function (c) { return csvCell(r[c]); }).join(','));
  fs.writeFileSync(file, (bom ? '\uFEFF' : '') + lines.join('\n') + '\n', 'utf8');
}
function candidatesRows(sessions) {
  return sessions.filter(function (s) { return s.type !== 'official_logs'; }).sort(function (a, b) {
    return (b.score || 0) - (a.score || 0);
  }).map(function (s) {
    return {
      session_key: s.session_key,
      agent: s.agent_name,
      type: s.type,
      title: s.title,
      cwd: s.cwd,
      started_at: s.started_at || '',
      requests: s.requests,
      total_tokens: s.total_tokens,
      score: s.score,
      suggested: s.suggested,
      hits: Object.keys(s.hits || {}).join('; '),
      first_user: String(s.first_user || '').slice(0, 180),
      source_file: s.source_file
    };
  });
}

// ---------- commands ----------
function findOfficialRoots(projectDir) {
  const out = [];
  const bases = [projectDir, path.dirname(projectDir)];
  for (const b of bases) {
    const logs = path.join(b, 'logs');
    if (!fs.existsSync(logs)) continue;
    for (const e of fs.readdirSync(logs, { withFileTypes: true })) {
      if (!e.isDirectory()) continue;
      const p = path.join(logs, e.name);
      if (fs.existsSync(path.join(p, 'manifest.json'))) out.push(p);
    }
  }
  return out;
}

function cmdInit() {
  const projectDir = path.resolve(opt('project-dir', process.cwd()));
  const out = path.resolve(opt('out', path.join(projectDir, 'tekon-usage')));
  const home = os.homedir();
  const sources = [];
  const add = function (type, p, name, platform, extra) {
    if (!fs.existsSync(p)) return;
    sources.push(Object.assign({ id: type, type: type, enabled: true, path: p, agent_name: name, platform: platform, host: os.hostname(), official: false }, extra || {}));
  };
  add('dsh', path.join(home, '.dsh'), 'DSH 原体（本机）', 'Windows/local');
  add('workbuddy', path.join(home, '.workbuddy'), 'WORKAGENT（WorkBuddy）', 'Windows/local');
  add('claude', path.join(home, '.claude'), 'Claude Code（本机）', 'Windows/local', { official_tool: true });
  add('codex', path.join(home, '.codex'), 'Codex（本机）', 'Windows/local', { official_tool: true });
  const officialArg = opt('official-logs', '');
  const officialRoots = officialArg ? [path.resolve(officialArg)] : findOfficialRoots(projectDir);
  officialRoots.forEach(function (p, i) {
    const login = opt('github-login', path.basename(p));
    sources.push({
      id: 'official-' + (i + 1), type: 'official_logs', enabled: true, path: p,
      agent_name: '官方 LOGS / ' + login, platform: 'official', host: 'repo',
      official: true, official_tool: true, login: login,
      team_id: opt('team-id', '')
    });
  });
  const cfg = {
    schema_version: '1.0',
    project: {
      name: opt('project-name', path.basename(projectDir)),
      repo: opt('repo', ''),
      roots: [projectDir].concat(opt('roots', '').split(/[;,]/).filter(Boolean)),
      keywords: opt('keywords', '').split(/[,;]/).filter(Boolean),
      exclude_keywords: opt('exclude-keywords', '').split(/[,;]/).filter(Boolean),
      since: opt('since', null),
      until: opt('until', null),
      timezone: opt('timezone', Intl.DateTimeFormat().resolvedOptions().timeZone || 'UTC'),
      include_score: 50,
      exclude_score: 0
    },
    sources: sources,
    manual_agents: [],
    output: { dir: out }
  };
  ensureDir(out);
  writeJson(path.join(out, 'tekon.config.json'), cfg);
  console.log(JSON.stringify({ ok: true, config: path.join(out, 'tekon.config.json'), sources: sources.map(function (s) { return { id: s.id, type: s.type, path: s.path }; }) }, null, 2));
}

function cmdScan() {
  const cfgFile = path.resolve(opt('config', 'tekon.config.json'));
  const cfg = readJson(cfgFile, null);
  if (!cfg) throw new Error('missing config: ' + cfgFile);
  const out = path.resolve(opt('out', cfg.output && cfg.output.dir ? cfg.output.dir : path.dirname(cfgFile)));
  ensureDir(path.join(out, 'raw'));
  const all = [];
  const audits = [];
  let officialManifest = null;
  for (let i = 0; i < (cfg.sources || []).length; i++) {
    const raw = cfg.sources[i];
    const src = sourceDefaults(raw, i);
    if (!src.enabled) continue;
    let got = [];
    if (src.type === 'dsh') {
      for (const f of dshSessionFiles(src.path)) got.push(scanDshFile(f, src));
    } else if (src.type === 'workbuddy') {
      got = scanWorkbuddySource(src);
    } else if (src.type === 'claude') {
      got = scanClaudeSource(src);
    } else if (src.type === 'codex') {
      got = scanCodexSource(src);
    } else if (src.type === 'official_logs') {
      const r = scanOfficialSource(src);
      got = r.sessions; audits.push.apply(audits, r.audit);
      if (r.manifest) officialManifest = r.manifest;
    } else {
      console.error('WARN unknown source type: ' + src.type + ' (' + src.id + ')');
    }
    for (const s of got) { classify(s, cfg); applyAgentRules(s, cfg); all.push(s); }
  }
  dedupeDsh(all);
  const decisions = readJson(path.join(out, 'decisions.json'), {});
  applyDecisions(all, decisions);
  writeJson(path.join(out, 'raw', 'sessions.json'), all);
  writeJson(path.join(out, 'raw', 'official_audit.json'), audits);
  writeJson(path.join(out, 'raw', 'official_manifest.json'), officialManifest);
  writeCsv(path.join(out, 'candidates.csv'), candidatesRows(all), ['session_key', 'agent', 'type', 'title', 'cwd', 'started_at', 'requests', 'total_tokens', 'score', 'suggested', 'hits', 'first_user', 'source_file'], true);
  const meta = {
    scanner: 'tekon-usage-scan@' + VERSION,
    generated_at: nowIso(),
    config_file: cfgFile,
    sources: (cfg.sources || []).length,
    sessions: all.length,
    included: all.filter(function (s) { return s.included_in_total; }).length,
    review: all.filter(function (s) { return s.suggested === 'review'; }).length,
    official_logs: audits.length
  };
  writeJson(path.join(out, 'raw', 'scan_meta.json'), meta);
  console.log(JSON.stringify(meta, null, 2));
}

function cmdCollect() {
  const type = opt('type', 'dsh');
  const p = path.resolve(opt('path', '.'));
  const out = path.resolve(opt('out', 'tekon-collect'));
  ensureDir(path.join(out, 'raw'));
  const src = sourceDefaults({ id: opt('agent-id', type), type: type, path: p, agent_name: opt('agent-name', type), platform: opt('platform', 'remote') }, 0);
  let got = [];
  if (type === 'dsh') { for (const f of dshSessionFiles(p)) got.push(scanDshFile(f, src)); dedupeDsh(got); }
  else if (type === 'workbuddy') got = scanWorkbuddySource(src);
  else if (type === 'claude') got = scanClaudeSource(src);
  else if (type === 'codex') got = scanCodexSource(src);
  else if (type === 'official_logs') { const r = scanOfficialSource(src); got = r.sessions; writeJson(path.join(out, 'raw', 'official_audit.json'), r.audit); }
  else throw new Error('unknown type: ' + type);
  const cfgFile = opt('config', '');
  const cfg = cfgFile ? readJson(path.resolve(cfgFile), null) : null;
  const projectCfg = cfg || { project: { roots: [], keywords: [] }, manual_agents: [], output: { dir: out } };
  for (const s of got) { classify(s, projectCfg); applyAgentRules(s, projectCfg); }
  applyDecisions(got, {});
  writeJson(path.join(out, 'raw', 'sessions.json'), got);
  console.log(JSON.stringify({ ok: true, type: type, sessions: got.length, out: path.join(out, 'raw', 'sessions.json') }, null, 2));
}

function cmdMerge() {
  const inputs = opt('inputs', '');
  const out = path.resolve(opt('out', 'raw/sessions.json'));
  let files = [];
  if (inputs) {
    for (const part of inputs.split(/[;,]/)) {
      const p = path.resolve(part);
      if (fs.existsSync(p) && fs.statSync(p).isDirectory()) files = files.concat(walk(p, function (f) { return f.endsWith('.json'); }, 0, 2));
      else if (fs.existsSync(p)) files.push(p);
      else {
        const dir = path.dirname(p);
        const base = path.basename(p);
        const rx = new RegExp('^' + base.replace(/[.]/g, '\\.').replace(/\*/g, '.*') + '$');
        files = files.concat(walk(dir, function (f) { return rx.test(path.basename(f)); }, 0, 1));
      }
    }
  }
  const all = [];
  for (const f of files) {
    const data = readJson(f, null);
    if (Array.isArray(data)) all.push.apply(all, data);
  }
  all.sort(function (a, b) { return String(a.started_at || '').localeCompare(String(b.started_at || '')); });
  const seen = new Map();
  for (const s of all) {
    // Dedupe by stable session identity, not by first assistant message id:
    // continuation copies share an opening message but carry unique later turns.
    const key = s.session_key || (s.type + ':' + s.session_id) || s.fingerprint;
    if (!seen.has(key)) { seen.set(key, s); continue; }
    const prev = seen.get(key);
    if (num(s.total_tokens) > num(prev.total_tokens)) seen.set(key, s);
  }
  const merged = Array.from(seen.values());
  writeJson(out, merged);
  console.log(JSON.stringify({ ok: true, inputs: files.length, sessions: merged.length, out: out }, null, 2));
}

export { dshSessionFiles };

const isMain = process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href;
if (isMain) {
  if (CMD === 'init') cmdInit();
  else if (CMD === 'scan') cmdScan();
  else if (CMD === 'collect') cmdCollect();
  else if (CMD === 'merge') cmdMerge();
  else {
    console.error('scan-core: unknown command ' + CMD);
    process.exit(1);
  }
}