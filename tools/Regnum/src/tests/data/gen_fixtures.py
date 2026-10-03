#!/usr/bin/env python3
# Regnum — генератор эталонных данных для тестов кодеков (zlib, PNG, JPEG, ZIP).
#
# Запуск из корня репозитория:  python tools/Regnum/src/tests/data/gen_fixtures.py
# Нужен Python 3.10+ и Pillow (только для генерации; код C++ от Python не зависит).
# Результат — файлы рядом со скриптом (zlib/, png/, jpeg/, zip/). Повторный запуск детерминирован
# (кроме JPEG-эталонов, зависящих от версии libjpeg в Pillow; они проверяются с допуском).
#
# Эталоны:
#   zlib/  — входы и их сжатые варианты из python zlib/gzip (уровни, стратегии, raw, поля gzip, несколько участников);
#   png/   — PNG всех типов цвета и глубин (свой писатель на python zlib: все фильтры, Adam7, tRNS, разбиение IDAT),
#            ожидаемый RGBA8 — *.rgba.zz (zlib); писатель сверяется с Pillow;
#   jpeg/  — JPEG из Pillow (baseline/progressive, субдискретизация, рестарты, EXIF, Adobe RGB/CMYK) и из встроенного
#            простого кодера (4:4:0, 4:1:1, раздельные сканы), эталон RGB — декодирование Pillow (*.ref.png);
#            portraits.bin — средние цвета клеток 64×64 портретов репозитория (декодирование Pillow);
#   zip/   — архивы python zipfile (stored, deflate, дескрипторы данных, UTF-8 и CP437 имена, каталоги, bzip2),
#            а также сконструированные: ZIP64, шифрованная запись.
import gzip
import io
import math
import os
import random
import struct
import sys
import zipfile
import zlib

from PIL import Image, ImageOps

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", "..", ".."))


