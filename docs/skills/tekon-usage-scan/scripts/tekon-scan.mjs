#!/usr/bin/env node
/**
 * tekon-usage-scan / report + validate front-end.
 * init/scan/collect/merge are delegated to scan-core.mjs.
 */
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
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
function readJson(p, fallback) {
  try { return JSON.parse(fs.readFileSync(p, 'utf8')); } catch { return fallback; }
}
function writeJson(p, v) { fs.mkdirSync(path.dirname(p), { recursive: true }); fs.writeFileSync(p, JSON.stringify(v, null, 2), 'utf8'); }
function num(v) { const n = Number(v); return Number.isFinite(n) ? n : 0; }
function fmt(n) { return num(n).toLocaleString('en-US'); }
function nowIso() { return new Date().toISOString(); }
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
function mdCell(v) { return String(v === null || v === undefined ? '' : v).replace(/\|/g, '\\|').replace(/\n/g, ' '); }
function table(rows, cols) {
  const head = '| ' + cols.map(function (c) { return c.label; }).join(' | ') + ' |';
  const sep = '|' + cols.map(function () { return '---|'; }).join('');
  const body = rows.map(function (r) {
    return '| ' + cols.map(function (c) { return mdCell(typeof c.value === 'function' ? c.value(r) : r[c.key]); }).join(' | ') + ' |';
  });
  return [head, sep].concat(body).join('\n');
}

if (['init', 'scan', 'collect', 'merge', 'help', '--help'].includes(CMD)) {
  if (CMD === 'help' || CMD === '--help') {
    console.log('tekon-usage-scan 1.1.0');
    console.log('usage: node tekon-scan.mjs <init|scan|collect|merge|report|validate> [options]');
    console.log('  init    --project-dir DIR --out DIR [--keywords a,b] [--official-logs DIR]');
    console.log('  scan    --config tekon.config.json');
    console.log('  collect --type dsh|claude|codex|workbuddy|official_logs --path DIR --out DIR');
    console.log('  merge   --inputs DIR_OR_GLOB --out raw/sessions.json');
    console.log('  report  --config tekon.config.json');
    console.log('  validate --dir OUT');
    process.exit(0);
  }
  const r = spawnSync(process.execPath, [path.join(HERE, 'scan-core.mjs')].concat(process.argv.slice(2)), { stdio: 'inherit' });
  process.exit(r.status === null ? 1 : r.status);
}

function cfgPath() {
  const p = opt('config', 'tekon.config.json');
  const abs = path.resolve(p);
  const cfg = readJson(abs, null);
  if (!cfg) throw new Error('cannot read config: ' + abs);
  return { file: abs, cfg: cfg, out: path.resolve(opt('out', cfg.output && cfg.output.dir ? cfg.output.dir : path.dirname(abs))) };
}

function applyDecisions(sessions, decisions) {
  for (const s of sessions) {
    if (s.type === 'official_logs') { s.decision = 'exclude'; s.included_in_total = false; continue; }
    const d = decisions[s.session_key];
    if (d === 'include' || d === 'exclude') s.decision = d;
    s.included_in_total = s.decision === 'include';
  }
}

