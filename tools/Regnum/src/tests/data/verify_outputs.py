#!/usr/bin/env python3
# Regnum — внешняя проверка файлов, записанных тестами кодеков (совместимость с Python zlib/gzip, Pillow, zipfile).
#
# Порядок:
#   tools/Regnum/build.sh test-codec --test        # тесты пишут файлы в .wmma/regnum-tests/codec
#   python tools/Regnum/src/tests/data/verify_outputs.py
#
# Проверяется:
#   codec/text.{0,1,6,9}.zz и .gz — распаковка zlib/gzip совпадает с codec/text.txt;
#   codec/png/kind*.png — Pillow читает PNG, пиксели RGBA совпадают с исходными kind*.rgba;
#   codec/архив тест.zip — zipfile.testzip() без ошибок, имена, CRC и размеры совпадают со списком .zip.txt.
# Код возврата 0 — всё совпало.
import gzip
import os
import sys
import zipfile
import zlib

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", "..", ".."))
OUT = os.environ.get("REGNUM_TEST_OUT", os.path.join(REPO, ".wmma", "regnum-tests"))
CODEC = os.path.join(OUT, "codec")

errors = []
checked = 0


def check(cond, msg):
    global checked
    checked += 1
    if not cond:
        errors.append(msg)


text = open(os.path.join(CODEC, "text.txt"), "rb").read()
for lv in (0, 1, 6, 9):
    zz = open(os.path.join(CODEC, f"text.{lv}.zz"), "rb").read()
    check(zlib.decompress(zz) == text, f"text.{lv}.zz: zlib")
    gz = open(os.path.join(CODEC, f"text.{lv}.gz"), "rb").read()
    check(gzip.decompress(gz) == text, f"text.{lv}.gz: gzip")

pngdir = os.path.join(CODEC, "png")
for name in sorted(os.listdir(pngdir)):
    if not name.endswith(".png"):
        continue
    im = Image.open(os.path.join(pngdir, name))
    im.load()
    raw = open(os.path.join(pngdir, name[:-4] + ".rgba"), "rb").read()
    got = im.convert("RGBA").tobytes()
    check(got == raw, f"{name}: пиксели не совпадают (режим Pillow {im.mode})")

for extra in ("png_pattern.png", "map_tile.png", "portrait.png"):
    p = os.path.join(CODEC, extra)
    if os.path.exists(p):
        im = Image.open(p)
        im.load()
        check(im.size[0] > 0, extra)

zpath = os.path.join(CODEC, "архив тест.zip")
with zipfile.ZipFile(zpath) as z:
    check(z.testzip() is None, "zip: testzip")
    check(z.comment.decode("utf-8") == "Архив Regnum", "zip: комментарий")
    infos = {i.filename: i for i in z.infolist()}
    for line in open(zpath + ".txt", encoding="utf-8").read().splitlines():
        name, crc, size = line.split("\t")
        i = infos.get(name)
        check(i is not None, f"zip: нет записи {name}")
        if i is None:
            continue
        check(i.CRC == int(crc, 16) and i.file_size == int(size), f"zip: {name}: CRC/размер")
        if not name.endswith("/"):
            data = z.read(name)
            check(zlib.crc32(data) & 0xFFFFFFFF == int(crc, 16), f"zip: {name}: содержимое")

if errors:
    print("ОШИБКИ:\n  " + "\n  ".join(errors))
    sys.exit(1)
print(f"OK: проверок {checked}")