def out(rel, data):
    path = os.path.join(HERE, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


# =============================================================== zlib
def gen_zlib():
    rnd = random.Random(20261001)
    words = ("провинция государство гильдия войско флот казна налог торговля маршрут ход "
             "kingdom guild army fleet treasury trade route turn castle tower river mountain "
             "Арден Ривертон Вингард Имрахиль 12 345 6789 0,5 — «» ё Ё").split()
    lines = []
    size = 0
    while size < 16000:
        line = " ".join(rnd.choice(words) for _ in range(rnd.randint(3, 14)))
        lines.append(line)
        size += len(line.encode("utf-8")) + 1
    text = ("\n".join(lines) + "\n").encode("utf-8")
    rbin = bytes(rnd.getrandbits(8) for _ in range(8192))
    zeros = bytes(20000)
    pattern = bytes(ord("abcdefg"[i % ((i // 997) % 7 + 1)]) for i in range(20000))
    inputs = {"text.txt": text, "random.bin": rbin, "zeros.bin": zeros, "pattern.bin": pattern, "empty.bin": b"", "one.bin": b"A"}
    for name, data in inputs.items():
        out("zlib/" + name, data)

    manifest = []

    def add(fname, fmt, plain, data):
        out("zlib/" + fname, data)
        manifest.append(f"{fname} {fmt} {plain}")

    def comp(data, level=6, wbits=15, strategy=zlib.Z_DEFAULT_STRATEGY, mem=8):
        c = zlib.compressobj(level, zlib.DEFLATED, wbits, mem, strategy)
        return c.compress(data) + c.flush()

    for lv in (0, 1, 6, 9):
        add(f"text.{lv}.zz", "zlib", "text.txt", comp(text, lv))
    add("text.fixed.zz", "zlib", "text.txt", comp(text, 6, strategy=zlib.Z_FIXED))
    add("text.huff.zz", "zlib", "text.txt", comp(text, 6, strategy=zlib.Z_HUFFMAN_ONLY))
    add("text.rle.zz", "zlib", "text.txt", comp(text, 6, strategy=zlib.Z_RLE))
    add("text.w9.zz", "zlib", "text.txt", comp(text, 6, wbits=9))
    add("text.mem1.zz", "zlib", "text.txt", comp(text, 9, mem=1))
    add("text.raw", "raw", "text.txt", comp(text, 6, wbits=-15))
    # точки сброса: пустые несжатые блоки посреди потока
    c = zlib.compressobj(6)
    parts = []
    for i in range(0, len(text), 5000):
        parts.append(c.compress(text[i:i + 5000]))
        parts.append(c.flush(zlib.Z_SYNC_FLUSH if (i // 5000) % 2 == 0 else zlib.Z_FULL_FLUSH))
    parts.append(c.flush())
    add("text.flush.zz", "zlib", "text.txt", b"".join(parts))
    # gzip: модуль gzip (FNAME, mtime) и вручную со всеми полями
    bio = io.BytesIO()
    with gzip.GzipFile(filename="text.txt", mode="wb", fileobj=bio, mtime=1700000000) as g:
        g.write(text)
    add("text.gz", "gzip", "text.txt", bio.getvalue())
    raw = comp(text, 9, wbits=-15)
    hdr = bytearray(b"\x1f\x8b\x08" + bytes([4 | 8 | 16 | 2]) + struct.pack("<I", 1700000000) + b"\x02\x03")
    extra = b"RG" + struct.pack("<H", 5) + b"hello"
    hdr += struct.pack("<H", len(extra)) + extra
    hdr += b"file.txt\x00" + "комментарий".encode("latin-1", "replace") + b"\x00"
    hdr += struct.pack("<H", zlib.crc32(bytes(hdr)) & 0xFFFF)
    add("text.fields.gz", "gzip", "text.txt", bytes(hdr) + raw + struct.pack("<II", zlib.crc32(text), len(text) & 0xFFFFFFFF))
    half = len(text) // 2
    add("text.multi.gz", "gzip", "text.txt", gzip.compress(text[:half], 6, mtime=0) + gzip.compress(text[half:], 1, mtime=0))
    add("random.6.zz", "zlib", "random.bin", comp(rbin, 6))
    add("random.1.zz", "zlib", "random.bin", comp(rbin, 1))
    add("random.raw", "raw", "random.bin", comp(rbin, 9, wbits=-15))
    add("zeros.9.zz", "zlib", "zeros.bin", comp(zeros, 9))
    add("zeros.1.raw", "raw", "zeros.bin", comp(zeros, 1, wbits=-15))
    add("zeros.gz", "gzip", "zeros.bin", gzip.compress(zeros, 9, mtime=0))
    add("pattern.6.zz", "zlib", "pattern.bin", comp(pattern, 6))
    add("pattern.rle.zz", "zlib", "pattern.bin", comp(pattern, 6, strategy=zlib.Z_RLE))
    add("pattern.1.zz", "zlib", "pattern.bin", comp(pattern, 1))
    add("empty.zz", "zlib", "empty.bin", comp(b""))
    add("empty.gz", "gzip", "empty.bin", gzip.compress(b"", mtime=0))
    add("empty.raw", "raw", "empty.bin", comp(b"", 6, wbits=-15))
    add("one.zz", "zlib", "one.bin", comp(b"A", 9))
    out("zlib/manifest.txt", ("\n".join(manifest) + "\n").encode())
    print(f"zlib: {len(manifest)} потоков")


# =============================================================== PNG
def png_chunk(t, data):
    return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def filter_rows(rows, bpp, start_filter):
    outb = bytearray()
    prev = bytes(len(rows[0])) if rows else b""
    for y, row in enumerate(rows):
        f = (start_filter + y) % 5
        outb.append(f)
        for i, v in enumerate(row):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            pred = (0, a, b, (a + b) // 2, paeth(a, b, c))[f]
            outb.append((v - pred) & 255)
        prev = row
    return bytes(outb)


CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]


def pack_row(samples, depth):
    if depth == 16:
        return b"".join(struct.pack(">H", s) for s in samples)
    if depth == 8:
        return bytes(samples)
    outb = bytearray((len(samples) * depth + 7) // 8)
    for i, s in enumerate(samples):
        bit = i * depth
        outb[bit >> 3] |= s << (8 - depth - (bit & 7))
    return bytes(outb)


def make_png(w, h, ct, depth, sample, interlace=False, plte=None, trns=None, idat_size=700, start_filter=0):
    nch = CHANNELS[ct]
    bits = nch * depth
    bpp = max(1, bits // 8)

    def rows_for(xs, ys):
        rows = []
        for y in ys:
            samples = []
            for x in xs:
                samples.extend(sample(x, y))
            rows.append(pack_row(samples, depth))
        return rows

    if not interlace:
        raw = filter_rows(rows_for(range(w), range(h)), bpp, start_filter)
    else:
        raw = b""
        for p, (x0, y0, dx, dy) in enumerate(ADAM7):
            xs, ys = list(range(x0, w, dx)), list(range(y0, h, dy))
            if xs and ys:
                raw += filter_rows(rows_for(xs, ys), bpp, start_filter + p)
    z = zlib.compress(raw, 9)
    data = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ct, 0, 0, 1 if interlace else 0))
    data += png_chunk(b"gAMA", struct.pack(">I", 45455)) + png_chunk(b"tEXt", b"Comment\x00Regnum")
    if plte is not None:
        data += png_chunk(b"PLTE", bytes(c for rgb in plte for c in rgb))
    if trns is not None:
        data += png_chunk(b"tRNS", trns)
    data += png_chunk(b"abCd", b"unknown ancillary")
    for i in range(0, len(z), idat_size):
        data += png_chunk(b"IDAT", z[i:i + idat_size])
    data += png_chunk(b"IEND", b"")
    return data


def to8(v, depth):
    if depth == 16:
        return (v * 255 + 32767) // 65535
    if depth == 8:
        return v
    return v * 255 // ((1 << depth) - 1)


def gen_png():
    count = 0
    mism = []

    def emit(name, w, h, ct, depth, interlace=False, key=None, palsize=None, pal_alpha=False, start_filter=0):
        nonlocal count
        maxv = (1 << depth) - 1
        nch = CHANNELS[ct]

        def sample(x, y):
            vals = []
            for c in range(nch):
                v = (x * x * 3 + y * 5 + c * 77 + ((x ^ y) & 7) * 9 + (x * y) % 13) * 2654435761 >> 13
                vals.append(v % (maxv + 1) if (x + y + c) % 3 else (x * 31 + y * 17 + c * 50) * (maxv // 255 if maxv > 255 else 1) % (maxv + 1))
            return vals

        plte = trns = None
        pal = None
        if ct == 3:
            n = palsize or (1 << depth)
            pal = [((i * 37) % 256, (i * 91 + 40) % 256, (255 - i * 13) % 256) for i in range(n)]
            plte = pal
            if pal_alpha:
                trns = bytes((i * 53) % 256 for i in range(min(n, 200)))
        if key is not None:
            trns = b"".join(struct.pack(">H", k) for k in key)
        data = make_png(w, h, ct, depth, sample, interlace, plte, trns, start_filter=start_filter)
        exp = bytearray()
        for y in range(h):
            for x in range(w):
                s = sample(x, y)
                if ct == 0:
                    g = to8(s[0], depth)
                    a = 0 if key is not None and s[0] == key[0] else 255
                    exp += bytes((g, g, g, a))
                elif ct == 2:
                    a = 0 if key is not None and list(s) == list(key) else 255
                    exp += bytes((to8(s[0], depth), to8(s[1], depth), to8(s[2], depth), a))
                elif ct == 3:
                    i = s[0]
                    if i < len(pal):
                        a = trns[i] if trns is not None and i < len(trns) else 255
                        exp += bytes((*pal[i], a))
                    else:
                        exp += bytes((0, 0, 0, 255))
                elif ct == 4:
                    g = to8(s[0], depth)
                    exp += bytes((g, g, g, to8(s[1], depth)))
                else:
                    exp += bytes(to8(v, depth) for v in s)
        out(f"png/{name}.png", data)
        out(f"png/{name}.rgba.zz", zlib.compress(bytes(exp), 9))
        count += 1
        # сверка писателя с Pillow (16 бит Pillow приводит иначе — допуск 1)
        try:
            im = Image.open(io.BytesIO(data))
            im.load()
            if ct == 3 and (palsize is not None and palsize < (1 << depth)):
                return  # Pillow иначе обрабатывает индексы вне палитры
            got = im.convert("RGBA").tobytes()
            if depth == 16 or ct in (0, 4) and depth == 16:
                return
            bad = sum(1 for a, b in zip(got, exp) if abs(a - b) > 1)
            if bad:
                mism.append(f"{name}: {bad}")
        except Exception as e:  # noqa
            mism.append(f"{name}: Pillow {e}")

    for il in (False, True):
        sfx = "i" if il else "n"
        for d in (1, 2, 4, 8, 16):
            emit(f"g{d}_{sfx}", 37, 29, 0, d, il, start_filter=d)
        for d in (8, 16):
            emit(f"rgb{d}_{sfx}", 37, 29, 2, d, il, start_filter=d)
            emit(f"ga{d}_{sfx}", 37, 29, 4, d, il, start_filter=d + 1)
            emit(f"rgba{d}_{sfx}", 37, 29, 6, d, il, start_filter=d + 2)
        for d in (1, 2, 4, 8):
            emit(f"p{d}_{sfx}", 37, 29, 3, d, il, pal_alpha=d >= 4, start_filter=d)
    emit("g8_key", 37, 29, 0, 8, key=(77,))
    emit("g2_key", 37, 29, 0, 2, key=(1,))
    emit("g16_key", 37, 29, 0, 16, key=(1234,))
    emit("rgb8_key", 37, 29, 2, 8, key=(0, 0, 0))
    emit("rgb16_key", 31, 17, 2, 16, key=(10, 20, 30))
    emit("p8_short", 37, 29, 3, 8, palsize=100)
    for (w, h) in ((1, 1), (2, 3), (5, 9), (9, 5), (33, 1), (1, 17)):
        emit(f"rgba8_i_{w}x{h}", w, h, 6, 8, True)
        emit(f"g1_i_{w}x{h}", w, h, 0, 1, True)
    emit("rgb8_wide", 300, 3, 2, 8, start_filter=4)
    print(f"png: {count} файлов" + ("; расхождения с Pillow: " + ", ".join(mism) if mism else "; сверка с Pillow пройдена"))


def gen_png_repo():
    # Контрольные суммы RGBA8 (Pillow) для исходной карты и выборки PNG репозитория.
    lines = []
    paths = ["tools/Regnum/assets/source/Expanded Map.png"]
    media = []
    for root, _, files in os.walk(os.path.join(REPO, "11_Медиа")):
        for f in files:
            if f.lower().endswith(".png"):
                media.append(os.path.relpath(os.path.join(root, f), REPO).replace("\\", "/"))
    paths += sorted(media)[::15]
    for rel in paths:
        im = Image.open(os.path.join(REPO, rel))
        crc = zlib.crc32(im.convert("RGBA").tobytes()) & 0xFFFFFFFF
        lines.append(f"{rel}\t{im.size[0]}\t{im.size[1]}\t{crc:08x}")
    out("png/repo_crc.txt", ("\n".join(lines) + "\n").encode("utf-8"))
    print(f"png: контрольные суммы {len(lines)} файлов репозитория")


# =============================================================== JPEG: простой кодер baseline (для 4:4:0, 4:1:1, раздельных сканов)
ZIGZAG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
          35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63]
LUMA_Q = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
          18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99]
CHROMA_Q = [17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99] + [99] * 32
COS = [[math.cos((2 * x + 1) * u * math.pi / 16) for x in range(8)] for u in range(8)]


def fdct(block):
    tmp = [[0.0] * 8 for _ in range(8)]
    for y in range(8):
        for u in range(8):
            s = sum(block[y * 8 + x] * COS[u][x] for x in range(8))
            tmp[y][u] = s * (math.sqrt(0.5) if u == 0 else 1.0) / 2
    res = [0.0] * 64
    for u in range(8):
        for v in range(8):
            s = sum(tmp[y][u] * COS[v][y] for y in range(8))
            res[v * 8 + u] = s * (math.sqrt(0.5) if v == 0 else 1.0) / 2
    return res


def huff_lengths(freq, limit=16):
    syms = [s for s in range(len(freq)) if freq[s]]
    if len(syms) == 1:
        return {syms[0]: 1}
    leaves = sorted((freq[s], s) for s in syms)
    # package-merge
    lists = [[(w, [s]) for w, s in leaves]]
    cur = lists[0]
    for _ in range(limit - 1):
        packs = [(cur[i][0] + cur[i + 1][0], cur[i][1] + cur[i + 1][1]) for i in range(0, len(cur) - 1, 2)]
        cur = sorted([(w, [s]) for w, s in leaves] + packs, key=lambda t: t[0])
    lens = {}
    for w, ss in cur[:2 * len(syms) - 2]:
        for s in ss:
            lens[s] = lens.get(s, 0) + 1
    return lens


def build_table(freq):
    f = list(freq) + [0.5]  # резервный символ 256 (наименьший вес — самый длинный код): код из одних единиц не используется
    lens = huff_lengths(f, 16)
    order = sorted(lens, key=lambda s: (lens[s], s))
    code, prev, codes = 0, 0, {}
    counts = [0] * 17
    vals = []
    for s in order:
        L = lens[s]
        code <<= (L - prev)
        prev = L
        codes[s] = (code, L)
        code += 1
        if s != 256:
            counts[L] += 1
            vals.append(s)
    del codes[256]
    return codes, counts[1:], vals


class BitOut:
    def __init__(self):
        self.buf = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, v, n):
        for i in range(n - 1, -1, -1):
            self.acc = (self.acc << 1) | ((v >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.buf.append(self.acc)
                if self.acc == 0xFF:
                    self.buf.append(0)
                self.acc = self.n = 0

    def pad(self):
        while self.n:
            self.put(1, 1)


def category(v):
    v = abs(v)
    n = 0
    while v:
        n += 1
        v >>= 1
    return n


def encode_simple_jpeg(rgb, w, h, sampling, quality=85, restart=0, interleaved=True, q16=False):
    hmax = max(s[0] for s in sampling)
    vmax = max(s[1] for s in sampling)
    scale = 5000 / quality if quality < 50 else 200 - 2 * quality
    qt = [[max(1, min(255, (q * scale + 50) // 100)) for q in LUMA_Q], [max(1, min(255, (q * scale + 50) // 100)) for q in CHROMA_Q]]
    planes = []
    full = [[], [], []]
    for y in range(h):
        for x in range(w):
            r, g, b = rgb[(y * w + x) * 3:(y * w + x) * 3 + 3]
            full[0].append(0.299 * r + 0.587 * g + 0.114 * b)
            full[1].append(-0.168736 * r - 0.331264 * g + 0.5 * b + 128)
            full[2].append(0.5 * r - 0.418688 * g - 0.081312 * b + 128)
    mcux = (w + 8 * hmax - 1) // (8 * hmax)
    mcuy = (h + 8 * vmax - 1) // (8 * vmax)
    for c, (hs, vs) in enumerate(sampling):
        sx, sy = hmax // hs, vmax // vs
        cw, ch = (w * hs + hmax - 1) // hmax, (h * vs + vmax - 1) // vmax
        pw, ph = mcux * hs * 8, mcuy * vs * 8
        plane = [[0.0] * pw for _ in range(ph)]
        for j in range(ph):
            for i in range(pw):
                ii, jj = min(i, cw - 1), min(j, ch - 1)
                acc = cnt = 0
                for yy in range(jj * sy, min(h, (jj + 1) * sy)):
                    for xx in range(ii * sx, min(w, (ii + 1) * sx)):
                        acc += full[c][yy * w + xx]
                        cnt += 1
                plane[j][i] = acc / cnt
        planes.append((plane, cw, ch))

    def qblock(c, bx, by):
        plane = planes[c][0]
        blk = [plane[by * 8 + y][bx * 8 + x] - 128 for y in range(8) for x in range(8)]
        F = fdct(blk)
        t = qt[0 if c == 0 else 1]
        return [int(round(F[ZIGZAG[k]] / t[ZIGZAG[k]])) for k in range(64)]

    # порядок блоков по сканам: [(компоненты скана, [(c, bx, by), ...] по MCU)]
    scans = []
    if interleaved:
        mcus = []
        for my in range(mcuy):
            for mx in range(mcux):
                blocks = []
                for c, (hs, vs) in enumerate(sampling):
                    for v in range(vs):
                        for u in range(hs):
                            blocks.append((c, mx * hs + u, my * vs + v))
                mcus.append(blocks)
        scans.append((list(range(len(sampling))), mcus))
    else:
        for c in range(len(sampling)):
            cw, ch = planes[c][1], planes[c][2]
            mcus = [[(c, bx, by)] for by in range((ch + 7) // 8) for bx in range((cw + 7) // 8)]
            scans.append(([c], mcus))
    coefs = {}
    for _, mcus in scans:
        for blocks in mcus:
            for (c, bx, by) in blocks:
                if (c, bx, by) not in coefs:
                    coefs[(c, bx, by)] = qblock(c, bx, by)

    def encode_scan(mcus, emit_dc, emit_ac, bits, rst):
        pred = {}
        for m, blocks in enumerate(mcus):
            if restart and m and m % restart == 0:
                rst(m // restart - 1)
                pred = {}
            for (c, bx, by) in blocks:
                q = coefs[(c, bx, by)]
                t = 0 if c == 0 else 1
                diff = q[0] - pred.get(c, 0)
                pred[c] = q[0]
                cat = category(diff)
                emit_dc(t, cat)
                if cat:
                    bits(diff if diff > 0 else diff + (1 << cat) - 1, cat)
                zr = 0
                for k in range(1, 64):
                    v = q[k]
                    if v == 0:
                        zr += 1
                        continue
                    while zr >= 16:
                        emit_ac(t, 0xF0)
                        zr -= 16
                    cat = category(v)
                    emit_ac(t, (zr << 4) | cat)
                    bits(v if v > 0 else v + (1 << cat) - 1, cat)
                    zr = 0
                if zr:
                    emit_ac(t, 0)

    fdc = [[0] * 256 for _ in range(2)]
    fac = [[0] * 256 for _ in range(2)]
    for _, mcus in scans:
        encode_scan(mcus, lambda t, s: fdc[t].__setitem__(s, fdc[t][s] + 1), lambda t, s: fac[t].__setitem__(s, fac[t][s] + 1),
                    lambda v, n: None, lambda k: None)
    nt = 2 if len(sampling) > 1 else 1
    dct = [build_table(fdc[t]) for t in range(nt)]
    act = [build_table(fac[t]) for t in range(nt)]

    o = bytearray(b"\xff\xd8")
    o += b"\xff\xe0" + struct.pack(">H", 16) + b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00"
    for t in range(nt):
        if q16:
            body = bytes([0x10 | t]) + b"".join(struct.pack(">H", qt[t][ZIGZAG[k]]) for k in range(64))
        else:
            body = bytes([t]) + bytes(qt[t][ZIGZAG[k]] for k in range(64))
        o += b"\xff\xdb" + struct.pack(">H", len(body) + 2) + body
    sof = struct.pack(">BHHB", 8, h, w, len(sampling))
    for c, (hs, vs) in enumerate(sampling):
        sof += bytes([c + 1, (hs << 4) | vs, 0 if c == 0 else 1])
    o += (b"\xff\xc1" if q16 else b"\xff\xc0") + struct.pack(">H", len(sof) + 2) + sof
    for t in range(nt):
        for cls, tab in ((0, dct[t]), (1, act[t])):
            body = bytes([(cls << 4) | t]) + bytes(tab[1]) + bytes(tab[2])
            o += b"\xff\xc4" + struct.pack(">H", len(body) + 2) + body
    if restart:
        o += b"\xff\xdd" + struct.pack(">HH", 4, restart)
    for comps, mcus in scans:
        sos = bytes([len(comps)]) + b"".join(bytes([c + 1, 0 if c == 0 else 0x11]) for c in comps) + b"\x00?\x00"
        o += b"\xff\xda" + struct.pack(">H", len(sos) + 2) + sos
        bw = BitOut()

        def rst(k, bw=bw):
            bw.pad()
            bw.buf += bytes([0xFF, 0xD0 + (k % 8)])

        encode_scan(mcus, lambda t, s, bw=bw: bw.put(*dct[t][0][s]), lambda t, s, bw=bw: bw.put(*act[t][0][s]), bw.put, rst)
        bw.pad()
        o += bw.buf
    o += b"\xff\xd9"
    return bytes(o)


def gen_jpeg():
    src_path = None
    for root, _, files in os.walk(os.path.join(REPO, "11_Медиа", "Портреты_персонажей")):
        for f in sorted(files):
            if f.lower().endswith(".jpg"):
                src_path = os.path.join(root, f)
                break
        if src_path:
            break
    base = Image.open(src_path).convert("RGB")
    W, H = base.size
    crop = base.crop((W // 4, H // 6, W // 4 + 244, H // 6 + 180)).resize((61, 45), Image.LANCZOS)
    big = base.crop((W // 5, H // 5, W // 5 + 452, H // 5 + 324)).resize((113, 81), Image.LANCZOS)
    tiny = base.crop((W // 3, H // 4, W // 3 + 116, H // 4 + 76)).resize((29, 19), Image.LANCZOS)
    manifest = []

    def ref(name, data, tol, note=""):
        im = Image.open(io.BytesIO(data))
        im = ImageOps.exif_transpose(im)
        rgb = im.convert("RGB")
        out(f"jpeg/{name}.jpg", data)
        ref_png = io.BytesIO()
        rgb.save(ref_png, "PNG", optimize=True)
        out(f"jpeg/{name}.ref.png", ref_png.getvalue())
        manifest.append(f"{name} {rgb.size[0]} {rgb.size[1]} {tol}")

    def pil(img, **kw):
        bio = io.BytesIO()
        img.save(bio, "JPEG", **kw)
        return bio.getvalue()

    ref("b444", pil(crop, quality=92, subsampling=0), 2)
    ref("b422", pil(crop, quality=90, subsampling=1), 2)
    ref("b420", pil(crop, quality=90, subsampling=2), 2)
    ref("b420_big", pil(big, quality=85, subsampling=2), 2)
    ref("q100", pil(crop, quality=100, subsampling=0), 2)
    ref("q10", pil(crop, quality=10, subsampling=2), 2)
    ref("opt", pil(crop, quality=88, subsampling=2, optimize=True), 2)
    ref("p420", pil(crop, quality=90, subsampling=2, progressive=True), 2)
    ref("p444", pil(crop, quality=90, subsampling=0, progressive=True), 2)
    ref("p422_big", pil(big, quality=80, subsampling=1, progressive=True), 2)
    ref("gray", pil(crop.convert("L"), quality=90), 2)
    ref("gray_p", pil(crop.convert("L"), quality=90, progressive=True), 2)
    ref("rst", pil(big, quality=90, subsampling=2, restart_marker_blocks=3), 2)
    ref("rst_rows", pil(crop, quality=90, subsampling=1, restart_marker_rows=1), 2)
    ref("rst_p", pil(big, quality=90, subsampling=2, progressive=True, restart_marker_blocks=5), 2)
    ref("adobe_rgb", pil(crop, quality=92, keep_rgb=True, subsampling=0), 2)
    ref("cmyk", pil(crop.convert("CMYK"), quality=92), 2)
    for o in range(1, 9):
        ex = Image.Exif()
        ex[0x0112] = o
        ref(f"exif{o}", pil(tiny, quality=90, subsampling=2, exif=ex.tobytes()), 2)
    crop_rgb = crop.tobytes()
    big_rgb = big.tobytes()
    ref("c440", encode_simple_jpeg(crop_rgb, 61, 45, [(1, 2), (1, 1), (1, 1)], 90), 2)
    ref("c440_big", encode_simple_jpeg(big_rgb, 113, 81, [(1, 2), (1, 1), (1, 1)], 85, restart=7), 2)
    ref("c411", encode_simple_jpeg(crop_rgb, 61, 45, [(4, 1), (1, 1), (1, 1)], 90), 4)
    ref("c_scans", encode_simple_jpeg(crop_rgb, 61, 45, [(2, 2), (1, 1), (1, 1)], 90, interleaved=False), 2)
    ref("c_mixed", encode_simple_jpeg(crop_rgb, 61, 45, [(2, 2), (1, 1), (2, 2)], 90), 2)
    ref("c_q16", encode_simple_jpeg(crop_rgb, 61, 45, [(2, 1), (1, 1), (1, 1)], 95, q16=True, restart=4), 2)
    out("jpeg/manifest.txt", ("\n".join(manifest) + "\n").encode())

    # Портреты репозитория: средние цвета клеток 64×64 (декодирование Pillow с учётом EXIF).
    blob = bytearray()
    lst = []
    for root, _, files in os.walk(os.path.join(REPO, "11_Медиа")):
        for f in sorted(files):
            if not f.lower().endswith((".jpg", ".jpeg")):
                continue
            p = os.path.join(root, f)
            im = ImageOps.exif_transpose(Image.open(p)).convert("RGB")
            w, h = im.size
            px = im.load()
            cw, ch = (w + 63) // 64, (h + 63) // 64
            sums = [[0, 0, 0, 0] for _ in range(cw * ch)]
            for y in range(h):
                for x in range(w):
                    r, g, b = px[x, y]
                    s = sums[(y // 64) * cw + x // 64]
                    s[0] += r
                    s[1] += g
                    s[2] += b
                    s[3] += 1
            cells = bytes(min(255, int(s[k] / s[3] + 0.5)) for s in sums for k in range(3))
            rel = os.path.relpath(p, REPO).replace("\\", "/")
            lst.append((rel, w, h, len(blob)))
            blob += cells
    out("jpeg/portraits.bin", bytes(blob))
    out("jpeg/portraits.txt", ("\n".join(f"{r}\t{w}\t{h}\t{o}" for r, w, h, o in lst) + "\n").encode("utf-8"))
    print(f"jpeg: {len(manifest)} файлов, портретов {len(lst)}")


# =============================================================== ZIP
class NoSeek(io.RawIOBase):
    def __init__(self):
        self.b = bytearray()

    def writable(self):
        return True

    def write(self, d):
        self.b += d
        return len(d)

    def seekable(self):
        return False


def gen_zip():
    rnd = random.Random(7)
    text = ("Regnum — архив проекта.\n" * 200).encode("utf-8")
    rbin = bytes(rnd.getrandbits(8) for _ in range(12000))
    files = [("readme.txt", text, zipfile.ZIP_DEFLATED), ("data/числа.json", b'{"a": [1, 2, 3]}', zipfile.ZIP_STORED),
             ("empty.txt", b"", zipfile.ZIP_DEFLATED), ("big.bin", rbin, zipfile.ZIP_DEFLATED), ("café.txt", b"cp437", zipfile.ZIP_DEFLATED)]
    bio = io.BytesIO()
    with zipfile.ZipFile(bio, "w") as z:
        for n, d, m in files:
            zi = zipfile.ZipInfo(n, date_time=(2026, 10, 1, 12, 30, 44))
            zi.compress_type = m
            z.writestr(zi, d)
        z.writestr(zipfile.ZipInfo("dir/", date_time=(2026, 10, 1, 12, 0, 0)), b"")
        z.comment = "Комментарий архива".encode("utf-8")
    out("zip/basic.zip", bio.getvalue())

    ns = NoSeek()
    with zipfile.ZipFile(ns, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for n, d, _ in files[:4]:
            with z.open(n, "w") as f:
                f.write(d)
    out("zip/descriptor.zip", bytes(ns.b))

    bio = io.BytesIO()
    with zipfile.ZipFile(bio, "w", compression=zipfile.ZIP_BZIP2) as z:
        z.writestr("bz.txt", text)
        z.writestr(zipfile.ZipInfo("plain.txt"), b"ok")
    out("zip/bzip2.zip", bio.getvalue())

    # ZIP64: запись конца каталога zip64 и поля 0xFFFF в обычной записи конца
    base = open(os.path.join(HERE, "zip", "basic.zip"), "rb").read()
    eocd = base.rfind(b"PK\x05\x06")
    cd_size, cd_off = struct.unpack("<II", base[eocd + 12:eocd + 20])
    n = struct.unpack("<H", base[eocd + 10:eocd + 12])[0]
    body = base[:eocd]
    z64 = struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0, n, n, cd_size, cd_off)
    loc = struct.pack("<IIQI", 0x07064B50, 0, len(body), 1)
    end = struct.pack("<IHHHHIIH", 0x06054B50, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0)
    out("zip/zip64.zip", body + z64 + loc + end)

    # зашифрованная запись: бит 0 флагов в локальном и центральном заголовках
    enc = bytearray(base)
    lh = enc.find(b"PK\x03\x04")
    enc[lh + 6] |= 1
    ch = enc.find(b"PK\x01\x02")
    enc[ch + 8] |= 1
    out("zip/encrypted.zip", bytes(enc))
    print("zip: 5 архивов")


if __name__ == "__main__":
    what = sys.argv[1:] or ["zlib", "png", "pngrepo", "jpeg", "zip"]
    for w in what:
        {"zlib": gen_zlib, "png": gen_png, "pngrepo": gen_png_repo, "jpeg": gen_jpeg, "zip": gen_zip}[w]()