function aggregate(sessions, audits, cfg) {
  const map = new Map();
  const included = sessions.filter(function (s) { return s.type !== 'official_logs' && s.included_in_total; });
  for (const s of included) {
    const id = s.agent_id || s.source_id;
    if (!map.has(id)) {
      map.set(id, {
        agent_id: id,
        agent_name: s.agent_name || id,
        platform: s.platform || '',
        type: s.type,
        official_tool: s.official_tool === true,
        official_collectable: s.official === true,
        collection_mode: s.collection_mode || 'local-session-store',
        included_in_total: true,
        sessions: 0, requests: 0,
        prompt_tokens: 0, cached_tokens: 0, input_tokens: 0, cache_write_tokens: 0, output_tokens: 0, total_tokens: 0,
        first_at: null, last_at: null, notes: s.notes || ''
      });
    }
    const a = map.get(id);
    a.sessions += 1;
    a.requests += num(s.requests);
    a.prompt_tokens += num(s.prompt_tokens);
    a.cached_tokens += num(s.cache_read_tokens);
    a.input_tokens += num(s.input_tokens);
    a.cache_write_tokens += num(s.cache_write_tokens);
    a.output_tokens += num(s.output_tokens);
    a.total_tokens += num(s.total_tokens);
    if (s.started_at && (!a.first_at || s.started_at < a.first_at)) a.first_at = s.started_at;
    if (s.last_event_at && (!a.last_at || s.last_event_at > a.last_at)) a.last_at = s.last_event_at;
  }
  for (const m of (cfg.manual_agents || [])) {
    const id = m.agent_id || m.id;
    if (!id) continue;
    map.set(id, {
      agent_id: id,
      agent_name: m.agent_name || id,
      platform: m.platform || 'manual',
      type: m.type || 'manual-ledger',
      official_tool: m.official_tool === true,
      official_collectable: m.official_collectable === true,
      collection_mode: m.collection_mode || 'manual-ledger',
      included_in_total: m.included_in_total !== false,
      sessions: num(m.sessions),
      requests: num(m.requests),
      prompt_tokens: num(m.prompt_tokens),
      cached_tokens: num(m.cached_tokens),
      input_tokens: num(m.input_tokens),
      cache_write_tokens: num(m.cache_write_tokens),
      output_tokens: num(m.output_tokens),
      total_tokens: num(m.total_tokens),
      first_at: m.first_at || null,
      last_at: m.last_at || null,
      notes: m.notes || 'manual ledger; no local transcript'
    });
  }
  const official = [];
  const byTool = new Map();
  for (const r of audits) {
    if (!byTool.has(r.tool)) byTool.set(r.tool, { tool: r.tool, sessions: 0, requests: 0, tin: 0, tout: 0 });
    const t = byTool.get(r.tool);
    t.sessions += 1; t.requests += num(r.assistant_events); t.tin += num(r.tokens_in); t.tout += num(r.tokens_out);
  }
  for (const t of byTool.values()) {
    official.push({
      agent_id: 'official-' + t.tool,
      agent_name: '官方 LOGS / ' + t.tool,
      platform: 'official',
      type: 'official_logs',
      official_tool: true,
      official_collectable: true,
      collection_mode: 'official-logs',
      included_in_total: false,
      sessions: t.sessions,
      requests: t.requests,
      prompt_tokens: t.tin,
      cached_tokens: 0,
      input_tokens: t.tin,
      cache_write_tokens: 0,
      output_tokens: t.tout,
      total_tokens: t.tin + t.tout,
      first_at: null,
      last_at: null,
      notes: 'official evidence only; not added to supplementary total'
    });
  }
  const agents = Array.from(map.values()).concat(official).sort(function (a, b) {
    if (!!a.included_in_total !== !!b.included_in_total) return a.included_in_total ? -1 : 1;
    return num(b.total_tokens) - num(a.total_tokens);
  });
  const totals = { sessions: 0, requests: 0, prompt_tokens: 0, cached_tokens: 0, input_tokens: 0, cache_write_tokens: 0, output_tokens: 0, total_tokens: 0 };
  for (const a of agents) {
    if (!a.included_in_total) continue;
    for (const k of Object.keys(totals)) totals[k] += num(a[k]);
  }
  return { agents: agents, included: included, official: official, totals: totals };
}

