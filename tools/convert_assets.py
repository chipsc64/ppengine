#!/usr/bin/env python3
"""Convert the ppengine assets for the Apollo V4 (port plan phase 3, T12-T16).

Needs Python 3.8+ and ffmpeg (with libfreetype for the font); nothing else.

    python3 tools/convert_assets.py [--out build/data]

Output (all numbers big-endian, as the 68080 reads them, unless noted):

  models/<name>.ppm    3D models, format below (T12), and levels of detail
                       <name>_lod1/2/3.ppm for distances (see LOD_CELLS)
  textures/<name>.dds  DXT1 textures with full mip chain, standard DDS layout
                       (little-endian, as .dds files are) (T13)
  gfx/<name>.565       2D images in R5G6B5 with a colour key (T14, T25)
  font8x8.bin          96 glyphs (ASCII 32-127), 8 bytes each, MSB = left (T15)
  sounds/<name>.wav    16-bit mono PCM WAV, 22050 Hz (T16)
  manifest.txt         what was written, with polygon counts

.ppm model ("PPM1"):
  char[4]  "PPM1"
  u16      part count
  u16      reserved (0)
  per part:
    char[32] part name (nearest named MatrixTransform, NUL padded),
             e.g. "Light1On" for the gantry lights
    char[32] texture name without extension ("" = none: bind white.dds)
    u8[4]    tint R, G, B, flags (bit 0: lit; OSG GL_LIGHTING was on)
    u16      vertex count
    u16      index count
    vertices: float x, y, z, nx, ny, nz, u, v (32 bytes each), transforms
              already applied, OSG model coordinates (z is up). Vertices
              with the same position (and UV on textured parts) are shared:
              lighting is off, so normals don't split them.
    indices:  u16, polygons (triangles and quads) separated by $FFFF, the
              list format of magDrawIndexedPolygons (KB
              os/libraries/maggie-library.md)
    (pad to an even byte count; parts start on 2-byte boundaries)

.565 image ("P565"):
  char[4] "P565", u16 width, u16 height, u16 key colour, u16 flags
  (bit 0: the image has keyed pixels), then width*height u16 pixels.
  Pixels that were transparent (alpha < 128) hold the key colour; opaque
  pixels never do.
"""

import argparse
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MODELS = [
    "car1", "car2", "car3", "car4",
    "highsign-amusement", "highsign-atari", "highsign-centipede", "highsign-digdug",
    "highsign-leftarrow", "highsign-poleposition", "highsign-rightarrow", "highsign-usa",
    "lowsign-atari", "lowsign-namco", "startline",
]

# Textures that are not referenced by a converted model.
# (name, source, size): size = square texture size in pixels.
EXTRA_TEXTURES = [
    ("road", "images/road_texture.png", 256),
    ("road_startline", "images/road_texture2.png", 256),
    ("explosion", "textures/explosion.png", 64),
]
MAX_TEXTURE_SIZE = 256
MIN_TEXTURE_SIZE = 32

# 2D images: (name, source, width, height)
IMAGES = [
    ("mountains", "models/textures/mountains4x2blurredblended.png", 2048, 64),
    ("gameover", "models/textures/gameover.png", 256, 32),
    ("namco", "images/namcolow.png", 256, 55),
]

# Levels of detail: vertex clustering on a grid of this cell size (model
# units). maggie.library spends ~10 us of CPU per polygon on the V4 (measured
# 2026-11-14), so distant models must have fewer. Level 0 is the full model.
# Cars are solid and take coarse cells. The signs and the start gantry are
# built from bars of ~0.1 units, which coarse cells make disappear (seen on
# the V4), so they get cells below half a pixel at the distance they are used
# (src/scene/maggie/maggie_backend.c, lod_distance).
LOD_CELLS = [0.0, 0.15, 0.25, 0.4]
SCENERY_LOD_CELLS = [0.0, 0.05, 0.1, 0.2]
CAR_MODELS = ("car1", "car2", "car3", "car4")

SOUNDS = [
    ("engine", "sounds/f1sound1.ogg"),
    ("skidding", "sounds/skidding.wav"),
    ("prepare_to_qualify", "sounds/prepare_to_qualify.wav"),
    ("crash", "sounds/crash.wav"),
]
SOUND_RATE = 22050

