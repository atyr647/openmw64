/** Durations of WAV and MP3 files, from their headers (no Node APIs). */

export type AudioInfo = { seconds: number; channels: number; rate: number };

export function wavInfo(b: Uint8Array): AudioInfo | null {
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const tag = (o: number) => String.fromCharCode(b[o]!, b[o + 1]!, b[o + 2]!, b[o + 3]!);
  if (b.length < 12 || tag(0) !== "RIFF" || tag(8) !== "WAVE") return null;
  let channels = 0, rate = 0, byteRate = 0;
  for (let o = 12; o + 8 <= b.length; ) {
    const id = tag(o), size = dv.getUint32(o + 4, true);
    if (id === "fmt ") {
      channels = dv.getUint16(o + 10, true);
      rate = dv.getUint32(o + 12, true);
      byteRate = dv.getUint32(o + 16, true); // right for PCM and ADPCM alike
    } else if (id === "data" && byteRate) {
      return { seconds: Math.min(size, b.length - o - 8) / byteRate, channels, rate };
    }
    o += 8 + size + (size & 1);
  }
  return null;
}

// MPEG audio: bitrates (kbps) for [version][layer], sample rates for [version].
const BITRATES: Record<string, number[]> = {
  "1-1": [0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448],
  "1-2": [0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384],
  "1-3": [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],
  "2-1": [0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256],
  "2-2": [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
  "2-3": [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
};
const RATES: Record<number, number[]> = { 1: [44100, 48000, 32000], 2: [22050, 24000, 16000], 25: [11025, 12000, 8000] };

/** Sums the frames of an MP3 (after any ID3v2 tag). */
export function mp3Info(b: Uint8Array): AudioInfo | null {
  let o = 0;
  if (b.length > 10 && b[0] === 0x49 && b[1] === 0x44 && b[2] === 0x33)
    o = 10 + (((b[6]! & 0x7f) << 21) | ((b[7]! & 0x7f) << 14) | ((b[8]! & 0x7f) << 7) | (b[9]! & 0x7f));
  let seconds = 0, channels = 0, rate = 0, frames = 0;
  while (o + 4 <= b.length) {
    if (b[o] !== 0xff || (b[o + 1]! & 0xe0) !== 0xe0) { o++; continue; }
    const vbits = (b[o + 1]! >> 3) & 3, lbits = (b[o + 1]! >> 1) & 3;
    const bri = b[o + 2]! >> 4, sri = (b[o + 2]! >> 2) & 3, pad = (b[o + 2]! >> 1) & 1;
    const version = vbits === 3 ? 1 : vbits === 2 ? 2 : vbits === 0 ? 25 : 0;
    const layer = lbits === 3 ? 1 : lbits === 2 ? 2 : lbits === 1 ? 3 : 0;
    if (!version || !layer || bri === 0 || bri === 15 || sri === 3) { o++; continue; }
    const kbps = BITRATES[`${version === 1 ? 1 : 2}-${layer}`]![bri]!, sr = RATES[version]![sri]!;
    const samples = layer === 1 ? 384 : layer === 3 && version !== 1 ? 576 : 1152;
    const len = layer === 1 ? (Math.floor((12 * kbps * 1000) / sr) + pad) * 4 : Math.floor(((samples / 8) * kbps * 1000) / sr) + pad;
    if (len < 4) { o++; continue; }
    seconds += samples / sr;
    rate = sr;
    channels = (b[o + 3]! >> 6) === 3 ? 1 : 2;
    frames++;
    o += len;
  }
  return frames ? { seconds, channels, rate } : null;
}