function buildReadme(cfg, agg, sessions, audits, generatedAt) {
  const p = cfg.project || {};
  const totals = agg.totals;
  const includedAgents = agg.agents.filter(function (a) { return a.included_in_total; });
  const officialAgents = agg.agents.filter(function (a) { return !a.included_in_total && a.type === 'official_logs'; });
  const review = sessions.filter(function (s) { return s.type !== 'official_logs' && s.decision === 'review'; });
  const zero = sessions.filter(function (s) { return s.included_in_total && num(s.total_tokens) === 0; });
  const dup = sessions.filter(function (s) { return num(s.duplicate_requests) > 0; });
  const badAudit = audits.filter(function (r) { return r.event_count_match === 'no'; });
  const delta = audits.filter(function (r) { return num(r.ts_delta_sec) > 1; });

  const lines = [];
  lines.push('# ' + (p.name || 'project') + ' · TEKON 用量统计');
  lines.push('');
  lines.push('> 生成时间：' + generatedAt + '  ');
  lines.push('> 扫描器：tekon-usage-scan@1.1.0  ');
  lines.push('> 口径：total_tokens = prompt_tokens + output_tokens；prompt_tokens = input_tokens + cache_read_tokens + cache_write_tokens。');
  lines.push('');
  lines.push('## 0. 结论');
  lines.push('');
  lines.push('- 纳入 **' + includedAgents.length + '** 个 agent / **' + fmt(totals.sessions) + '** 个会话 / **' + fmt(totals.requests) + '** 次请求。');
  lines.push('- 去重后总用量 **' + fmt(totals.total_tokens) + ' TEKON**（prompt ' + fmt(totals.prompt_tokens) + ' / cached ' + fmt(totals.cached_tokens) + ' / output ' + fmt(totals.output_tokens) + '）。');
  lines.push('- 官方 LOGS 共 **' + audits.length + '** 个文件、**' + fmt(audits.reduce(function (x, r) { return x + num(r.assistant_events); }, 0)) + '** 条 assistant 事件；只作证据，不计入上面的补充总量。');
  lines.push('- 仍需人工判断的会话 **' + review.length + '** 个；candidates.csv 里 review 行默认不计入。');
  lines.push('');
  lines.push('## 1. 各端明细（计入总量的）');
  lines.push('');
  lines.push(table(includedAgents, [
    { label: 'Agent', key: 'agent_name' },
    { label: '平台', key: 'platform' },
    { label: '会话', value: function (r) { return fmt(r.sessions); } },
    { label: '请求', value: function (r) { return fmt(r.requests); } },
    { label: 'prompt', value: function (r) { return fmt(r.prompt_tokens); } },
    { label: 'cached', value: function (r) { return fmt(r.cached_tokens); } },
    { label: 'output', value: function (r) { return fmt(r.output_tokens); } },
    { label: 'total', value: function (r) { return fmt(r.total_tokens); } },
    { label: '官方可采集', value: function (r) { return r.official_collectable ? 'yes' : 'no'; } }
  ]));
  lines.push('');
  lines.push('**TOTAL：' + fmt(totals.total_tokens) + ' tokens / ' + fmt(totals.requests) + ' requests / ' + fmt(totals.sessions) + ' sessions**');
  lines.push('');
  lines.push('## 2. 官方 LOGS 审计');
  lines.push('');
  if (officialAgents.length) {
    lines.push(table(officialAgents, [
      { label: '工具', key: 'agent_name' },
      { label: '文件', value: function (r) { return fmt(r.sessions); } },
      { label: 'assistant 事件', value: function (r) { return fmt(r.requests); } },
      { label: 'tokens_in', value: function (r) { return fmt(r.prompt_tokens); } },
      { label: 'tokens_out', value: function (r) { return fmt(r.output_tokens); } },
      { label: '计入总量', value: function () { return 'no'; } }
    ]));
    lines.push('');
  }
  lines.push('- 官方自动采集只支持 Claude Code（含 AIoT-IDE）、AIoT-IDE、OpenCode、Codex；DSH / WorkBuddy / K3DSH / SUPERAGENT 为 official=false、collection_mode=local-session-store。');
  lines.push('- official_logs_audit.csv 逐文件记录 event_count 是否与 manifest 一致；不一致不会改官方文件，只在异常清单里标出。');
  lines.push('');
  lines.push('## 3. 口径与去重');
  lines.push('');
  lines.push('- DSH 会话是多帧 zstd，按 message.id 全局去重；VM 崩溃恢复产生的副本按 message.id 归属最早会话，后续副本只计其新增消息。');
  lines.push('- WorkBuddy 按 providerData.messageId 去重；同一请求的 rawUsage 会挂在多条 function_call 上。');
  lines.push('- 平台账单（MiMo xlsx / DSH 平台 CSV）与本地 transcript 是同一批 token 的两个视图，默认不相加；需要账单口径时写入 manual_agents 并注明。');
  lines.push('- cached_tokens 是 prompt_tokens 的子集，不能再加一次。');
  lines.push('');
  lines.push('## 4. 异常清单');
  lines.push('');
  lines.push('- event_count 不一致：' + (badAudit.length ? badAudit.map(function (r) { return r.session_id; }).slice(0, 10).join(', ') : '无') + '。');
  lines.push('- 官方 manifest 与文件首末事件时间差 > 1s：' + delta.length + ' 个；最大 ' + (delta.length ? String(Math.max.apply(null, delta.map(function (r) { return num(r.ts_delta_sec); }))) + 's' : '0') + '。');
  lines.push('- 计入总量但 token 为 0 的会话：' + zero.length + ' 个（可能是空会话/只有标题请求）。');
  lines.push('- 发生过去重的会话：' + dup.length + ' 个；sessions.csv 的 duplicate_requests 列给出被丢弃的重复 assistant 条数。');
  lines.push('- review 会话：' + review.length + ' 个，未计入；请在 decisions.json 里显式写 include / exclude。');
  lines.push('');
  lines.push('## 5. 文件清单');
  lines.push('');
  lines.push('| 文件 | 用途 |');
  lines.push('|---|---|');
  lines.push('| summary.csv | 各 agent 汇总（含未计入的官方证据行） |');
  lines.push('| sessions.csv | 逐会话明细与 included_in_total |');
  lines.push('| manifest.json | 对齐 logs/ 的机器可读清单 |');
  lines.push('| official_logs_audit.csv | 官方 LOGS 逐文件审计 |');
  lines.push('| platform_ledger.csv | 手工/平台台账（若有） |');
  lines.push('| report_data.json | 校验用汇总 |');
  lines.push('| TEKON_usage_report.xlsx | 可选，python scripts/build_xlsx.py --dir . |');
  lines.push('');
  lines.push('## 6. 复现');
  lines.push('');
  lines.push('    node scripts/tekon-scan.mjs scan --config tekon.config.json');
  lines.push('    node scripts/tekon-scan.mjs report --config tekon.config.json');
  lines.push('    node scripts/tekon-scan.mjs validate --dir .');
  lines.push('');
  lines.push('> 本报告是补充台账，不是官方工时认定。官方只采集 Claude Code / AIoT-IDE / OpenCode / Codex；其他 agent 的用量用于内部核算与答辩说明。');
  lines.push('');
  return lines.join('\n');
}

