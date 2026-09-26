#!/usr/bin/env npx tsx
/**
 * Census: how big Morrowind would be on a 64 MB cartridge (PLAN_ALL_ON_N64.md
 * §4.7), measured on the player's own files.
 *
 *   npx tsx census.ts "/path/to/Data Files" [--json census.json]
 *
 * Reads Morrowind.esm, Morrowind.bsa and loose files (Tribunal and Bloodmoon
 * are left out), converts or measures each category the way the ROM builder
 * would, and prints the budget for three quality tiers:
 *
 *   records, dialogue, books, scripts, cells   the ESM's records, compressed in
 *                                              16 KB chunks (deflate as a stand-in
 *                                              for the N64 decompressor)
 *   terrain       LAND converted: heights kept, normals dropped (rebuilt on
 *                 the console), colours and texture indices kept; per cell
 *   textures      decoded, capped at the tier's size and encoded as C64T
 *   meshes, animation, collision   counted by ../nifstat (OpenMW's NIF reader)
 *                 and priced with a fixed per-vertex / per-key model
 *   voice, music, sound effects    durations from the files, priced at the
 *                 tier's Opus bitrate
 *   code          from the linked-size measurement (PLAN §3), not from files
 *
 * Every model constant is at the top of this file. Options:
 *   --esm FILE (repeatable)  default Morrowind.esm, else every .esm/.omwgame/.omwaddon
 *   --bsa FILE (repeatable)  default Morrowind.bsa, else every .bsa
 *   --nifstat PATH           default ../nifstat/nifstat (build it with make)
 *   --chroma64 DIR           CHROMA64 checkout (C64T encoder), default ../../../../../chroma64
 *   --texture-sample N       encode every Nth texture and scale up (faster)
 *   --json FILE              everything as JSON, largest items included
 */
