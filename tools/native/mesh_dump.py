#!/usr/bin/env python3
"""Reads a mesh dump (--native_dump=<file> --native_dump_frame=<n>) and checks that the
engine's vertex data is decoded right: it writes the frame's geometry as an OBJ file and a
top-down picture of it.

Layouts (big-endian, found from the dumps):
- Vertex buffer: header of 32 bytes; words 24/28 are the GPU vertex fetch constant: address |
  type 3, and (size in words << 2) | endian. The data follows the header.
- Index buffer: header of 32 bytes; word 24 the data address, word 28 its size in bytes.
- Vertex declaration: element count at +24, elements from +52, 12 bytes each like
  D3DVERTEXELEMENT9: stream u16, offset u16, type u32, method u8, usage u8, usage index u8.

Usage: python tools/native/mesh_dump.py <dump path without extension> [out prefix]
"""
import collections
import math
import struct
import sys

# Xbox 360 D3DDECLTYPE values seen in the game (type: (struct format, component count)).
DECL_TYPES = {
    0x002A23B9: ('>3f', 'FLOAT3'),
    0x002C23A5: ('>2f', 'FLOAT2'),
    0x001A23A6: ('>4f', 'FLOAT4'),
    0x002C83A4: ('>f', 'FLOAT1'),
}
# A mesh this wide whose vertices sit this high on average is the sky dome.
SKY_EXTENT = 2000.0
SKY_HEIGHT = 100.0
USAGE = {0: 'POSITION', 1: 'BLENDWEIGHT', 2: 'BLENDINDICES', 3: 'NORMAL', 5: 'TEXCOORD',
         6: 'TANGENT', 7: 'BINORMAL', 10: 'COLOR'}


def load(prefix):
    data = open(prefix + '.bin', 'rb').read()
    blobs, meshes = {}, []
    for line in open(prefix + '.txt'):
        parts = line.split()
        if not parts or parts[0].startswith('#'):
            continue
        if parts[0] == 'blob':
            offset, size = int(parts[4]), int(parts[3])
            blobs[(parts[1], int(parts[2], 16))] = data[offset:offset + size]
        elif parts[0] == 'mesh':
            fields = dict(zip(parts[2::2], parts[3::2]))
            fields['record'] = parts[1]
            meshes.append(fields)
    return blobs, meshes


def u32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def declaration(blob):
    count = u32(blob, 24)
    elements = []
    for i in range(count):
        o = 52 + 12 * i
        stream, offset = struct.unpack_from('>HH', blob, o)
        etype = u32(blob, o + 4)
        method, usage, index = blob[o + 8], blob[o + 9], blob[o + 10]
        elements.append((stream, offset, etype, usage, index))
    return elements