KEY_COLOUR = 0xF81F  # magenta in R5G6B5


# ----------------------------------------------------------------------------
# ffmpeg helpers

def ffmpeg(args, capture=True):
    command = ["ffmpeg", "-hide_banner", "-v", "error", "-nostdin"] + args
    result = subprocess.run(command, stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.PIPE)
    if result.returncode != 0:
        sys.exit("ffmpeg failed: %s\n%s" % (" ".join(command), result.stderr.decode(errors="replace")))
    return result.stdout


def load_rgba(path, width, height):
    """The image at path scaled to width x height, as bytes RGBA."""
    data = ffmpeg(["-i", path, "-vf", "scale=%d:%d:flags=lanczos,format=rgba" % (width, height),
                   "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgba", "-"])
    if len(data) != width * height * 4:
        sys.exit("unexpected image size from %s" % path)
    return data


def image_size(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                          "stream=width,height", "-of", "csv=p=0", path],
                         stdout=subprocess.PIPE, check=True).stdout.decode().strip()
    w, h = out.split(",")
    return int(w), int(h)


# ----------------------------------------------------------------------------
# OSG ASCII (.osgt) parser

def tokenize(line):
    tokens = []
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if c.isspace():
            i += 1
        elif c == '"':
            j = i + 1
            while j < n and line[j] != '"':
                j += 2 if line[j] == "\\" else 1
            tokens.append(line[i:j + 1])
            i = j + 1
        else:
            j = i
            while j < n and not line[j].isspace():
                j += 1
            tokens.append(line[i:j])
            i = j
    return tokens


class Block:
    def __init__(self, head):
        self.head = head      # tokens before '{'
        self.items = []       # token lists and Blocks

    def blocks(self, name=None):
        return [b for b in self.items if isinstance(b, Block) and (name is None or b.head[0] == name)]

    def block(self, name):
        found = self.blocks(name)
        return found[0] if found else None

    def lines(self):
        return [l for l in self.items if not isinstance(l, Block)]

    def value(self, key):
        for line in self.lines():
            if line and line[0] == key:
                return line[1:]
        return None


def parse_osgt(path):
    root = Block(["<root>"])
    stack = [root]
    with open(path, encoding="latin-1") as f:
        for raw in f:
            if raw.lstrip().startswith("#"):
                continue
            tokens = tokenize(raw)
            if not tokens:
                continue
            if tokens == ["}"]:
                stack.pop()
            elif tokens[-1] == "{":
                block = Block(tokens[:-1])
                stack[-1].items.append(block)
                stack.append(block)
            else:
                stack[-1].items.append(tokens)
    if len(stack) != 1:
        sys.exit("%s: unbalanced braces" % path)
    return root


def unquote(token):
    return token[1:-1] if token.startswith('"') else token


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


IDENTITY = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]


def transform_point(p, m):  # OSG: row vector times matrix
    x, y, z = p
    return tuple(x * m[0][j] + y * m[1][j] + z * m[2][j] + m[3][j] for j in range(3))


def transform_normal(n, m):
    x, y, z = n
    r = [x * m[0][j] + y * m[1][j] + z * m[2][j] for j in range(3)]
    length = math.sqrt(sum(c * c for c in r)) or 1.0
    return tuple(c / length for c in r)


def array_rows(block):
    """Rows of numbers from an 'Array TRUE ArrayID n VecXfArray count' block."""
    array = block.block("Array")
    if array is None:
        return []
    return [[float(t) for t in line] for line in array.lines()]


def find_first(block, name):
    """Depth-first search for a block named name."""
    for b in block.blocks():
        if b.head[0] == name:
            return b
        found = find_first(b, name)
        if found:
            return found
    return None