import { execFileSync } from "node:child_process";
import { existsSync, readdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { basename, dirname, extname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { deflateRawSync } from "node:zlib";
import { mp3Info, wavInfo } from "./audio.ts";
import { normalizePath, readBsa } from "./bsa.ts";
import { cString, readEsm } from "./esm.ts";

// ─────────────────────────── model constants ───────────────────────────

const MB = 1024 * 1024;
const CART_LIMIT = 0x03ff0000 / MB; // 63.94 MB, what a 64 MB cartridge leaves after the header

type Tier = { name: string; texCap: number; texStep: number; voiceKbps: number; musicKbps: number; sfxKbps: number; landColourHalf: boolean };
const TIERS: Tier[] = [
  { name: "A", texCap: 256, texStep: 8, voiceKbps: 16, musicKbps: 48, sfxKbps: 32, landColourHalf: false },
  { name: "B", texCap: 128, texStep: 10, voiceKbps: 12, musicKbps: 32, sfxKbps: 24, landColourHalf: false },
  { name: "C", texCap: 64, texStep: 14, voiceKbps: 8, musicKbps: 24, sfxKbps: 16, landColourHalf: true },
];

/** OpenMW code without Lua and the old renderer (PLAN §3: about 11 MB), compressed. Not measured from files. */
const CODE_MB = 11 * 0.55;
/** OpenMW's own resources the game needs (fonts, GUI layouts): an allowance. */
const RESOURCES_MB = 0.5;
/** Converted mesh: 16-byte vertex (Tiny3D: int16 position, packed normal, RGBA, int16 UV), 6 bytes per triangle, 32 per shape. */
const VERTEX_BYTES = 16, TRI_BYTES = 6, SHAPE_BYTES = 32;
/** Animation: bytes per rotation / translation key (with its time), per scale or float key, per skin weight, per morph vertex. */
const ROT_KEY = 8, TRANS_KEY = 8, SMALL_KEY = 4, SKIN_WEIGHT = 3, MORPH_VERT = 6;
/** Collision if stored instead of rebuilt: int16 position, 16-bit indices. */
const COLL_VERT = 6, COLL_TRI = 6;
/** What compressing the converted meshes and animation saves (assumed, not measured yet). */
const MESH_PACK = 0.7;
/** Records are read on demand, so they are compressed in independent chunks of this size. */
const CHUNK = 16384;

// ─────────────────────────── arguments ───────────────────────────

const argv = process.argv.slice(2);
const opt = (name: string) => { const i = argv.indexOf(name); return i >= 0 ? argv[i + 1] : undefined; };
const opts = (name: string) => argv.flatMap((a, i) => (a === name && argv[i + 1] ? [argv[i + 1]!] : []));
const dataDir = argv.find((a, i) => !a.startsWith("--") && (i === 0 || !argv[i - 1]!.startsWith("--")));
if (!dataDir || !statSync(dataDir).isDirectory()) {
  console.error('usage: npx tsx census.ts "/path/to/Data Files" [--json out.json] [--texture-sample N] ...');
  process.exit(1);
}
const here = dirname(fileURLToPath(import.meta.url));
const chroma64 = resolve(opt("--chroma64") ?? join(here, "../../../../../chroma64"));
const nifstat = resolve(opt("--nifstat") ?? join(here, "../nifstat/nifstat"));
const sample = Math.max(1, Number(opt("--texture-sample") ?? 1));

const top = readdirSync(dataDir);
const findTop = (name: string) => top.find((f) => f.toLowerCase() === name.toLowerCase());
const esmFiles = opts("--esm").length ? opts("--esm") : findTop("Morrowind.esm") ? [findTop("Morrowind.esm")!] : top.filter((f) => /\.(esm|omwgame|omwaddon)$/i.test(f));
const bsaFiles = opts("--bsa").length ? opts("--bsa") : findTop("Morrowind.bsa") ? [findTop("Morrowind.bsa")!] : top.filter((f) => /\.bsa$/i.test(f));
const log = (s: string) => process.stderr.write(s + "\n");

// ─────────────────────────── files: archives, then loose files over them ───────────────────────────

type VFile = { name: string; src: string; read: () => Uint8Array; size: number };
const vfs = new Map<string, VFile>();
for (const b of bsaFiles) {
  const buf = new Uint8Array(readFileSync(join(dataDir, b)));
  for (const e of readBsa(buf)) vfs.set(e.name, { name: e.name, src: b, size: e.size, read: () => buf.subarray(e.offset, e.offset + e.size) });
}
const walk = (dir: string): string[] => readdirSync(dir).flatMap((f) => { const p = join(dir, f); return statSync(p).isDirectory() ? walk(p) : [p]; });
for (const p of walk(dataDir)) {
  const name = normalizePath(relative(dataDir, p));
  if (!name.includes("/")) continue; // top-level files are the ESMs and archives
  vfs.set(name, { name, src: "loose", size: statSync(p).size, read: () => new Uint8Array(readFileSync(p)) });
}
log(`${vfs.size} files (${bsaFiles.join(", ") || "no archive"} + loose), ESM: ${esmFiles.join(", ")}`);

// ─────────────────────────── records ───────────────────────────

/** Compressed size of `parts` packed back to back and cut into independent chunks. */
function chunked(parts: Uint8Array[]): number {
  let total = 0, cur: Uint8Array[] = [], n = 0;
  const flush = () => { if (n) total += deflateRawSync(Buffer.concat(cur), { level: 9 }).length; cur = []; n = 0; };
  for (const p of parts) {
    cur.push(p);
    n += p.length;
    if (n >= CHUNK) flush();
  }
  flush();
  return total;
}

const CATEGORY: Record<string, string> = { DIAL: "dialogue", INFO: "dialogue", BOOK: "books", SCPT: "scripts", CELL: "cells", LAND: "terrain" };
const recParts = new Map<string, Uint8Array[]>();
const recRaw = new Map<string, number>();
const recCount = new Map<string, number>();
const landRaw: Uint8Array[][] = [];
const referenced = new Set<string>(); // texture base names the records use
const texRef = (s: string) => { if (s) referenced.add(basename(normalizePath(s)).replace(/\.[^.]+$/, "")); };
for (const f of esmFiles) {
  for (const r of readEsm(new Uint8Array(readFileSync(join(dataDir, f))))) {
    recCount.set(r.type, (recCount.get(r.type) ?? 0) + 1);
    const cat = CATEGORY[r.type] ?? "records";
    recRaw.set(cat, (recRaw.get(cat) ?? 0) + r.raw.length);
    if (r.type === "LAND") {
      landRaw.push(r.subs.filter((s) => s.type !== "VNML").map((s) => s.data));
      continue;
    }
    if (!recParts.has(cat)) recParts.set(cat, []);
    recParts.get(cat)!.push(r.raw);
    for (const s of r.subs) {
      if (s.type === "ICON" || s.type === "ITEX" || s.type === "PTEX" || (r.type === "LTEX" && s.type === "DATA") || (r.type === "BSGN" && s.type === "TNAM")) texRef(cString(s.data));
      if (r.type === "BOOK" && s.type === "TEXT") for (const m of cString(s.data).matchAll(/SRC="([^"]+)"/gi)) texRef(m[1]!);
    }
  }
}
const records: Record<string, number> = {};
for (const [cat, parts] of recParts) records[cat] = chunked(parts);
log(`records: ${[...recCount.values()].reduce((a, b) => a + b, 0)} (${recCount.get("LAND") ?? 0} LAND)`);

/** Terrain for one tier: each cell's converted LAND compressed on its own (a cell loads alone). */
function terrain(t: Tier): number {
  let total = 0;
  for (const subs of landRaw) {
    const parts = subs.map((d) => {
      if (!t.landColourHalf || d.length !== 65 * 65 * 3) return d;
      const half = new Uint8Array(33 * 33 * 3); // VCLR at half resolution
      for (let y = 0; y < 33; y++) for (let x = 0; x < 33; x++) for (let c = 0; c < 3; c++) half[(y * 33 + x) * 3 + c] = d[(y * 2 * 65 + x * 2) * 3 + c]!;
      return half;
    });
    total += deflateRawSync(Buffer.concat(parts), { level: 9 }).length;
  }
  return total;
}

// ─────────────────────────── meshes ───────────────────────────

type NifStat = { name: string; src: string; bytes: number; ok: boolean; error?: string; shapes: number; verts: number; tris: number; collVerts: number; collTris: number; rotKeys: number; transKeys: number; scaleKeys: number; floatKeys: number; morphVerts: number; skinWeights: number; textures: string[] };
const nifs = new Map<string, NifStat>();
let nifNote = "";
if (existsSync(nifstat)) {
  const args = [...bsaFiles.map((b) => join(dataDir, b)), dataDir];
  const out = execFileSync(nifstat, args, { maxBuffer: 1 << 30 }).toString();
  for (const line of out.split("\n")) {
    if (!line.trim()) continue;
    const s = JSON.parse(line) as NifStat;
    if (!nifs.has(s.name) || s.src === "loose") nifs.set(s.name, s); // loose files override the archive
  }
  for (const s of nifs.values()) for (const t of s.textures ?? []) texRef(t);
} else nifNote = `nifstat not found at ${nifstat} (make -C rombuilder/nifstat): meshes not counted`;
const nifList = [...nifs.values()];
const nifFailed = nifList.filter((s) => !s.ok);
const sum = (k: keyof NifStat) => nifList.reduce((n, s) => n + (s.ok ? Number(s[k]) : 0), 0);
const meshBytes = (sum("verts") * VERTEX_BYTES + sum("tris") * TRI_BYTES + sum("shapes") * SHAPE_BYTES) * MESH_PACK;
const animBytes = (sum("rotKeys") * ROT_KEY + sum("transKeys") * TRANS_KEY + (sum("scaleKeys") + sum("floatKeys")) * SMALL_KEY + sum("skinWeights") * SKIN_WEIGHT + sum("morphVerts") * MORPH_VERT) * MESH_PACK;
const collBytes = (sum("collVerts") * COLL_VERT + sum("collTris") * COLL_TRI) * MESH_PACK;

// ─────────────────────────── textures ───────────────────────────

const { encodeC64T } = await import(join(chroma64, "src/lib/chroma/c64t.ts"));
const { decodeDds, decodeTga } = await import(join(chroma64, "src/lib/chroma/dds.ts"));
const { resizeArea } = await import(join(chroma64, "src/lib/chroma/buffer.ts"));
type Img = { width: number; height: number; data: Uint8ClampedArray };

const texGroup = (name: string) =>
  name.startsWith("icons/") ? "icons" : name.startsWith("bookart/") ? "bookart" : name.startsWith("splash/") ? "splash" : name.startsWith("textures/") ? "textures" : null;
const texFiles = [...vfs.values()].filter((f) => /\.(dds|tga|bmp)$/.test(f.name) && texGroup(f.name));
const pow2Below = (n: number) => { let p = 1; while (p * 2 <= n) p *= 2; return p; };

type TexResult = { name: string; group: string; w: number; h: number; smoothAlpha: boolean; bytes: number[] };
const texResults: TexResult[] = [];
const texFailed: string[] = [];
let done = 0;
const t0 = Date.now();
for (const [i, f] of texFiles.entries()) {
  if (i % sample) continue;
  const group = texGroup(f.name)!;
  let img: Img;
  try {
    const b = f.read();
    img = f.name.endsWith(".dds") ? decodeDds(b) : f.name.endsWith(".tga") ? decodeTga(b) : (() => { throw new Error("BMP not supported yet"); })();
  } catch (e) {
    texFailed.push(`${f.name}: ${(e as Error).message}`);
    continue;
  }
  let partial = 0;
  for (let p = 3; p < img.data.length; p += 4) if (img.data[p]! > 16 && img.data[p]! < 240) partial++;
  const smoothAlpha = partial > (img.width * img.height) / 20;
  const bytes = TIERS.map((t) => {
    // Icons are small already; book art keeps 256; everything else follows the tier.
    const cap = group === "icons" ? 64 : group === "bookart" || group === "splash" ? 256 : t.texCap;
    const w = Math.min(pow2Below(Math.max(1, img.width)), cap), h = Math.min(pow2Below(Math.max(1, img.height)), cap);
    try {
      const src = w === img.width && h === img.height ? img : resizeArea(img, w, h);
      return (encodeC64T(src, { step: t.texStep }) as Uint8Array).length;
    } catch {
      return w * h * 2; // too small or odd for C64T: plain RGBA16
    }
  });
  texResults.push({ name: f.name, group, w: img.width, h: img.height, smoothAlpha, bytes });
  if (++done % 100 === 0) log(`textures: ${done} / ${Math.ceil(texFiles.length / sample)} (${((Date.now() - t0) / 1000).toFixed(0)} s)`);
}
const texTotal = (group: string, tier: number) => (texResults.filter((r) => r.group === group).reduce((n, r) => n + r.bytes[tier]!, 0) * sample);
const texRefShare = (tier: number) => {
  const all = texResults.filter((r) => r.group === "textures");
  const used = all.filter((r) => referenced.has(basename(r.name).replace(/\.[^.]+$/, "")));
  const tot = all.reduce((n, r) => n + r.bytes[tier]!, 0);
  return { count: used.length, of: all.length, share: tot ? used.reduce((n, r) => n + r.bytes[tier]!, 0) / tot : 0 };
};

// ─────────────────────────── audio ───────────────────────────

const audio = { voice: 0, music: 0, sfx: 0 };
const audioFailed: string[] = [];
for (const f of vfs.values()) {
  if (!/\.(wav|mp3)$/.test(f.name) || !(f.name.startsWith("sound/") || f.name.startsWith("music/"))) continue;
  const info = f.name.endsWith(".mp3") ? mp3Info(f.read()) : wavInfo(f.read());
  if (!info) { audioFailed.push(f.name); continue; }
  const kind = f.name.startsWith("music/") ? "music" : f.name.startsWith("sound/vo/") ? "voice" : "sfx";
  audio[kind] += info.seconds;
}

// ─────────────────────────── the budget ───────────────────────────

type Row = { what: string; mb: number[]; how: string };
const rows: Row[] = [];
const all = (mb: number) => TIERS.map(() => mb);
rows.push({ what: "OpenMW code", mb: all(CODE_MB), how: "PLAN §3 (11 MB), compressed; not from your files" });
rows.push({ what: "OpenMW resources", mb: all(RESOURCES_MB), how: "fonts and GUI layouts; an allowance" });
for (const [cat, label] of [["records", "Records"], ["dialogue", "Dialogue"], ["books", "Books"], ["scripts", "Scripts"], ["cells", "Cells and references"]] as const)
  rows.push({ what: label, mb: all((records[cat] ?? 0) / MB), how: `${((recRaw.get(cat) ?? 0) / MB).toFixed(1)} MB in the ESM, chunk-compressed` });
rows.push({ what: "Terrain", mb: TIERS.map((t) => terrain(t) / MB), how: `${landRaw.length} cells, ${((recRaw.get("terrain") ?? 0) / MB).toFixed(1)} MB in the ESM; normals rebuilt on the console` });
for (const [group, label] of [["textures", "Textures"], ["icons", "Icons"], ["bookart", "Book art"]] as const)
  rows.push({ what: label, mb: TIERS.map((_, i) => texTotal(group, i) / MB), how: `${texResults.filter((r) => r.group === group).length * sample} files as C64T${group === "textures" ? `, capped at ${TIERS.map((t) => t.texCap).join(" / ")}` : ""}` });
rows.push({ what: "Meshes", mb: all(meshBytes / MB), how: nifNote || `${nifList.length} NIFs: ${sum("verts")} vertices, ${sum("tris")} triangles` });
rows.push({ what: "Animation", mb: all(animBytes / MB), how: `${sum("rotKeys") + sum("transKeys") + sum("scaleKeys") + sum("floatKeys")} keys, ${sum("skinWeights")} skin weights` });
rows.push({ what: "Voice", mb: TIERS.map((t) => (audio.voice * t.voiceKbps * 125) / MB), how: `${(audio.voice / 3600).toFixed(2)} h at ${TIERS.map((t) => t.voiceKbps).join(" / ")} kbps` });
rows.push({ what: "Music", mb: TIERS.map((t) => (audio.music * t.musicKbps * 125) / MB), how: `${(audio.music / 60).toFixed(1)} min at ${TIERS.map((t) => t.musicKbps).join(" / ")} kbps` });
rows.push({ what: "Sound effects", mb: TIERS.map((t) => (audio.sfx * t.sfxKbps * 125) / MB), how: `${(audio.sfx / 60).toFixed(1)} min at ${TIERS.map((t) => t.sfxKbps).join(" / ")} kbps` });
const totals = TIERS.map((_, i) => rows.reduce((n, r) => n + r.mb[i]!, 0));

const pad = (s: string, n: number) => (s.length >= n ? s : s + " ".repeat(n - s.length));
const num = (v: number) => v.toFixed(2).padStart(9);
console.log(`\nMorrowind on a 64 MB cartridge: census of ${resolve(dataDir)}\n`);
console.log(pad("MB, by tier", 22) + TIERS.map((t) => t.name.padStart(9)).join("") + "   how (tiers: A best, C smallest)");
for (const r of rows) console.log(pad(r.what, 22) + r.mb.map(num).join("") + "   " + r.how);
console.log(pad("Total", 22) + totals.map(num).join("") + `   limit ${CART_LIMIT.toFixed(2)}`);
console.log(pad("Fits?", 22) + totals.map((t) => (t <= CART_LIMIT ? "yes" : "no").padStart(9)).join(""));
console.log(`\nNot included: collision (rebuilt on the console; ${(collBytes / MB).toFixed(2)} MB if stored), splash screens (${(texTotal("splash", 0) / MB).toFixed(2)} MB), video.`);
const ref = texRefShare(1);
console.log(`Textures named by meshes or records: ${ref.count} of ${ref.of}, ${(ref.share * 100).toFixed(0)}% of the texture bytes (UI and sky textures are used by name from code, so the rest is not all unused).`);
const smooth = texResults.filter((r) => r.smoothAlpha && r.group === "textures").length * sample;
if (smooth) console.log(`${smooth} textures have smooth (not cut-out) alpha, which C64T does not store yet; counted as opaque.`);
if (sample > 1) console.log(`Textures: every ${sample}th file encoded, totals scaled up.`);
for (const [label, list] of [["NIFs that failed to load", nifFailed.map((s) => `${s.name}: ${s.error}`)], ["textures that failed", texFailed], ["audio not understood", audioFailed]] as const)
  if (list.length) console.log(`${list.length} ${label}, e.g. ${list.slice(0, 3).join("; ")}`);

const jsonOut = opt("--json");
if (jsonOut) {
  const biggest = <T,>(xs: T[], k: (x: T) => number) => [...xs].sort((a, b) => k(b) - k(a)).slice(0, 25);
  writeFileSync(jsonOut, JSON.stringify({
    dataDir: resolve(dataDir), esm: esmFiles, bsa: bsaFiles, tiers: TIERS, limitMB: CART_LIMIT, rows, totals,
    recordCounts: Object.fromEntries(recCount), audioSeconds: audio,
    biggestTextures: biggest(texResults, (r) => r.bytes[1]!), biggestMeshes: biggest(nifList.filter((s) => s.ok), (s) => s.verts),
    failed: { nif: nifFailed.map((s) => s.name), textures: texFailed, audio: audioFailed },
  }, null, 1));
  log(`wrote ${jsonOut}`);
}