function cmdReport() {
  const c = cfgPath();
  const cfg = c.cfg;
  const out = c.out;
  const sessions = readJson(path.join(out, 'raw', 'sessions.json'), null);
  if (!Array.isArray(sessions)) throw new Error('missing raw/sessions.json; run scan first');
  const audits = readJson(path.join(out, 'raw', 'official_audit.json'), []);
  const decisions = readJson(path.join(out, 'decisions.json'), {});
  applyDecisions(sessions, decisions);
  const generatedAt = nowIso();
  const agg = aggregate(sessions, audits, cfg);

  writeCsv(path.join(out, 'summary.csv'), agg.agents, [
    'agent_id', 'agent_name', 'platform', 'type', 'official_tool', 'official_collectable', 'collection_mode', 'included_in_total',
    'sessions', 'requests', 'prompt_tokens', 'cached_tokens', 'input_tokens', 'cache_write_tokens', 'output_tokens', 'total_tokens',
    'first_at', 'last_at', 'notes'
  ], true);

  const sRows = sessions.map(function (s) {
    return {
      session_key: s.session_key,
      agent_id: s.agent_id,
      agent_name: s.agent_name,
      type: s.type,
      platform: s.platform,
      official_tool: s.official_tool === true,
      official_collectable: s.official === true,
      collection_mode: s.collection_mode,
      decision: s.decision,
      included_in_total: s.included_in_total ? 'yes' : 'no',
      session_id: s.session_id,
      parent_session_id: s.parent_session_id || '',
      title: s.title,
      cwd: s.cwd,
      started_at: s.started_at,
      last_event_at: s.last_event_at,
      requests: s.requests,
      duplicate_requests: s.duplicate_requests || 0,
      prompt_tokens: s.prompt_tokens,
      cached_tokens: s.cache_read_tokens,
      input_tokens: s.input_tokens,
      cache_write_tokens: s.cache_write_tokens,
      output_tokens: s.output_tokens,
      total_tokens: s.total_tokens,
      models: Object.keys(s.models || {}).join('; '),
      score: s.score,
      suggested: s.suggested,
      hits: Object.keys(s.hits || {}).join('; '),
      first_user: s.first_user,
      source_file: s.source_file,
      notes: s.notes
    };
  });
  writeCsv(path.join(out, 'sessions.csv'), sRows, [
    'session_key', 'agent_id', 'agent_name', 'type', 'platform', 'official_tool', 'official_collectable', 'collection_mode',
    'decision', 'included_in_total', 'session_id', 'parent_session_id', 'title', 'cwd', 'started_at', 'last_event_at',
    'requests', 'duplicate_requests', 'prompt_tokens', 'cached_tokens', 'input_tokens', 'cache_write_tokens', 'output_tokens',
    'total_tokens', 'models', 'score', 'suggested', 'hits', 'first_user', 'source_file', 'notes'
  ], true);

  if (audits.length) {
    writeCsv(path.join(out, 'official_logs_audit.csv'), audits, [
      'file', 'tool', 'session_id', 'events', 'assistant_events', 'tokens_in', 'tokens_out', 'models',
      'first_ts', 'last_ts', 'manifest_started_at', 'manifest_last_event_at', 'ts_delta_sec',
      'manifest_event_count', 'event_count_match', 'health', 'collection_mode', 'file_path'
    ], true);
  }
  const manual = (cfg.manual_agents || []).map(function (m) {
    return {
      agent_id: m.agent_id || m.id, agent_name: m.agent_name || m.id, platform: m.platform || 'manual',
      sessions: num(m.sessions), requests: num(m.requests), prompt_tokens: num(m.prompt_tokens),
      cached_tokens: num(m.cached_tokens), output_tokens: num(m.output_tokens), total_tokens: num(m.total_tokens),
      notes: m.notes || ''
    };
  });
  writeCsv(path.join(out, 'platform_ledger.csv'), manual, ['agent_id', 'agent_name', 'platform', 'sessions', 'requests', 'prompt_tokens', 'cached_tokens', 'output_tokens', 'total_tokens', 'notes'], true);

  const included = sessions.filter(function (s) { return s.included_in_total; });
  const manifest = {
    schema_version: '1.0',
    generated_at: generatedAt,
    generator: 'tekon-usage-scan@1.1.0',
    project: {
      name: (cfg.project && cfg.project.name) || '',
      repo: (cfg.project && cfg.project.repo) || '',
      roots: (cfg.project && cfg.project.roots) || [],
      since: (cfg.project && cfg.project.since) || null,
      until: (cfg.project && cfg.project.until) || null,
      timezone: (cfg.project && cfg.project.timezone) || 'UTC'
    },
    accounting: {
      total: 'prompt_tokens + output_tokens',
      prompt: 'input_tokens + cache_read_tokens + cache_write_tokens',
      cached_subset_of_prompt: true,
      official_tools: ['claude-code', 'aiot-ide', 'opencode', 'codex'],
      supplementary_agents_are_official: false,
      dedupe: 'DSH: message.id; WorkBuddy: providerData.messageId; official: manifest file'
    },
    totals: agg.totals,
    agents: agg.agents,
    sessions: included.map(function (s) {
      return {
        session_key: s.session_key, agent_id: s.agent_id, session_id: s.session_id,
        parent_session_id: s.parent_session_id || null, title: s.title, cwd: s.cwd,
        started_at: s.started_at, last_event_at: s.last_event_at, requests: s.requests,
        prompt_tokens: s.prompt_tokens, cached_tokens: s.cache_read_tokens,
        output_tokens: s.output_tokens, total_tokens: s.total_tokens,
        models: s.models, official: s.official === true, collection_mode: s.collection_mode
      };
    }),
    review: sessions.filter(function (s) { return s.type !== 'official_logs' && s.decision === 'review'; }).map(function (s) {
      return { session_key: s.session_key, agent_id: s.agent_id, title: s.title, total_tokens: s.total_tokens, score: s.score };
    }),
    evidence: {
      official_logs_files: audits.length,
      official_logs_events: audits.reduce(function (x, r) { return x + num(r.events); }, 0),
      official_logs_assistant_events: audits.reduce(function (x, r) { return x + num(r.assistant_events); }, 0),
      official_event_count_mismatch: audits.filter(function (r) { return r.event_count_match === 'no'; }).length
    },
    unallocated: manual
  };
  writeJson(path.join(out, 'manifest.json'), manifest);
  const reportData = {
    generated_at: generatedAt,
    totals: agg.totals,
    summary: agg.agents,
    sessions: sessions.length,
    included_sessions: included.length,
    audit_rows: audits.length,
    manifest_totals: manifest.totals
  };
  writeJson(path.join(out, 'report_data.json'), reportData);
  fs.writeFileSync(path.join(out, 'README.md'), buildReadme(cfg, agg, sessions, audits, generatedAt), 'utf8');
  console.log(JSON.stringify({ ok: true, out: out, totals: agg.totals, agents: agg.agents.length, sessions: sessions.length, audits: audits.length }, null, 2));
}