def parse_geometry(geometry, matrix, part_name):
    stateset = geometry.block("StateSet")
    tint = (255, 255, 255)
    lit = True
    texture = ""
    if stateset:
        material = find_first(stateset, "osg::Material")
        if material:
            diffuse = material.value("Diffuse")
            if diffuse:  # TRUE Front r g b a Back ...
                rgb = [float(v) for v in diffuse[2:5]]
                tint = tuple(max(0, min(255, int(round(c * 255)))) for c in rgb)
        modes = find_first(stateset, "ModeList")
        if modes and modes.value("GL_LIGHTING") == ["OFF"]:
            lit = False
        image = find_first(stateset, "Image")
        if image and image.value("FileName"):
            texture = os.path.splitext(os.path.basename(unquote(image.value("FileName")[0])))[0]

    positions = array_rows(geometry.block("VertexData")) if geometry.block("VertexData") else []
    normals = array_rows(geometry.block("NormalData")) if geometry.block("NormalData") else []
    texcoords = []
    tex_block = geometry.block("TexCoordData")
    if tex_block and tex_block.block("Data"):
        texcoords = array_rows(tex_block.block("Data"))

    vertices = []
    for i, p in enumerate(positions):
        n = normals[i] if i < len(normals) else (0.0, 0.0, 1.0)
        uv = texcoords[i] if i < len(texcoords) else (0.0, 0.0)
        vertices.append(transform_point(p, matrix) + transform_normal(n, matrix) + (uv[0], 1.0 - uv[1]))

    polygons = []
    primitive_list = geometry.block("PrimitiveSetList")
    for primitive in (primitive_list.blocks() if primitive_list else []):
        kind = primitive.head[1]
        indices = [int(t) for line in primitive.lines() for t in line]
        size = {"GL_TRIANGLES": 3, "GL_QUADS": 4}.get(kind)
        if size is None:
            sys.exit("unsupported primitive %s" % kind)
        for i in range(0, len(indices), size):
            polygons.append(indices[i:i + size])

    return {"name": part_name, "texture": texture, "tint": tint, "lit": lit,
            "vertices": vertices, "polygons": polygons}


def collect_parts(block, matrix, part_name, parts):
    kind = block.head[0]
    if kind == "osg::MatrixTransform":
        name = block.value("Name")
        if name:
            part_name = unquote(name[0])
        m_block = block.block("Matrix")
        if m_block:
            local = [[float(t) for t in line] for line in m_block.lines()]
            matrix = mat_mul(local, matrix)
    if kind == "osg::Geometry":
        parts.append(parse_geometry(block, matrix, part_name))
        return
    for child in block.blocks():
        if child.head[0] in ("Children", "Drawables") or child.head[0].startswith("osg::") \
                or child.head[0] == "<root>":
            collect_parts(child, matrix, part_name, parts)


def merge_parts(parts):
    """One part per (name, texture, tint, lit): fewer draw calls on Maggie."""
    merged = {}
    order = []
    for part in parts:
        key = (part["name"], part["texture"], part["tint"], part["lit"])
        if key not in merged:
            merged[key] = {"name": part["name"], "texture": part["texture"], "tint": part["tint"],
                           "lit": part["lit"], "vertices": [], "polygons": []}
            order.append(key)
        target = merged[key]
        base = len(target["vertices"])
        target["vertices"].extend(part["vertices"])
        target["polygons"].extend([[i + base for i in poly] for poly in part["polygons"]])
    return [merged[k] for k in order]


def polygon_normal(points):
    """Newell normal (not normalised) of a polygon."""
    nx = ny = nz = 0.0
    for i, a in enumerate(points):
        b = points[(i + 1) % len(points)]
        nx += (a[1] - b[1]) * (a[2] + b[2])
        ny += (a[2] - b[2]) * (a[0] + b[0])
        nz += (a[0] - b[0]) * (a[1] + b[1])
    return nx, ny, nz


def is_planar_convex_quad(points):
    """True when the 4 points form a flat, convex quad (what Maggie draws)."""
    n = polygon_normal(points)
    length = math.sqrt(sum(c * c for c in n))
    if length < 1e-9:
        return False
    n = tuple(c / length for c in n)
    size = max(math.dist(points[0], p) for p in points[1:]) or 1.0
    d0 = sum(n[i] * points[0][i] for i in range(3))
    for p in points[1:]:
        if abs(sum(n[i] * p[i] for i in range(3)) - d0) > 0.01 * size:
            return False
    for i in range(4):
        a, b, c = points[i], points[(i + 1) % 4], points[(i + 2) % 4]
        e1 = [b[k] - a[k] for k in range(3)]
        e2 = [c[k] - b[k] for k in range(3)]
        cross = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        if sum(cross[k] * n[k] for k in range(3)) < -1e-9:
            return False
    return True


