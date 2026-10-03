// regnum-cli icon — значок программы из векторного знака (app/logo.h): PNG 16/24/32/48/64/256 и ICO для Windows.
//
//   regnum-cli icon [файл.ico] [--png=<папка>]
//
// ICO: размеры до 64 — 32-битный DIB с маской (совместимо со всеми версиями Windows), 256 — PNG.
#include <cstdio>

#include "app/logo.h"
#include "base/fs.h"
#include "cli/cli.h"
#include "codec/png.h"

namespace rg::cli {
namespace {

constexpr int kSizes[] = {16, 24, 32, 48, 64, 256};

gfx::Image renderIcon(int size) {
  gfx::Image img(size, size, 0);
  gfx::Canvas c(img);
  app::drawLogoTile(c, RectF{0, 0, float(size), float(size)});
  return img;
}

void put16(std::string& s, u32 v) {
  s.push_back(char(v & 255));
  s.push_back(char((v >> 8) & 255));
}
void put32(std::string& s, u32 v) {
  put16(s, v & 0xffff);
  put16(s, v >> 16);
}

// DIB для ICO: BITMAPINFOHEADER (высота удвоена), пиксели BGRA снизу вверх без премультипликации, маска AND.
std::string dib(const gfx::Image& img) {
  std::string s;
  int w = img.w, h = img.h;
  int maskRow = ((w + 31) / 32) * 4;
  put32(s, 40);
  put32(s, u32(w));
  put32(s, u32(h * 2));
  put16(s, 1);
  put16(s, 32);
  put32(s, 0);
  put32(s, u32(w * h * 4 + maskRow * h));
  put32(s, 0);
  put32(s, 0);
  put32(s, 0);
  put32(s, 0);
  for (int y = h - 1; y >= 0; y--)
    for (int x = 0; x < w; x++) {
      Color c = gfx::unpremul(img.at(x, y));
      s.push_back(char(c.b));
      s.push_back(char(c.g));
      s.push_back(char(c.r));
      s.push_back(char(c.a));
    }
  for (int y = h - 1; y >= 0; y--) {
    std::string row(size_t(maskRow), '\0');
    for (int x = 0; x < w; x++)
      if ((img.at(x, y) >> 24) == 0) row[size_t(x / 8)] = char(u8(row[size_t(x / 8)]) | u8(0x80 >> (x % 8)));
    s += row;
  }
  return s;
}

std::string pngBytes(const gfx::Image& img) {
  codec::RgbaImage r;
  r.w = img.w;
  r.h = img.h;
  r.rgba = img.toRgba();
  std::vector<u8> b = codec::encodePng(r, 9);
  return std::string(b.begin(), b.end());
}

int runIcon(const std::vector<std::string>& args) {
  auto pos = positional(args);
  std::string out;
  if (!pos.empty()) out = pos[0];
  else {
    std::string assets = fs::join(fs::exeDir(), "../../assets");
    out = fs::isDir(assets) ? fs::join(assets, "regnum.ico") : std::string("regnum.ico");
  }
  std::string pngDir = option(args, "--png");
  std::vector<std::string> images;
  for (int size : kSizes) {
    gfx::Image img = renderIcon(size);
    images.push_back(size >= 256 ? pngBytes(img) : dib(img));
    if (!pngDir.empty()) {
      codec::RgbaImage r;
      r.w = img.w;
      r.h = img.h;
      r.rgba = img.toRgba();
      std::string f = fs::join(pngDir, "regnum-" + std::to_string(size) + ".png");
      if (!codec::writePngFile(f, r, 9)) fail("Не удалось записать " + f);
    }
  }
  std::string ico;
  put16(ico, 0);
  put16(ico, 1);
  put16(ico, u32(images.size()));
  u32 offset = u32(6 + 16 * images.size());
  for (size_t i = 0; i < images.size(); i++) {
    int size = kSizes[i];
    ico.push_back(char(size >= 256 ? 0 : size));
    ico.push_back(char(size >= 256 ? 0 : size));
    ico.push_back(0);
    ico.push_back(0);
    put16(ico, 1);
    put16(ico, 32);
    put32(ico, u32(images[i].size()));
    put32(ico, offset);
    offset += u32(images[i].size());
  }
  for (auto& im : images) ico += im;
  std::string err;
  if (!fs::writeFileAtomic(out, ico, &err)) fail("Не удалось записать «" + out + "»: " + err);
  std::printf("Значок: %s (%zu байт, размеры 16–256)\n", out.c_str(), ico.size());
  return 0;
}

Command reg("icon", "[файл.ico] [--png=<папка>]", "Значок программы (ICO и PNG) из векторного знака", &runIcon);

}  // namespace
}  // namespace rg::cli