function cmdValidate() {
  const dirArg = opt('dir', '');
  const out = dirArg ? path.resolve(dirArg) : cfgPath().out;
  const required = ['summary.csv', 'sessions.csv', 'manifest.json', 'report_data.json', 'README.md'];
  const missing = required.filter(function (f) { return !fs.existsSync(path.join(out, f)); });
  const data = readJson(path.join(out, 'report_data.json'), null);
  const manifest = readJson(path.join(out, 'manifest.json'), null);
  if (!data || !manifest) {
    console.log(JSON.stringify({ ok: false, missing: missing, error: 'report_data.json or manifest.json missing' }, null, 2));
    process.exit(1);
  }
  const checks = [];
  const sumAgents = (manifest.agents || []).filter(function (a) { return a.included_in_total; }).reduce(function (x, a) { return x + num(a.total_tokens); }, 0);
  checks.push({ name: 'manifest totals == sum(included agents)', ok: sumAgents === num(manifest.totals.total_tokens), expected: sumAgents, actual: num(manifest.totals.total_tokens) });
  checks.push({ name: 'report_data totals == manifest totals', ok: num(data.totals.total_tokens) === num(manifest.totals.total_tokens), expected: num(data.totals.total_tokens), actual: num(manifest.totals.total_tokens) });
  checks.push({ name: 'included sessions == manifest.sessions', ok: num(data.included_sessions) === (manifest.sessions || []).length, expected: num(data.included_sessions), actual: (manifest.sessions || []).length });
  const bad = checks.filter(function (x) { return !x.ok; });
  const res = { ok: missing.length === 0 && bad.length === 0, missing: missing, checks: checks, totals: manifest.totals };
  console.log(JSON.stringify(res, null, 2));
  if (!res.ok) process.exit(1);
}

if (CMD === 'report') cmdReport();
else if (CMD === 'validate') cmdValidate();
else {
  console.error('unknown command: ' + CMD);
  process.exit(1);
}