def strip_triangles(indices):
    tris = []
    run = []
    for i in indices:
        if i == 0xFFFF:  # strip restart
            run = []
            continue
        run.append(i)
        if len(run) >= 3:
            a, b, c = run[-3], run[-2], run[-1]
            if len(run) % 2 == 0:
                a, b = b, a
            if a != b and b != c and a != c:
                tris.append((a, b, c))
    return tris


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    prefix = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else prefix
    blobs, meshes = load(prefix)
    stats = collections.Counter()
    decl_kinds = collections.Counter()
    vertices, triangles = [], []
    for m in meshes:
        vb, ib, decl = int(m['vb'], 16), int(m['ib'], 16), int(m['decl'], 16)
        vb_head, ib_head = blobs.get(('vb', vb)), blobs.get(('ib', ib))
        decl_blob = blobs.get(('decl', decl))
        if not (vb_head and ib_head and decl_blob):
            stats['missing blob'] += 1
            continue
        vdata = blobs.get(('vb_data', u32(vb_head, 24) & ~3))
        idata = blobs.get(('ib_data', u32(ib_head, 24) & ~3))
        if vdata is None or idata is None:
            stats['missing data'] += 1
            continue
        elements = declaration(decl_blob)
        decl_kinds[tuple((USAGE.get(e[3], e[3]), DECL_TYPES.get(e[2], ('', '%06X' % e[2]))[1])
                         for e in elements)] += 1
        pos = [e for e in elements if e[3] == 0 and e[4] == 0 and e[0] == 0]
        if not pos or pos[0][2] not in DECL_TYPES:
            stats['position not float'] += 1
            continue
        fmt = DECL_TYPES[pos[0][2]][0]
        stride = int(m['stride'])
        count = len(vdata) // stride
        mesh_vertices = []
        for v in range(count):
            p = struct.unpack_from(fmt, vdata, v * stride + pos[0][1])
            mesh_vertices.append(tuple(p[:3]) if len(p) >= 3 else (p[0], p[1], 0.0))
        good = [p for p in mesh_vertices if all(math.isfinite(k) and abs(k) < 1e5 for k in p)]
        if len(good) < len(mesh_vertices) or not good:
            # Skinned meshes (the characters) keep their vertices in another space.
            stats['not in world space'] += 1
            continue
        extent = [max(p[i] for p in good) - min(p[i] for p in good) for i in range(3)]
        center_y = sum(p[1] for p in good) / len(good)
        if max(extent) > SKY_EXTENT and center_y > SKY_HEIGHT:
            stats['sky'] += 1
            continue
        base_index = len(vertices)
        vertices.extend(mesh_vertices)
        index_count = int(m['indices'])
        indices = struct.unpack_from('>%dH' % min(index_count, len(idata) // 2), idata)
        tris = [(a + base_index, b + base_index, c + base_index)
                for a, b, c in strip_triangles(indices) if max(a, b, c) < count]
        triangles.extend(tris)
        stats['meshes drawn'] += 1
    print('%d meshes in the dump: %s' % (len(meshes), dict(stats)))
    print('%d vertices, %d triangles' % (len(vertices), len(triangles)))
    print('vertex layouts (count: elements):')
    for kind, n in decl_kinds.most_common(12):
        print('  %4d: %s' % (n, ', '.join('%s %s' % k for k in kind)))

    with open(out + '.obj', 'w') as f:
        for x, y, z in vertices:
            f.write('v %g %g %g\n' % (x, y, z))
        for a, b, c in triangles:
            f.write('f %d %d %d\n' % (a + 1, b + 1, c + 1))

    # Top-down picture (x right, z down), lowest triangles first, flat-shaded.
    from PIL import Image, ImageDraw
    finite = [v for v in vertices if all(math.isfinite(c) and abs(c) < 1e5 for c in v)]
    xs = sorted(v[0] for v in finite)
    zs = sorted(v[2] for v in finite)
    lo_x, hi_x = xs[len(xs) // 100], xs[-len(xs) // 100 - 1]
    lo_z, hi_z = zs[len(zs) // 100], zs[-len(zs) // 100 - 1]
    size = 1600
    scale = size / max(hi_x - lo_x, hi_z - lo_z, 1e-3)
    img = Image.new('RGB', (size, size), (20, 20, 30))
    draw = ImageDraw.Draw(img)
    light = (0.4, 0.8, 0.3)
    order = sorted(triangles, key=lambda t: sum(vertices[i][1] if math.isfinite(vertices[i][1]) else -1e9 for i in t))
    def ok(p):
        return all(math.isfinite(k) and abs(k) < 1e5 for k in p)

    for a, b, c in order:
        pa, pb, pc = vertices[a], vertices[b], vertices[c]
        if not (ok(pa) and ok(pb) and ok(pc)):
            continue
        u = [pb[i] - pa[i] for i in range(3)]
        w = [pc[i] - pa[i] for i in range(3)]
        n = (u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0])
        length = math.sqrt(sum(k * k for k in n)) or 1.0
        shade = abs(sum(n[i] * light[i] for i in range(3))) / length
        g = int(40 + 200 * shade)
        pts = [((p[0] - lo_x) * scale, (p[2] - lo_z) * scale) for p in (pa, pb, pc)]
        draw.polygon(pts, fill=(g, g, int(g * 0.9)))
    img.save(out + '_top.png')
    print('wrote %s.obj and %s_top.png' % (out, out))


if __name__ == '__main__':
    main()
