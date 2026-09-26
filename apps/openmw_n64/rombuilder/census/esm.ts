/**
 * TES3 (Morrowind.esm) records, scanned from bytes (no Node APIs).
 *
 * A record is: char[4] type, u32 size, u32 (unused), u32 flags, then `size`
 * bytes of subrecords; a subrecord is char[4] type, u32 size, data.
 */

export type Subrecord = { type: string; data: Uint8Array };
export type EsmRecord = { type: string; flags: number; raw: Uint8Array; subs: Subrecord[] };

const tag = (b: Uint8Array, o: number) => String.fromCharCode(b[o]!, b[o + 1]!, b[o + 2]!, b[o + 3]!);

export function* readEsm(buf: Uint8Array): Generator<EsmRecord> {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  let o = 0;
  while (o + 16 <= buf.length) {
    const type = tag(buf, o), size = dv.getUint32(o + 4, true), flags = dv.getUint32(o + 12, true);
    const end = o + 16 + size;
    if (end > buf.length) throw new Error(`truncated ${type} record at ${o}`);
    const subs: Subrecord[] = [];
    for (let p = o + 16; p + 8 <= end; ) {
      const st = tag(buf, p), ss = dv.getUint32(p + 4, true);
      subs.push({ type: st, data: buf.subarray(p + 8, p + 8 + ss) });
      p += 8 + ss;
    }
    yield { type, flags, raw: buf.subarray(o, end), subs };
    o = end;
  }
}

export const cString = (d: Uint8Array) => {
  let s = "";
  for (const c of d) {
    if (c === 0) break;
    s += String.fromCharCode(c);
  }
  return s;
};
