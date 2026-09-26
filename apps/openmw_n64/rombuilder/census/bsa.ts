/**
 * Morrowind (TES3) BSA archives, read from bytes (no Node APIs).
 *
 * Layout: u32 version (0x100), u32 hash table offset (from byte 12), u32 file
 * count; then per file u32 size + u32 offset (into the data), per file u32
 * name offset (into the names), the names (NUL-terminated), per file an
 * 8-byte hash; then the data.
 */

export type BsaEntry = { name: string; offset: number; size: number };

/** Lower case, forward slashes: the form every lookup here uses. */
export const normalizePath = (p: string) => p.replace(/\\/g, "/").toLowerCase();

export function readBsa(buf: Uint8Array): BsaEntry[] {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  if (dv.getUint32(0, true) !== 0x100) throw new Error("not a Morrowind BSA");
  const hashOffset = dv.getUint32(4, true), count = dv.getUint32(8, true);
  const namesStart = 12 + count * 12, dataStart = 12 + hashOffset + count * 8;
  const out: BsaEntry[] = [];
  for (let i = 0; i < count; i++) {
    const size = dv.getUint32(12 + i * 8, true), offset = dv.getUint32(16 + i * 8, true);
    let p = namesStart + dv.getUint32(12 + count * 8 + i * 4, true), name = "";
    while (buf[p] !== 0) name += String.fromCharCode(buf[p++]!);
    out.push({ name: normalizePath(name), offset: dataStart + offset, size });
  }
  return out;
}