def optimise_part(part, cell):
    """Shared vertices; with cell > 0 also vertex clustering."""
    textured = bool(part["texture"])
    keys = {}
    vertices = []
    remap = []
    for v in part["vertices"]:
        if cell:
            key = tuple(round(c / cell) for c in v[:3])
        else:
            key = tuple(round(c, 4) for c in v[:3])
        if textured:
            key += tuple(round(c, 2 if cell else 4) for c in v[6:8])
        if key not in keys:
            keys[key] = len(vertices)
            vertices.append(v)
        remap.append(keys[key])

    polygons = []
    seen = set()
    for poly in part["polygons"]:
        indices = []
        for i in poly:
            r = remap[i]
            if not indices or (r != indices[-1] and r != indices[0]):
                indices.append(r)
        if len(indices) < 3:
            continue
        points = [vertices[i][:3] for i in indices]
        n = polygon_normal(points)
        area = math.sqrt(sum(c * c for c in n)) / 2
        if area < 1e-6:
            continue
        # Polygons facing down were left out here once (the camera is above
        # the cars), but that opened holes in the cars on the V4: the road
        # showed through at the back. All polygons are kept.
        key = tuple(sorted(indices))
        if key in seen:
            continue
        seen.add(key)
        if len(indices) == 4 and not is_planar_convex_quad(points):
            # Split along the diagonal that keeps both triangles facing the
            # quad's way.
            a, b, c, d = indices
            first = [(a, b, c), (a, c, d)]
            second = [(a, b, d), (b, c, d)]
            def facing(tris):
                return all(sum(x * y for x, y in zip(polygon_normal([vertices[i][:3] for i in t]), n)) > 0
                           for t in tris)
            polygons.extend(first if facing(first) or not facing(second) else second)
        elif len(indices) > 4:
            polygons.extend((indices[0], indices[k], indices[k + 1]) for k in range(1, len(indices) - 1))
        else:
            polygons.append(tuple(indices))

    used = sorted(set(i for poly in polygons for i in poly))
    compact = {old: new for new, old in enumerate(used)}
    return {"name": part["name"], "texture": part["texture"], "tint": part["tint"], "lit": part["lit"],
            "vertices": [vertices[i] for i in used],
            "polygons": [[compact[i] for i in poly] for poly in polygons]}


