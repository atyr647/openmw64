#!/usr/bin/env python3
"""Generate a tiny Morrowind-format data set for testing OpenMW-N64 without
the (copyrighted) game files.

Writes into <out>/ (default: ../filesystem, which the Makefile packs into the
ROM as rom:/):

  n64test.esm              TES3 plugin: STAT objects, an NPC, three cells
  n64test.bsa              Morrowind BSA with some meshes and textures
  meshes/test/*.nif        loose NIF 4.0.0.2 meshes (the rest are in the BSA)
  textures/*.dds           loose textures

Everything is laid out exactly as components/esm3, components/bsa and
components/nif read it; see those readers for the field order used below.

Optionally pass --example-suite DIR to also use two CC0 textures from
https://gitlab.com/OpenMW/example-suite (DXT1 rock, DXT5 fern with alpha).
"""

import argparse
import math
import os
import shutil
import struct
import subprocess
import tempfile


# --------------------------------------------------------------------------
# DDS (DXT1 with a full mip chain, as Morrowind's textures have)

def dxt1_block(pixels):
    """pixels: 16 (r,g,b) tuples. Returns 8 bytes."""
    def to565(c):
        return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)

    def from565(v):
        return (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)

    lum = [p[0] * 3 + p[1] * 6 + p[2] for p in pixels]
    hi = pixels[lum.index(max(lum))]
    lo = pixels[lum.index(min(lum))]
    c0, c1 = to565(hi), to565(lo)
    if c0 < c1:
        c0, c1 = c1, c0
    if c0 == c1:
        return struct.pack('<HHI', c0, c1, 0)
    a, b = from565(c0), from565(c1)
    palette = [a, b,
               tuple((2 * a[i] + b[i]) // 3 for i in range(3)),
               tuple((a[i] + 2 * b[i]) // 3 for i in range(3))]
    bits = 0
    for i, p in enumerate(pixels):
        best = min(range(4), key=lambda k: sum((p[j] - palette[k][j]) ** 2 for j in range(3)))
        bits |= best << (2 * i)
    return struct.pack('<HHI', c0, c1, bits)


def make_dds(width, height, pixel_fn):
    levels = []
    w, h = width, height
    image = [[pixel_fn(x, y) for x in range(w)] for y in range(h)]
    while True:
        data = bytearray()
        for by in range(0, max(h, 4), 4):
            for bx in range(0, max(w, 4), 4):
                block = [image[min(by + j, h - 1)][min(bx + i, w - 1)] for j in range(4) for i in range(4)]
                data += dxt1_block(block)
        levels.append(bytes(data))
        if w == 1 and h == 1:
            break
        nw, nh = max(1, w // 2), max(1, h // 2)
        image = [[tuple(sum(image[min(y * 2 + dy, h - 1)][min(x * 2 + dx, w - 1)][c]
                            for dx in (0, 1) for dy in (0, 1)) // 4 for c in range(3))
                  for x in range(nw)] for y in range(nh)]
        w, h = nw, nh

    header = bytearray(128)
    header[0:4] = b'DDS '
    struct.pack_into('<IIIIIII', header, 4, 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000,
                     height, width, len(levels[0]), 0, len(levels))
    struct.pack_into('<II4s', header, 76, 32, 0x4, b'DXT1')
    struct.pack_into('<I', header, 108, 0x1000 | 0x8 | 0x400000)
    return bytes(header) + b''.join(levels)


def planks(x, y):
    board = (y // 16) % 4
    base = [(150, 100, 60), (130, 85, 50), (160, 110, 65), (120, 80, 45)][board]
    grain = int(12 * math.sin(x * 0.4 + board * 2.1) + 8 * math.sin(x * 0.13))
    seam = 40 if y % 16 == 0 else 0
    return tuple(max(0, min(255, c + grain - seam)) for c in base)


def crate(x, y):
    edge = x < 6 or y < 6 or x > 57 or y > 57 or abs(x - y) < 4
    return (90, 60, 30) if edge else (185, 140, 80)


def stone(x, y):
    row = y // 16
    offset = 16 if row % 2 else 0
    mortar = y % 16 < 2 or (x + offset) % 32 < 2
    n = (x * 7 + y * 13) % 17
    return (70, 70, 75) if mortar else (140 + n, 135 + n, 125 + n)


# --------------------------------------------------------------------------
# NIF 4.0.0.2 (Morrowind)

class Nif:
    VERSION = 0x04000002

    def __init__(self):
        self.records = []  # (type name, bytes-producing callable)

    def add(self, type_name):
        self.records.append([type_name, b''])
        return len(self.records) - 1

    def set(self, index, data):
        self.records[index][1] = data

    def build(self, roots=(0,)):
        out = bytearray(b'NetImmerse File Format, Version 4.0.0.2\n')
        out += struct.pack('<II', self.VERSION, len(self.records))
        for type_name, data in self.records:
            out += sized(type_name.encode()) + data
        out += struct.pack('<I', len(roots))
        for r in roots:
            out += struct.pack('<i', r)
        return bytes(out)


def sized(b):
    return struct.pack('<I', len(b)) + b


def b32(v):  # NIF bools are 32-bit before 4.1.0.0
    return struct.pack('<i', 1 if v else 0)


def obj_net(name):
    return sized(name.encode()) + struct.pack('<ii', -1, -1)  # extra data, controller


def av_object(name, translation=(0, 0, 0), rotation=None, scale=1.0, properties=(), flags=0x000C):
    rotation = rotation or (1, 0, 0, 0, 1, 0, 0, 0, 1)
    out = obj_net(name) + struct.pack('<H', flags)
    out += struct.pack('<3f', *translation) + struct.pack('<9f', *rotation) + struct.pack('<f', scale)
    out += struct.pack('<3f', 0, 0, 0)  # velocity
    out += struct.pack('<I', len(properties)) + b''.join(struct.pack('<i', p) for p in properties)
    out += b32(False)  # no bounding volume
    return out


def ni_node(name, children, **kw):
    return (av_object(name, **kw) + struct.pack('<I', len(children))
            + b''.join(struct.pack('<i', c) for c in children) + struct.pack('<I', 0))


def geometry(name, data_ref, **kw):
    return av_object(name, **kw) + struct.pack('<ii', data_ref, -1)  # data, skin


def geometry_data(verts, normals, uvs, colors=None):
    out = struct.pack('<H', len(verts)) + b32(True) + b''.join(struct.pack('<3f', *v) for v in verts)
    out += b32(True) + b''.join(struct.pack('<3f', *n) for n in normals)
    cx = [sum(v[i] for v in verts) / len(verts) for i in range(3)]
    radius = max(math.dist(cx, v) for v in verts)
    out += struct.pack('<4f', *cx, radius)
    out += b32(colors is not None)
    if colors is not None:
        out += b''.join(struct.pack('<4f', *c) for c in colors)
    out += struct.pack('<H', 1 if uvs else 0) + b32(bool(uvs))
    if uvs:
        out += b''.join(struct.pack('<2f', *uv) for uv in uvs)
    return out


def tri_shape_data(verts, normals, uvs, tris, colors=None):
    out = geometry_data(verts, normals, uvs, colors) + struct.pack('<H', len(tris))
    out += struct.pack('<I', len(tris) * 3) + b''.join(struct.pack('<3H', *t) for t in tris)
    return out + struct.pack('<H', 0)  # match groups


def tri_strips_data(verts, normals, uvs, strips, colors=None):
    ntris = sum(len(s) - 2 for s in strips)
    out = geometry_data(verts, normals, uvs, colors) + struct.pack('<H', ntris)
    out += struct.pack('<H', len(strips)) + b''.join(struct.pack('<H', len(s)) for s in strips)
    # 4.0.0.2 has no 'has points' flag here (see NiTriStripsData::read)
    out += b''.join(struct.pack('<%dH' % len(s), *s) for s in strips)
    return out


def texturing_property(source_ref):
    out = obj_net('') + struct.pack('<H', 0) + struct.pack('<I', 2)  # flags, apply mode (modulate)
    out += struct.pack('<I', 1)  # one texture slot: base
    out += b32(True) + struct.pack('<i', source_ref)
    out += struct.pack('<III', 3, 2, 0)  # clamp (wrap), filter, uv set
    out += struct.pack('<hh', 0, 0) + struct.pack('<H', 0)  # PS2 L/K, unknown
    return out


def source_texture(filename):
    return (obj_net('') + struct.pack('<B', 1) + sized(filename.encode())
            + struct.pack('<III', 5, 2, 3) + struct.pack('<B', 1))


def alpha_property(flags=0x12ED, threshold=128):
    return obj_net('') + struct.pack('<H', flags) + struct.pack('<B', threshold)


def material_property(diffuse=(1, 1, 1), emissive=(0, 0, 0)):
    return (obj_net('') + struct.pack('<H', 0) + struct.pack('<3f', *diffuse) + struct.pack('<3f', *diffuse)
            + struct.pack('<3f', 0, 0, 0) + struct.pack('<3f', *emissive) + struct.pack('<ff', 10.0, 1.0))


def box(sx, sy, sz, z0=0.0, uv_scale=1.0):
    """Axis-aligned box, faces wound CCW seen from outside."""
    verts, normals, uvs, tris = [], [], [], []
    faces = [((1, 0, 0), (0, 1, 0), (0, 0, 1)), ((-1, 0, 0), (0, -1, 0), (0, 0, 1)),
             ((0, 1, 0), (-1, 0, 0), (0, 0, 1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
             ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    half = (sx / 2, sy / 2, sz / 2)
    for n, u, v in faces:
        base = len(verts)
        for du, dv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = [n[i] * half[i] + u[i] * du * half[i] + v[i] * dv * half[i] for i in range(3)]
            p[2] += half[2] + z0
            verts.append(tuple(p))
            normals.append(n)
            uvs.append(((du + 1) / 2 * uv_scale, (1 - dv) / 2 * uv_scale))
        tris += [(base, base + 1, base + 2), (base, base + 2, base + 3)]
    return verts, normals, uvs, tris


def textured_mesh(texture, verts, normals, uvs, tris, extra_props=()):
    nif = Nif()
    root = nif.add('NiNode')
    shape = nif.add('NiTriShape')
    data = nif.add('NiTriShapeData')
    tex = nif.add('NiTexturingProperty')
    src = nif.add('NiSourceTexture')
    props = [tex]
    for type_name, payload in extra_props:
        props.append(nif.add(type_name))
        nif.set(props[-1], payload)
    nif.set(root, ni_node('Scene Root', [shape]))
    nif.set(shape, geometry('Tri Shape', data, properties=props))
    nif.set(data, tri_shape_data(verts, normals, uvs, tris))
    nif.set(tex, texturing_property(src))
    nif.set(src, source_texture(texture))
    return nif.build()


def floor_nif():
    s = 1024.0
    verts = [(-s, -s, 0), (s, -s, 0), (s, s, 0), (-s, s, 0)]
    uvs = [(0, 8), (8, 8), (8, 0), (0, 0)]
    return textured_mesh('tx_n64_planks.tga', verts, [(0, 0, 1)] * 4, uvs, [(0, 1, 2), (0, 2, 3)])


def walls_nif(texture):
    """Four inward-facing walls as NiTriStrips (one strip per wall)."""
    s, h = 1024.0, 384.0
    corners = [(-s, -s), (s, -s), (s, s), (-s, s)]
    verts, normals, uvs, strips = [], [], [], []
    for i in range(4):
        (x0, y0), (x1, y1) = corners[i], corners[(i + 1) % 4]
        n = (-(y1 - y0) / (2 * s), (x1 - x0) / (2 * s), 0)  # points into the room
        base = len(verts)
        # vertices: bottom-left, top-left, bottom-right, top-right; the strip
        # BL, TL, BR, TR is counter-clockwise seen from inside the room
        for (x, y, z, u, v) in ((x0, y0, 0, 0, 3), (x0, y0, h, 0, 0), (x1, y1, 0, 5, 3), (x1, y1, h, 5, 0)):
            verts.append((x, y, z))
            normals.append(n)
            uvs.append((u, v))
        strips.append([base + 0, base + 1, base + 2, base + 3])
    nif = Nif()
    root = nif.add('NiNode')
    shape = nif.add('NiTriStrips')
    data = nif.add('NiTriStripsData')
    tex = nif.add('NiTexturingProperty')
    src = nif.add('NiSourceTexture')
    nif.set(root, ni_node('Scene Root', [shape]))
    nif.set(shape, geometry('Walls', data, properties=[tex]))
    nif.set(data, tri_strips_data(verts, normals, uvs, strips))
    nif.set(tex, texturing_property(src))
    nif.set(src, source_texture(texture))
    return nif.build()


def crate_nif():
    """Crate with a glowing, rotated child box and a collision node that must not render."""
    nif = Nif()
    root = nif.add('NiNode')
    shape = nif.add('NiTriShape')
    data = nif.add('NiTriShapeData')
    tex = nif.add('NiTexturingProperty')
    src = nif.add('NiSourceTexture')
    child = nif.add('NiNode')
    gem = nif.add('NiTriShape')
    gem_data = nif.add('NiTriShapeData')
    gem_mat = nif.add('NiMaterialProperty')
    collision = nif.add('RootCollisionNode')
    col_shape = nif.add('NiTriShape')
    col_data = nif.add('NiTriShapeData')

    nif.set(root, ni_node('Scene Root', [shape, child, collision]))
    nif.set(shape, geometry('Crate', data, properties=[tex]))
    nif.set(data, tri_shape_data(*box(64, 64, 64)))
    nif.set(tex, texturing_property(src))
    nif.set(src, source_texture('tx_n64_crate.tga'))

    c, s = math.cos(math.radians(45)), math.sin(math.radians(45))
    nif.set(child, ni_node('Gem Node', [gem], translation=(0, 0, 80), rotation=(c, -s, 0, s, c, 0, 0, 0, 1),
                           scale=0.5))
    v, n, uv, t = box(40, 40, 40)
    colors = [(0.3 + 0.7 * (p[2] > 20), 0.9, 0.4, 1.0) for p in v]
    nif.set(gem, geometry('Gem', gem_data, properties=[gem_mat]))
    nif.set(gem_data, tri_shape_data(v, n, uv, t, colors))
    nif.set(gem_mat, material_property(emissive=(0.2, 0.5, 0.2)))

    # A huge box only for collision: if it ever renders, the whole room goes dark.
    nif.set(collision, ni_node('Collision', [col_shape]))
    nif.set(col_shape, geometry('Collision Box', col_data))
    nif.set(col_data, tri_shape_data(*box(1000, 1000, 1000, z0=-500)))
    return nif.build()


def pillar_nif():
    """Vertex-colored, untextured box as NiTriStrips."""
    v, n, uv, tris = box(48, 48, 320)
    colors = [(0.8, 0.3 + 0.5 * (p[2] / 320), 0.3, 1.0) for p in v]
    strips = [[a, b, d, c] for (a, b, c), (_, _, d) in zip(tris[0::2], tris[1::2])]
    nif = Nif()
    root = nif.add('NiNode')
    shape = nif.add('NiTriStrips')
    data = nif.add('NiTriStripsData')
    nif.set(root, ni_node('Scene Root', [shape]))
    nif.set(shape, geometry('Pillar', data))
    nif.set(data, tri_strips_data(v, n, uv, strips, colors))
    return nif.build()


def fern_nif(texture):
    """Two crossed, double-sided quads with alpha testing."""
    verts, normals, uvs, tris = [], [], [], []
    for angle in (0, 90):
        c, s = math.cos(math.radians(angle)), math.sin(math.radians(angle))
        base = len(verts)
        for (u, z) in ((-1, 0), (1, 0), (1, 1), (-1, 1)):
            verts.append((u * 64 * c, u * 64 * s, z * 128))
            normals.append((-s, c, 0))
            uvs.append(((u + 1) / 2, 1 - z))
        tris += [(base, base + 1, base + 2), (base, base + 2, base + 3),
                 (base, base + 2, base + 1), (base, base + 3, base + 2)]
    return textured_mesh(texture, verts, normals, uvs, tris, [('NiAlphaProperty', alpha_property())])


# --------------------------------------------------------------------------
# BSA (Morrowind, version 0x100)

def make_bsa(files):
    """files: list of (path with backslashes, bytes)."""
    names = bytearray()
    name_offsets = []
    for path, _ in files:
        name_offsets.append(len(names))
        names += path.encode() + b'\0'
    n = len(files)
    dirsize = 12 * n + len(names)
    offsets = bytearray()
    data = bytearray()
    for _, content in files:
        offsets += struct.pack('<II', len(content), len(data))
        data += content
    out = struct.pack('<III', 0x100, dirsize, n) + offsets
    out += b''.join(struct.pack('<I', o) for o in name_offsets) + names
    out += b'\0' * (8 * n)  # hashes (only the game's own lookup uses them)
    return out + data


# --------------------------------------------------------------------------
# ESM (TES3)

def sub(name, data):
    return name.encode() + struct.pack('<I', len(data)) + data


def zstr(s):
    return s.encode() + b'\0'


def record(name, subs, flags=0):
    body = b''.join(subs)
    return name.encode() + struct.pack('<III', len(body), 0, flags) + body


def ref(index, obj_id, pos, rot=(0, 0, 0), scale=None):
    subs = [sub('FRMR', struct.pack('<I', index)), sub('NAME', zstr(obj_id))]
    if scale is not None:
        subs.append(sub('XSCL', struct.pack('<f', scale)))
    subs.append(sub('DATA', struct.pack('<6f', *pos, *rot)))
    return b''.join(subs)


def cell(name, interior, refs, grid=(0, 0), ambient=None):
    subs = [sub('NAME', zstr(name)), sub('DATA', struct.pack('<Iii', 1 if interior else 0x2, *grid))]
    if ambient:
        subs.append(sub('AMBI', struct.pack('<IIIf', *ambient)))
    return record('CELL', subs + [r for r in refs])


def make_esm():
    statics = {
        'test_floor': 'test\\floor.nif',
        'test_walls': 'test\\walls.nif',
        'test_crate': 'test\\crate.nif',
        'test_pillar': 'test\\pillar.nif',
        'test_fern': 'test\\fern.nif',
        'test_missing': 'test\\does_not_exist.nif',
    }
    records = [record('STAT', [sub('NAME', zstr(i)), sub('MODL', zstr(m))]) for i, m in statics.items()]
    records.append(record('NPC_', [sub('NAME', zstr('test_npc')), sub('FNAM', zstr('Test NPC'))]))

    refs = [ref(1, 'test_floor', (0, 0, 0)), ref(2, 'test_walls', (0, 0, 0))]
    idx = 3
    for i, (x, y) in enumerate([(-300, 200), (250, 350), (400, -250), (-450, -400), (0, 600)]):
        refs.append(ref(idx, 'test_crate', (x, y, 0), (0, 0, i * 0.4), scale=1.0 + 0.25 * (i % 3)))
        idx += 1
    for x in (-600, 600):
        for y in (-600, 0, 600):
            refs.append(ref(idx, 'test_pillar', (x, y, 0)))
            idx += 1
    for x, y in [(-150, -150), (150, -100), (0, 250), (-500, 400)]:
        refs.append(ref(idx, 'test_fern', (x, y, 0), (0, 0, x * 0.01)))
        idx += 1
    refs.append(ref(idx, 'test_npc', (100, 100, 0)))
    refs.append(ref(idx + 1, 'test_missing', (0, 0, 0)))

    hall_ambient = (0x00403830, 0x00c8d0e0, 0x00181410, 0.5)  # 0x00BBGGRR
    cells = [
        cell('Test Hall', True, refs, ambient=hall_ambient),
        cell('Test Cellar', True, [ref(1, 'test_floor', (0, 0, 0)), ref(2, 'test_crate', (0, 200, 0))],
             ambient=(0x00101010, 0x00404080, 0, 1.0)),
        # Close-up of one plain and one rotated+scaled instance; the camera
        # starts at the centroid of the references, looking north (+Y).
        cell('Test Lab', True, [ref(1, 'test_floor', (0, 0, 0)),
                                ref(2, 'test_crate', (-60, 150, 0)),
                                ref(3, 'test_crate', (60, 150, 0), (0, 0, 0.5), scale=1.25),
                                ref(4, 'test_fern', (0, 220, 0)),
                                ref(5, 'test_pillar', (0, -300, 0))],
             ambient=(0x00403830, 0x00c8d0e0, 0x00181410, 0.5)),
        # Performance check: a 20x20 grid of crates (24 triangles each).
        cell('Test Stress', True, [ref(1, 'test_floor', (0, 0, 0))]
             + [ref(2 + i, 'test_crate', ((i % 20) * 100 - 950, (i // 20) * 100 - 950, 0), (0, 0, i * 0.3))
                for i in range(400)],
             ambient=(0x00403830, 0x00c8d0e0, 0x00181410, 0.5)),
        cell('', False, [ref(1, 'test_crate', (100, 100, 0))], grid=(1, 2)),  # exterior: not listed
    ]
    body = records + cells
    hedr = struct.pack('<fI', 1.3, 0) + b'OpenMW-N64'.ljust(32, b'\0') \
        + b'Test data for the N64 port'.ljust(256, b'\0') + struct.pack('<I', len(body))
    return record('TES3', [sub('HEDR', hedr)]) + b''.join(body)


def imagemagick_dds(path, size):
    """DXT1 DDS with a full mip chain via ImageMagick, or None if unavailable."""
    if not os.path.exists(path) or open(path, 'rb').read(4) != b'DDS ':
        return None
    with tempfile.TemporaryDirectory() as tmp:
        dst = os.path.join(tmp, 'out.dds')
        try:
            subprocess.run(['convert', path, '-resize', f'{size}x{size}!', '-define', 'dds:compression=dxt1',
                            '-define', 'dds:mipmaps=8', 'dds:' + dst], check=True, capture_output=True)
        except (OSError, subprocess.CalledProcessError):
            return None
        return open(dst, 'rb').read()


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', default=os.path.join(here, '..', 'filesystem'))
    parser.add_argument('--example-suite', help='path to an OpenMW example-suite checkout (git lfs pulled)')
    args = parser.parse_args()

    out = args.out
    os.makedirs(os.path.join(out, 'meshes', 'test'), exist_ok=True)
    os.makedirs(os.path.join(out, 'textures'), exist_ok=True)

    def write(rel, data):
        with open(os.path.join(out, rel), 'wb') as f:
            f.write(data)

    wall_texture = 'tx_n64_stone.tga'
    fern_texture = 'tx_n64_crate.tga'
    rock = fern = None
    planks_dds = make_dds(64, 64, planks)
    crate_dds = make_dds(64, 64, crate)
    if args.example_suite and shutil.which('convert'):
        # Morrowind-sized (256x256 DXT1 with mipmaps) versions of CC0 textures,
        # so native-resolution texture paging has real detail to show.
        es = args.example_suite
        barrel = os.path.join(es, 'example_static_props', 'data', 'textures', 'the_barrel.dds')
        road = os.path.join(es, 'the_hub', 'data', 'textures', 'ground', 'road_01.dds')
        rock256 = os.path.join(es, 'game_template', 'data', 'textures', 'tx_rock_01.dds')
        crate_dds = imagemagick_dds(barrel, 256) or crate_dds
        planks_dds = imagemagick_dds(road, 256) or planks_dds
        rock = imagemagick_dds(rock256, 256)
        if rock:
            wall_texture = 'tx_rock_01.tga'
    if args.example_suite:
        rock_path = os.path.join(args.example_suite, 'game_template', 'data', 'textures', 'tx_rock_01.dds')
        fern_path = os.path.join(args.example_suite, 'the_hub', 'data', 'textures', 'fern_01.dds')
        if not rock and os.path.exists(rock_path) and open(rock_path, 'rb').read(4) == b'DDS ':
            rock = open(rock_path, 'rb').read()
            wall_texture = 'tx_rock_01.tga'
        if os.path.exists(fern_path) and open(fern_path, 'rb').read(4) == b'DDS ':
            fern = open(fern_path, 'rb').read()
            fern_texture = 'tx_n64_fern.tga'

    bsa_files = [
        ('meshes\\test\\floor.nif', floor_nif()),
        ('meshes\\test\\walls.nif', walls_nif(wall_texture)),
        ('meshes\\test\\crate.nif', crate_nif()),
        ('textures\\tx_n64_planks.dds', planks_dds),
        ('textures\\tx_n64_crate.dds', crate_dds),
        ('textures\\tx_n64_stone.dds', make_dds(64, 64, stone)),
    ]
    if rock:
        bsa_files.append(('textures\\tx_rock_01.dds', rock))
    write('n64test.bsa', make_bsa(bsa_files))

    # Loose files, to exercise the loose-over-BSA lookup.
    write('meshes/test/pillar.nif', pillar_nif())
    write('meshes/test/fern.nif', fern_nif(fern_texture))
    if fern:
        write('textures/tx_n64_fern.dds', fern)

    write('n64test.esm', make_esm())
    print('wrote test data to', os.path.normpath(out))


if __name__ == '__main__':
    main()
