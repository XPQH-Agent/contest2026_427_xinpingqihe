import fs from 'node:fs';
import zlib from 'node:zlib';

const MAGIC = Buffer.from([0x28, 0xb5, 0x2f, 0xfd]);

/** Decode a multi-frame zstd file (DSH writes one frame per event). */
export function decodeZstdFrames(file) {
  if (typeof zlib.zstdDecompressSync !== 'function') {
    throw new Error('node:zlib.zstdDecompressSync is unavailable; use Node >= 22.15');
  }
  const buf = fs.readFileSync(file);
  const starts = [];
  for (let i = 0; ; ) {
    const k = buf.indexOf(MAGIC, i);
    if (k < 0) break;
    starts.push(k);
    i = k + 4;
  }
  if (!starts.length) throw new Error('not a zstd frame: ' + file);
  const chunks = [];
  for (let i = 0; i < starts.length; i++) {
    const end = i + 1 < starts.length ? starts[i + 1] : buf.length;
    let done = false;
    // zstd frames may have up to a few trailing bytes; trim and retry.
    for (let trim = 0; trim <= 3 && !done; trim++) {
      try {
        const out = zlib.zstdDecompressSync(buf.subarray(starts[i], end - trim));
        chunks.push(out);
        done = true;
      } catch { /* try next trim */ }
    }
    if (!done) throw new Error('cannot decompress frame ' + i + ' of ' + file);
  }
  return Buffer.concat(chunks);
}

export function readJsonlFile(file) {
  const text = fs.readFileSync(file, 'utf8');
  return parseJsonl(text);
}

export function parseJsonl(text) {
  const out = [];
  for (const line of text.split(/\r?\n/)) {
    if (!line.trim()) continue;
    try { out.push(JSON.parse(line)); } catch { /* skip malformed line */ }
  }
  return out;
}

export function readDshEvents(file) {
  const name = file.toLowerCase();
  const buf = name.endsWith('.zstd') || name.endsWith('.zst')
    ? decodeZstdFrames(file)
    : fs.readFileSync(file);
  return parseJsonl(buf.toString('utf8'));
}

export function walk(dir, predicate, depth = 0, maxDepth = 8) {
  if (depth > maxDepth) return [];
  let entries;
  try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { return []; }
  const out = [];
  for (const e of entries) {
    if (e.name === 'node_modules' || e.name === '.git') continue;
    const p = dir + (dir.endsWith('/') || dir.endsWith('\\') ? '' : '/') + e.name;
    if (e.isDirectory()) out.push(...walk(p, predicate, depth + 1, maxDepth));
    else if (predicate(p, e.name)) out.push(p);
  }
  return out;
}