def lattice_clusters(part, link=0.35):
    """Groups of vertices that belong together: shared by polygons, or
    closer than `link` to each other. Returns lists of polygon indices."""
    vertices = part["vertices"]
    parent = list(range(len(vertices)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for poly in part["polygons"]:
        for i in poly[1:]:
            parent[find(i)] = find(poly[0])
    grid = {}
    for i, v in enumerate(vertices):
        grid.setdefault(tuple(int(math.floor(c / link)) for c in v[:3]), []).append(i)
    for key, members in grid.items():
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for j in grid.get((key[0] + dx, key[1] + dy, key[2] + dz), []):
                        parent[find(j)] = find(members[0])
    groups = {}
    for index, poly in enumerate(part["polygons"]):
        groups.setdefault(find(poly[0]), []).append(index)
    return list(groups.values())


def solidify_lattices(part):
    """Replaces flat lattice panels (gantry posts, sign legs) by one solid
    quad each. Their bars are thinner than a pixel at a distance, and Maggie
    then draws them only in pieces (seen on the V4); a solid panel in the same
    colour reads better and costs one polygon instead of ~150."""
    vertices = list(part["vertices"])
    keep = []
    quads = []
    for group in lattice_clusters(part):
        polys = [part["polygons"][i] for i in group]
        points = [vertices[i][:3] for poly in polys for i in poly]
        lo = [min(p[k] for p in points) for k in range(3)]
        hi = [max(p[k] for p in points) for k in range(3)]
        extent = [hi[k] - lo[k] for k in range(3)]
        flat_axis = min(range(3), key=lambda k: extent[k])
        normal = [0.0, 0.0, 0.0]
        area = 0.0
        for poly in polys:
            n = polygon_normal([vertices[i][:3] for i in poly])
            normal = [normal[k] + n[k] for k in range(3)]
            area += math.sqrt(sum(c * c for c in n)) / 2
        a, b = [k for k in range(3) if k != flat_axis]
        box_area = extent[a] * extent[b]
        long_side = max(extent[a], extent[b])
        short_side = min(extent[a], extent[b])
        # Posts and legs are long and narrow; the "X" boxes beside the START
        # banner are flat lattices too, but square, and must keep their look.
        is_lattice = (len(polys) >= 20 and extent[flat_axis] < 0.02 and box_area > 0
                      and area < 0.8 * box_area and abs(normal[flat_axis]) > 0
                      and short_side > 0 and long_side >= 2 * short_side)
        if not is_lattice:
            keep.extend(polys)
            continue
        corners = []
        for ca_, cb_ in ((lo[a], lo[b]), (hi[a], lo[b]), (hi[a], hi[b]), (lo[a], hi[b])):
            p = [0.0, 0.0, 0.0]
            p[flat_axis] = lo[flat_axis]
            p[a] = ca_
            p[b] = cb_
            corners.append(tuple(p))
        n = polygon_normal(corners)
        if sum(n[k] * normal[k] for k in range(3)) < 0:
            corners.reverse()
        unit = [0.0, 0.0, 0.0]
        unit[flat_axis] = 1.0 if normal[flat_axis] > 0 else -1.0
        base = len(vertices)
        for c in corners:
            vertices.append(c + tuple(unit) + (0.0, 0.0))
        quads.append([base, base + 1, base + 2, base + 3])
    result = dict(part)
    result["vertices"] = vertices
    result["polygons"] = keep + quads
    return result


def name_field(text):
    raw = text.encode("latin-1")[:31]
    return raw + b"\0" * (32 - len(raw))


def write_model(path, parts):
    out = bytearray(b"PPM1" + struct.pack(">HH", len(parts), 0))
    for part in parts:
        indices = []
        for poly in part["polygons"]:
            if indices:
                indices.append(0xFFFF)
            indices.extend(poly)
        if len(part["vertices"]) >= 0xFFFF or len(indices) > 0xFFFF:
            sys.exit("%s: part %s too large" % (path, part["name"]))
        out += name_field(part["name"]) + name_field(part["texture"])
        out += struct.pack(">BBBB", *part["tint"], 1 if part["lit"] else 0)
        out += struct.pack(">HH", len(part["vertices"]), len(indices))
        for v in part["vertices"]:
            out += struct.pack(">8f", *v)
        out += struct.pack(">%dH" % len(indices), *indices)
    with open(path, "wb") as f:
        f.write(out)


# ----------------------------------------------------------------------------
# DXT1 (BC1) encoder and DDS writer

def to565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def from565(c):
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    b = c & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def encode_block(pixels):
    """pixels: 16 (r, g, b, a) tuples, row by row. Returns 8 bytes (DXT1)."""
    opaque = [p for p in pixels if p[3] >= 128]
    has_alpha = len(opaque) < 16
    if not opaque:
        return struct.pack("<HHI", 0, 0, 0xFFFFFFFF)

    # Endpoints: the two opaque pixels furthest apart along the bounding-box diagonal.
    lo = [min(p[i] for p in opaque) for i in range(3)]
    hi = [max(p[i] for p in opaque) for i in range(3)]
    axis = [hi[i] - lo[i] for i in range(3)]

    def projection(p):
        return sum(p[i] * axis[i] for i in range(3))

    a = min(opaque, key=projection)
    b = max(opaque, key=projection)
    c0 = to565(*a[:3])
    c1 = to565(*b[:3])

    if has_alpha:
        if c0 > c1:
            c0, c1 = c1, c0
        e0, e1 = from565(c0), from565(c1)
        palette = [e0, e1, tuple((e0[i] + e1[i]) // 2 for i in range(3))]
        transparent_index = 3
    else:
        if c0 < c1:
            c0, c1 = c1, c0
        e0, e1 = from565(c0), from565(c1)
        if c0 == c1:
            palette = [e0]
        else:
            palette = [e0, e1,
                       tuple((2 * e0[i] + e1[i]) // 3 for i in range(3)),
                       tuple((e0[i] + 2 * e1[i]) // 3 for i in range(3))]
        transparent_index = None

    bits = 0
    for n, p in enumerate(pixels):
        if transparent_index is not None and p[3] < 128:
            index = transparent_index
        else:
            index = min(range(len(palette)),
                        key=lambda k: sum((p[i] - palette[k][i]) ** 2 for i in range(3)))
        bits |= index << (2 * n)
    return struct.pack("<HHI", c0, c1, bits)


def encode_dxt1(rgba, size):
    out = bytearray()
    for by in range(0, max(size, 4), 4):
        for bx in range(0, max(size, 4), 4):
            pixels = []
            for y in range(4):
                for x in range(4):
                    sx = min(bx + x, size - 1)
                    sy = min(by + y, size - 1)
                    o = (sy * size + sx) * 4
                    pixels.append(tuple(rgba[o:o + 4]))
            out += encode_block(pixels)
    return bytes(out)


def downsample(rgba, size):
    half = size // 2
    out = bytearray(half * half * 4)
    for y in range(half):
        for x in range(half):
            for c in range(4):
                s = 0
                for dy in (0, 1):
                    for dx in (0, 1):
                        s += rgba[((2 * y + dy) * size + 2 * x + dx) * 4 + c]
                out[(y * half + x) * 4 + c] = (s + 2) // 4
    return bytes(out)


def write_dds(path, rgba, size):
    levels = []
    current = rgba
    level_size = size
    while True:
        levels.append(encode_dxt1(current, level_size))
        if level_size == 1:
            break
        current = downsample(current, level_size)
        level_size //= 2
    has_alpha = any(rgba[i] < 128 for i in range(3, len(rgba), 4))

    header = struct.pack("<4sIIIIIII", b"DDS ", 124,
                         0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000,
                         size, size, len(levels[0]), 0, len(levels))
    header += b"\0" * 44
    header += struct.pack("<II4sIIIII", 32, 0x4 | (0x1 if has_alpha else 0), b"DXT1", 0, 0, 0, 0, 0)
    header += struct.pack("<IIIII", 0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)
    assert len(header) == 128
    with open(path, "wb") as f:
        f.write(header)
        for level in levels:
            f.write(level)
    return has_alpha, len(levels)


def texture_size(path):
    w, h = image_size(path)
    size = 1 << max(0, math.ceil(math.log2(max(w, h))))
    return max(MIN_TEXTURE_SIZE, min(MAX_TEXTURE_SIZE, size))


# ----------------------------------------------------------------------------
# 2D images, font, sounds

def write_565(path, rgba, width, height):
    pixels = []
    keyed = False
    for i in range(width * height):
        r, g, b, a = rgba[i * 4:i * 4 + 4]
        if a < 128:
            pixels.append(KEY_COLOUR)
            keyed = True
        else:
            c = to565(r, g, b)
            pixels.append(c ^ 1 if c == KEY_COLOUR else c)
    with open(path, "wb") as f:
        f.write(b"P565" + struct.pack(">HHHH", width, height, KEY_COLOUR, 1 if keyed else 0))
        f.write(struct.pack(">%dH" % len(pixels), *pixels))
    return keyed


def write_font(path, ttf):
    """Glyphs 32-127 of the TrueType font rendered at 8 pixels, 1 bit per pixel."""
    chars = "".join(chr(c) for c in range(32, 128))
    width = 8 * len(chars)
    with tempfile.TemporaryDirectory() as tmp:
        textfile = os.path.join(tmp, "chars.txt")
        with open(textfile, "w", encoding="latin-1") as f:
            f.write(chars.replace("\x7f", " "))
        gray = ffmpeg(["-f", "lavfi", "-i", "color=c=black:s=%dx8" % width, "-frames:v", "1",
                       "-vf", "drawtext=fontfile=%s:textfile=%s:expansion=none:fontsize=8:fontcolor=white:x=0:y=0"
                       % (ttf, textfile), "-f", "rawvideo", "-pix_fmt", "gray", "-"])
    out = bytearray()
    for c in range(len(chars)):
        for y in range(8):
            row = 0
            for x in range(8):
                if gray[y * width + c * 8 + x] >= 128:
                    row |= 0x80 >> x
            out.append(row)
    with open(path, "wb") as f:
        f.write(out)
    return out


def write_sound(path, source):
    ffmpeg(["-y", "-i", source, "-ac", "1", "-ar", str(SOUND_RATE), "-c:a", "pcm_s16le",
            "-map_metadata", "-1", "-fflags", "+bitexact", "-flags:a", "+bitexact", path])
    return os.path.getsize(path)


# ----------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default=os.path.join(ROOT, "build", "data"))
    args = parser.parse_args()
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg is needed")

    out = args.out
    for sub in ("models", "textures", "gfx", "sounds"):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    manifest = []

    # T12: models
    textures = {}
    for name in MODELS:
        source = os.path.join(ROOT, "models", name + ".osgt")
        parts = []
        collect_parts(parse_osgt(source), IDENTITY, "", parts)
        parts = merge_parts(parts)
        cells = LOD_CELLS if name in CAR_MODELS else SCENERY_LOD_CELLS
        for level, cell in enumerate(cells):
            source_parts = parts
            if level > 0 and name not in CAR_MODELS:
                source_parts = [solidify_lattices(p) for p in parts]
            lod = [optimise_part(p, cell) for p in source_parts]
            lod = [p for p in lod if p["polygons"]]
            filename = name + (".ppm" if level == 0 else "_lod%d.ppm" % level)
            write_model(os.path.join(out, "models", filename), lod)
            polygons = sum(len(p["polygons"]) for p in lod)
            vertices = sum(len(p["vertices"]) for p in lod)
            manifest.append("model   %-24s %3d parts %5d polygons %5d vertices" % (filename, len(lod), polygons, vertices))
        for part in parts:
            if part["texture"]:
                textures[part["texture"]] = os.path.join(ROOT, "models", "textures", part["texture"] + ".png")

    # T13: textures
    jobs = [(n, p, texture_size(p)) for n, p in sorted(textures.items())]
    jobs += [(n, os.path.join(ROOT, p), s) for n, p, s in EXTRA_TEXTURES]
    for name, source, size in jobs:
        has_alpha, levels = write_dds(os.path.join(out, "textures", name + ".dds"),
                                      load_rgba(source, size, size), size)
        manifest.append("texture %-24s %4dx%-4d %2d mips%s" % (name, size, size, levels,
                                                                 " 1-bit alpha" if has_alpha else ""))
    write_dds(os.path.join(out, "textures", "white.dds"), bytes([255] * 32 * 32 * 4), 32)
    manifest.append("texture %-24s   32x32   6 mips (for untextured parts)" % "white")

    # T14, T25: 2D images
    for name, source, width, height in IMAGES:
        keyed = write_565(os.path.join(out, "gfx", name + ".565"),
                          load_rgba(os.path.join(ROOT, source), width, height), width, height)
        manifest.append("image   %-24s %4dx%-4d%s" % (name, width, height, " colour key" if keyed else ""))

    # T15: font
    write_font(os.path.join(out, "font8x8.bin"), os.path.join(ROOT, "fonts", "prstartk.ttf"))
    manifest.append("font    font8x8.bin              96 glyphs 8x8")

    # T16: sounds
    for name, source in SOUNDS:
        size = write_sound(os.path.join(out, "sounds", name + ".wav"), os.path.join(ROOT, source))
        manifest.append("sound   %-24s %d bytes, 16-bit mono %d Hz" % (name, size, SOUND_RATE))

    with open(os.path.join(out, "manifest.txt"), "w") as f:
        f.write("\n".join(manifest) + "\n")
    print("\n".join(manifest))


if __name__ == "__main__":
    main()
