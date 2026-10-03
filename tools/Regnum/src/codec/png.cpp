// Regnum — PNG: декодер (все типы цвета, глубины, Adam7) и кодер (адаптивные фильтры, сокращение формата).
#include "codec/png.h"

#include <cstdlib>

#include "base/fs.h"
#include "codec/zlib.h"

namespace rg::codec {

namespace {

constexpr u8 kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

inline u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]); }
inline u16 be16(const u8* p) { return u16((p[0] << 8) | p[1]); }

inline int channelsOf(int ct) {
  switch (ct) {
    case 0: return 1;
    case 2: return 3;
    case 3: return 1;
    case 4: return 2;
    case 6: return 4;
  }
  return 0;
}

bool depthValid(int ct, int d) {
  switch (ct) {
    case 0: return d == 1 || d == 2 || d == 4 || d == 8 || d == 16;
    case 3: return d == 1 || d == 2 || d == 4 || d == 8;
    case 2:
    case 4:
    case 6: return d == 8 || d == 16;
  }
  return false;
}

struct Adam7Pass {
  int x0, y0, dx, dy;
};
constexpr Adam7Pass kAdam7[7] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};

inline u64 rowBytes(u64 w, int bitsPerPixel) { return (w * u64(bitsPerPixel) + 7) / 8; }

bool setErr(std::string* error, const std::string& m) {
  if (error) *error = "PNG: " + m;
  return false;
}

bool parseHeader(const u8* data, size_t size, PngInfo& info, std::string* error) {
  if (size < 8 || std::memcmp(data, kSig, 8) != 0) return setErr(error, "неверная подпись файла");
  if (size < 33) return setErr(error, "файл обрезан");
  if (be32(data + 8) != 13 || std::memcmp(data + 12, "IHDR", 4) != 0) return setErr(error, "нет заголовка IHDR");
  if (crc32(data + 12, 17) != be32(data + 29)) return setErr(error, "ошибка контрольной суммы IHDR");
  const u8* h = data + 16;
  u32 w = be32(h), ht = be32(h + 4);
  info.bitDepth = h[8];
  info.colorType = h[9];
  if (w == 0 || ht == 0 || w > 0x7FFFFFFFu || ht > 0x7FFFFFFFu) return setErr(error, "неверный размер изображения");
  if (!depthValid(info.colorType, info.bitDepth)) return setErr(error, "неверное сочетание типа цвета и глубины");
  if (h[10] != 0 || h[11] != 0 || h[12] > 1) return setErr(error, "неподдерживаемый метод сжатия, фильтрации или чересстрочности");
  info.w = int(w);
  info.h = int(ht);
  info.interlaced = h[12] == 1;
  return true;
}

inline u8 paeth(int a, int b, int c) {
  int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
  if (pa <= pb && pa <= pc) return u8(a);
  return pb <= pc ? u8(b) : u8(c);
}

// Снять фильтр одной строки. src и dst могут перекрываться при dst <= src (сжатие на месте).
template <int BPP>
void unfilterRow(int f, const u8* src, u8* dst, const u8* prev, size_t n, int bppRuntime) {
  const size_t bpp = BPP > 0 ? size_t(BPP) : size_t(bppRuntime);
  switch (f) {
    case 0:
      if (dst != src) std::memmove(dst, src, n);
      break;
    case 1:
      for (size_t i = 0; i < bpp && i < n; i++) dst[i] = src[i];
      for (size_t i = bpp; i < n; i++) dst[i] = u8(src[i] + dst[i - bpp]);
      break;
    case 2:
      for (size_t i = 0; i < n; i++) dst[i] = u8(src[i] + prev[i]);
      break;
    case 3:
      for (size_t i = 0; i < bpp && i < n; i++) dst[i] = u8(src[i] + (prev[i] >> 1));
      for (size_t i = bpp; i < n; i++) dst[i] = u8(src[i] + ((dst[i - bpp] + prev[i]) >> 1));
      break;
    case 4:
      for (size_t i = 0; i < bpp && i < n; i++) dst[i] = u8(src[i] + prev[i]);
      for (size_t i = bpp; i < n; i++) dst[i] = u8(src[i] + paeth(dst[i - bpp], prev[i], prev[i - bpp]));
      break;
  }
}

// Снять фильтры с rows строк, начиная с buf: строки (1 + stride) сжимаются до stride на месте.
bool unfilterRows(u8* buf, size_t rows, size_t stride, int bpp, std::string* error) {
  std::vector<u8> zero(stride, 0);
  for (size_t y = 0; y < rows; y++) {
    u8* row = buf + y * (stride + 1);
    int f = row[0];
    if (f > 4) return setErr(error, "неверный тип фильтра строки");
    const u8* src = row + 1;
    u8* dst = buf + y * stride;
    const u8* prev = y ? buf + (y - 1) * stride : zero.data();
    switch (bpp) {
      case 1: unfilterRow<1>(f, src, dst, prev, stride, bpp); break;
      case 2: unfilterRow<2>(f, src, dst, prev, stride, bpp); break;
      case 3: unfilterRow<3>(f, src, dst, prev, stride, bpp); break;
      case 4: unfilterRow<4>(f, src, dst, prev, stride, bpp); break;
      case 6: unfilterRow<6>(f, src, dst, prev, stride, bpp); break;
      case 8: unfilterRow<8>(f, src, dst, prev, stride, bpp); break;
      default: unfilterRow<0>(f, src, dst, prev, stride, bpp); break;
    }
  }
  return true;
}

struct Format {
  int ct, depth;
  u8 pal[256][4];
  int palSize = 0;
  bool hasKey = false;
  u16 key[3] = {};  // tRNS для серого/RGB (в исходной глубине)
};

inline u8 to8(u16 v) { return u8((u32(v) * 255 + 32767) / 65535); }

// Значение пикселя x (глубина < 8) из упакованной строки.
inline u32 packed(const u8* row, size_t x, int depth) {
  size_t bit = x * size_t(depth);
  return (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
}

// Преобразовать пиксель x строки row в RGBA8.
inline void pixel(const Format& F, const u8* row, size_t x, u8* o) {
  switch (F.ct) {
    case 0: {
      u32 v;
      u8 g;
      if (F.depth == 16) {
        v = be16(row + x * 2);
        g = to8(u16(v));
      } else if (F.depth == 8) {
        v = row[x];
        g = u8(v);
      } else {
        v = packed(row, x, F.depth);
        g = u8(v * 255 / ((1u << F.depth) - 1));
      }
      o[0] = o[1] = o[2] = g;
      o[3] = (F.hasKey && v == F.key[0]) ? 0 : 255;
      break;
    }
    case 2:
      if (F.depth == 8) {
        const u8* p = row + x * 3;
        o[0] = p[0];
        o[1] = p[1];
        o[2] = p[2];
        o[3] = (F.hasKey && p[0] == F.key[0] && p[1] == F.key[1] && p[2] == F.key[2]) ? 0 : 255;
      } else {
        const u8* p = row + x * 6;
        u16 r = be16(p), g = be16(p + 2), b = be16(p + 4);
        o[0] = to8(r);
        o[1] = to8(g);
        o[2] = to8(b);
        o[3] = (F.hasKey && r == F.key[0] && g == F.key[1] && b == F.key[2]) ? 0 : 255;
      }
      break;
    case 3: {
      u32 i = F.depth == 8 ? row[x] : packed(row, x, F.depth);
      if (int(i) < F.palSize) std::memcpy(o, F.pal[i], 4);
      else { o[0] = o[1] = o[2] = 0; o[3] = 255; }  // индекс вне палитры — чёрный
      break;
    }
    case 4:
      if (F.depth == 8) {
        o[0] = o[1] = o[2] = row[x * 2];
        o[3] = row[x * 2 + 1];
      } else {
        o[0] = o[1] = o[2] = to8(be16(row + x * 4));
        o[3] = to8(be16(row + x * 4 + 2));
      }
      break;
    case 6:
      if (F.depth == 8) std::memcpy(o, row + x * 4, 4);
      else {
        const u8* p = row + x * 8;
        o[0] = to8(be16(p));
        o[1] = to8(be16(p + 2));
        o[2] = to8(be16(p + 4));
        o[3] = to8(be16(p + 6));
      }
      break;
  }
}

// Строка в RGBA8. Back — обход справа налево (расширение на месте: выход не затирает ещё не прочитанный вход).
template <bool Back>
void convertRow(const Format& F, const u8* row, u8* out, size_t w) {
  auto each = [w](auto&& fn) {
    if constexpr (Back) {
      for (size_t x = w; x-- > 0;) fn(x);
    } else {
      for (size_t x = 0; x < w; x++) fn(x);
    }
  };
  if (F.depth == 8 && !F.hasKey) {
    switch (F.ct) {
      case 0:
        each([&](size_t x) { u8 g = row[x]; out[x * 4] = g; out[x * 4 + 1] = g; out[x * 4 + 2] = g; out[x * 4 + 3] = 255; });
        return;
      case 2:
        each([&](size_t x) {
          u8 r = row[x * 3], g = row[x * 3 + 1], b = row[x * 3 + 2];
          out[x * 4] = r; out[x * 4 + 1] = g; out[x * 4 + 2] = b; out[x * 4 + 3] = 255;
        });
        return;
      case 3:
        each([&](size_t x) {
          u8 i = row[x];
          if (int(i) < F.palSize) std::memcpy(out + x * 4, F.pal[i], 4);
          else { out[x * 4] = out[x * 4 + 1] = out[x * 4 + 2] = 0; out[x * 4 + 3] = 255; }
        });
        return;
      case 4:
        each([&](size_t x) { u8 g = row[x * 2], a = row[x * 2 + 1]; out[x * 4] = g; out[x * 4 + 1] = g; out[x * 4 + 2] = g; out[x * 4 + 3] = a; });
        return;
      case 6:
        if (out != row) std::memmove(out, row, w * 4);
        return;
    }
  }
  u8 px[4];
  each([&](size_t x) {
    pixel(F, row, x, px);
    std::memcpy(out + x * 4, px, 4);
  });
}

}  // namespace

std::optional<PngInfo> pngInfo(const u8* data, size_t size, std::string* error) {
  PngInfo info;
  if (!parseHeader(data, size, info, error)) return std::nullopt;
  return info;
}

std::optional<RgbaImage> decodePng(const u8* data, size_t size, std::string* error) { return decodePng(data, size, PngDecodeOptions{}, error); }

std::optional<RgbaImage> decodePng(const u8* data, size_t size, const PngDecodeOptions& opt, std::string* error) {
  PngInfo info;
  if (!data || !parseHeader(data, size, info, error)) return std::nullopt;
  if (i64(info.w) * i64(info.h) > opt.maxPixels) {
    setErr(error, strf("слишком большое изображение (%d × %d)", info.w, info.h));
    return std::nullopt;
  }
  auto F = std::make_unique<Format>();
  F->ct = info.colorType;
  F->depth = info.bitDepth;
  for (int i = 0; i < 256; i++) F->pal[i][0] = F->pal[i][1] = F->pal[i][2] = 0, F->pal[i][3] = 255;

  // ---------------------------------------------------------------- блоки
  std::vector<u8> idat;
  bool havePlte = false, haveIend = false, idatDone = false, sawIdat = false;
  size_t pos = 33;
  while (pos + 12 <= size) {
    u32 len = be32(data + pos);
    if (len > 0x7FFFFFFFu) { setErr(error, "неверная длина блока"); return std::nullopt; }
    if (u64(pos) + 12 + len > size) {
      if (sawIdat) break;  // обрезанный хвост после данных: решит распаковка
      setErr(error, "файл обрезан");
      return std::nullopt;
    }
    const u8* type = data + pos + 4;
    const u8* cd = data + pos + 8;
    bool critical = !(type[0] & 0x20);
    bool crcOk = crc32(type, size_t(len) + 4) == be32(cd + len);
    pos += 12 + size_t(len);
    if (!crcOk) {
      if (critical) {
        setErr(error, strf("ошибка контрольной суммы блока %.4s", reinterpret_cast<const char*>(type)));
        return std::nullopt;
      }
      continue;  // повреждённый вспомогательный блок пропускается
    }
    if (std::memcmp(type, "IDAT", 4) == 0) {
      if (idatDone) continue;  // блоки IDAT не подряд — лишние данные игнорируются
      if (F->ct == 3 && !havePlte) { setErr(error, "нет палитры перед данными"); return std::nullopt; }
      if (idat.empty()) idat.reserve(std::min<size_t>(size, size_t(len) * 4 + 64));
      idat.insert(idat.end(), cd, cd + len);
      sawIdat = true;
      continue;
    }
    if (sawIdat) idatDone = true;
    if (std::memcmp(type, "IEND", 4) == 0) { haveIend = true; break; }
    if (std::memcmp(type, "PLTE", 4) == 0) {
      if (F->ct == 0 || F->ct == 4) continue;  // для серого палитра недопустима — игнорируем
      if (len % 3 != 0 || len == 0 || len > 768) {
        if (F->ct == 3) { setErr(error, "неверная палитра"); return std::nullopt; }
        continue;
      }
      if (F->ct == 3) {
        F->palSize = int(len / 3);
        for (int i = 0; i < F->palSize; i++) {
          F->pal[i][0] = cd[i * 3];
          F->pal[i][1] = cd[i * 3 + 1];
          F->pal[i][2] = cd[i * 3 + 2];
        }
        havePlte = true;
      }
      continue;
    }
    if (std::memcmp(type, "tRNS", 4) == 0) {
      if (F->ct == 3) {
        for (u32 i = 0; i < len && i < 256; i++) F->pal[i][3] = cd[i];
      } else if (F->ct == 0 && len >= 2) {
        F->hasKey = true;
        F->key[0] = be16(cd);
      } else if (F->ct == 2 && len >= 6) {
        F->hasKey = true;
        F->key[0] = be16(cd);
        F->key[1] = be16(cd + 2);
        F->key[2] = be16(cd + 4);
      }
      continue;
    }
    if (std::memcmp(type, "IHDR", 4) == 0) { setErr(error, "повторный заголовок IHDR"); return std::nullopt; }
    if (critical) {
      setErr(error, strf("неподдерживаемый обязательный блок %.4s", reinterpret_cast<const char*>(type)));
      return std::nullopt;
    }
  }
  (void)haveIend;  // отсутствие IEND допустимо, если данные полны
  if (idat.empty()) { setErr(error, "нет данных изображения"); return std::nullopt; }

  // ---------------------------------------------------------------- распаковка
  const int bitsPP = channelsOf(F->ct) * F->depth;
  const int bpp = std::max(1, bitsPP / 8);
  const u64 W = u64(info.w), H = u64(info.h);
  u64 rawSize = 0;
  if (!info.interlaced) {
    rawSize = H * (rowBytes(W, bitsPP) + 1);
  } else {
    for (auto& p : kAdam7) {
      u64 pw = W > u64(p.x0) ? (W - u64(p.x0) + u64(p.dx) - 1) / u64(p.dx) : 0;
      u64 ph = H > u64(p.y0) ? (H - u64(p.y0) + u64(p.dy) - 1) / u64(p.dy) : 0;
      if (pw && ph) rawSize += ph * (rowBytes(pw, bitsPP) + 1);
    }
  }
  const u64 outSize = W * H * 4;
  if (rawSize > u64(1) << 40 || outSize > u64(1) << 40) { setErr(error, "слишком большое изображение"); return std::nullopt; }

  RgbaImage img;
  img.w = info.w;
  img.h = info.h;
  std::string zerr;
  if (!info.interlaced) {
    // Один буфер: распаковка, снятие фильтров со сжатием строк, расширение до RGBA на месте.
    std::vector<u8> buf(static_cast<size_t>(std::max(rawSize, outSize)));
    auto got = inflateInto(idat, ZFormat::Zlib, std::span<u8>(buf.data(), size_t(rawSize)), &zerr, true);
    if (!got) { setErr(error, zerr); return std::nullopt; }
    if (*got != rawSize) { setErr(error, "недостаточно данных изображения"); return std::nullopt; }
    idat = std::vector<u8>();
    const size_t stride = size_t(rowBytes(W, bitsPP));
    if (!unfilterRows(buf.data(), size_t(H), stride, bpp, error)) return std::nullopt;
    const size_t w = size_t(W), h = size_t(H);
    if (F->ct == 6 && F->depth == 8) {
      // уже RGBA8
    } else if (bitsPP <= 32) {
      // снизу вверх и справа налево: выход пикселя не затирает вход предыдущих
      for (size_t y = h; y-- > 0;) convertRow<true>(*F, buf.data() + y * stride, buf.data() + y * w * 4, w);
    } else {
      // 6–8 байт на пиксель на входе: сверху вниз
      for (size_t y = 0; y < h; y++) convertRow<false>(*F, buf.data() + y * stride, buf.data() + y * w * 4, w);
    }
    buf.resize(size_t(outSize));
    img.rgba = std::move(buf);
    return img;
  }

  std::vector<u8> raw(static_cast<size_t>(rawSize));
  auto got = inflateInto(idat, ZFormat::Zlib, std::span<u8>(raw), &zerr, true);
  if (!got) { setErr(error, zerr); return std::nullopt; }
  if (*got != rawSize) { setErr(error, "недостаточно данных изображения"); return std::nullopt; }
  idat = std::vector<u8>();
  img.rgba.assign(size_t(outSize), 0);
  size_t off = 0;
  for (auto& p : kAdam7) {
    size_t pw = W > u64(p.x0) ? size_t((W - u64(p.x0) + u64(p.dx) - 1) / u64(p.dx)) : 0;
    size_t ph = H > u64(p.y0) ? size_t((H - u64(p.y0) + u64(p.dy) - 1) / u64(p.dy)) : 0;
    if (!pw || !ph) continue;
    size_t stride = size_t(rowBytes(pw, bitsPP));
    u8* pass = raw.data() + off;
    if (!unfilterRows(pass, ph, stride, bpp, error)) return std::nullopt;
    for (size_t y = 0; y < ph; y++) {
      const u8* row = pass + y * stride;
      u8* out = img.rgba.data() + ((size_t(p.y0) + y * size_t(p.dy)) * size_t(W)) * 4;
      for (size_t x = 0; x < pw; x++) pixel(*F, row, x, out + (size_t(p.x0) + x * size_t(p.dx)) * 4);
    }
    off += ph * (stride + 1);
  }
  return img;
}

// ================================================================ кодирование
namespace {

void putBe32(std::vector<u8>& o, u32 v) {
  o.push_back(u8(v >> 24));
  o.push_back(u8(v >> 16));
  o.push_back(u8(v >> 8));
  o.push_back(u8(v));
}

void chunk(std::vector<u8>& o, const char* type, const u8* data, size_t n) {
  putBe32(o, u32(n));
  size_t start = o.size();
  o.insert(o.end(), type, type + 4);
  if (n) o.insert(o.end(), data, data + n);
  putBe32(o, crc32(o.data() + start, n + 4));
}

// Фильтр строки с минимальной суммой модулей (эвристика libpng).
void filterRow(const u8* cur, const u8* prev, size_t n, size_t bpp, u8* out, std::vector<u8> (&cand)[5]) {
  u64 best = ~u64(0);
  int bestF = 0;
  for (int f = 0; f < 5; f++) {
    u8* c = cand[f].data();
    u64 sum = 0;
    switch (f) {
      case 0:
        for (size_t i = 0; i < n; i++) { c[i] = cur[i]; sum += u64(std::abs(int(i8(c[i])))); }
        break;
      case 1:
        for (size_t i = 0; i < n; i++) { c[i] = u8(cur[i] - (i >= bpp ? cur[i - bpp] : 0)); sum += u64(std::abs(int(i8(c[i])))); }
        break;
      case 2:
        for (size_t i = 0; i < n; i++) { c[i] = u8(cur[i] - prev[i]); sum += u64(std::abs(int(i8(c[i])))); }
        break;
      case 3:
        for (size_t i = 0; i < n; i++) {
          int a = i >= bpp ? cur[i - bpp] : 0;
          c[i] = u8(cur[i] - ((a + prev[i]) >> 1));
          sum += u64(std::abs(int(i8(c[i]))));
        }
        break;
      case 4:
        for (size_t i = 0; i < n; i++) {
          int a = i >= bpp ? cur[i - bpp] : 0, cc = i >= bpp ? prev[i - bpp] : 0;
          c[i] = u8(cur[i] - paeth(a, prev[i], cc));
          sum += u64(std::abs(int(i8(c[i]))));
        }
        break;
    }
    if (sum < best) {
      best = sum;
      bestF = f;
    }
  }
  out[0] = u8(bestF);
  std::memcpy(out + 1, cand[bestF].data(), n);
}

}  // namespace

std::vector<u8> encodePng(const RgbaImage& img, int level) {
  PngEncodeOptions opt;
  opt.level = level;
  return encodePng(img, opt);
}

std::vector<u8> encodePng(const RgbaImage& img, const PngEncodeOptions& opt) {
  if (img.w <= 0 || img.h <= 0 || img.rgba.size() < size_t(img.w) * size_t(img.h) * 4) return {};
  const size_t w = size_t(img.w), h = size_t(img.h), npx = w * h;
  const u8* src = img.rgba.data();

  // ---------------------------------------------------------------- анализ (один проход)
  bool opaque = true, gray = true, palOk = opt.reduce;
  u32 palColors[256];
  int palN = 0;
  // Открытая адресация: цвет -> индекс палитры (−1 — свободно).
  std::vector<u32> slotColor;
  std::vector<i16> slotIdx;
  if (opt.reduce) {
    slotColor.assign(1024, 0);
    slotIdx.assign(1024, -1);
  }
  u32 lastC = 0;
  bool haveLast = false;
  if (opt.reduce) {
    for (size_t i = 0; i < npx; i++) {
      const u8* p = src + i * 4;
      if (p[3] != 255) opaque = false;
      if (p[0] != p[1] || p[1] != p[2]) gray = false;
      if (palOk) {
        u32 c = u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
        if (haveLast && c == lastC) continue;
        lastC = c;
        haveLast = true;
        u32 hsh = (c * 2654435761u) >> 22;
        for (;;) {
          if (slotIdx[hsh] < 0) {
            if (palN == 256) { palOk = false; break; }
            slotColor[hsh] = c;
            slotIdx[hsh] = i16(palN);
            palColors[palN++] = c;
            break;
          }
          if (slotColor[hsh] == c) break;
          hsh = (hsh + 1) & 1023;
        }
      } else if (!opaque && !gray) {
        break;
      }
    }
  } else {
    opaque = gray = false;
  }

  // ---------------------------------------------------------------- выбор формата
  int ct, depth = 8;
  if (gray && opaque) {
    ct = 0;
    bool d4 = true, d2 = true, d1 = true;
    for (size_t i = 0; i < npx && d4; i++) {
      u8 v = src[i * 4];
      d4 = v % 17 == 0;
      d2 = d2 && v % 85 == 0;
      d1 = d1 && v % 255 == 0;
    }
    if (d1) depth = 1;
    else if (d2) depth = 2;
    else if (d4) depth = 4;
  } else if (palOk && palN > 0) {
    ct = 3;
    depth = palN <= 2 ? 1 : palN <= 4 ? 2 : palN <= 16 ? 4 : 8;
  } else if (gray) {
    ct = 4;
  } else if (opaque) {
    ct = 2;
  } else {
    ct = 6;
  }

  // палитра: сначала полупрозрачные (короче tRNS), затем по порядку появления
  u8 remap[256] = {};
  std::vector<u32> pal;
  if (ct == 3) {
    std::vector<int> order(static_cast<size_t>(palN));
    for (int i = 0; i < palN; i++) order[size_t(i)] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return (palColors[a] >> 24 == 255 ? 1 : 0) < (palColors[b] >> 24 == 255 ? 1 : 0); });
    for (int i = 0; i < palN; i++) {
      remap[order[size_t(i)]] = u8(i);
      pal.push_back(palColors[order[size_t(i)]]);
    }
  }
  auto palIndex = [&](const u8* p) -> u8 {
    u32 c = u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
    u32 hsh = (c * 2654435761u) >> 22;
    while (slotColor[hsh] != c || slotIdx[hsh] < 0) hsh = (hsh + 1) & 1023;
    return remap[slotIdx[hsh]];
  };

  const int ch = channelsOf(ct);
  const size_t stride = (w * size_t(ch * depth) + 7) / 8;
  const size_t bpp = std::max<size_t>(1, size_t(ch * depth) / 8);
  const bool adaptive = opt.level > 0 && ct != 3 && depth == 8;

  // ---------------------------------------------------------------- строки с фильтрами
  std::vector<u8> raw(h * (stride + 1));
  std::vector<u8> cur(stride), prev(stride, 0);
  std::vector<u8> cand[5];
  if (adaptive)
    for (auto& c : cand) c.resize(stride);
  for (size_t y = 0; y < h; y++) {
    const u8* s = src + y * w * 4;
    u8* c = cur.data();
    switch (ct) {
      case 6: std::memcpy(c, s, w * 4); break;
      case 2:
        for (size_t x = 0; x < w; x++) { c[x * 3] = s[x * 4]; c[x * 3 + 1] = s[x * 4 + 1]; c[x * 3 + 2] = s[x * 4 + 2]; }
        break;
      case 4:
        for (size_t x = 0; x < w; x++) { c[x * 2] = s[x * 4]; c[x * 2 + 1] = s[x * 4 + 3]; }
        break;
      case 0:
        if (depth == 8) {
          for (size_t x = 0; x < w; x++) c[x] = s[x * 4];
        } else {
          std::memset(c, 0, stride);
          u32 div = 255 / ((1u << depth) - 1);
          for (size_t x = 0; x < w; x++) {
            size_t bit = x * size_t(depth);
            c[bit >> 3] |= u8((s[x * 4] / div) << (8 - depth - (bit & 7)));
          }
        }
        break;
      case 3:
        if (depth == 8) {
          for (size_t x = 0; x < w; x++) c[x] = palIndex(s + x * 4);
        } else {
          std::memset(c, 0, stride);
          for (size_t x = 0; x < w; x++) {
            size_t bit = x * size_t(depth);
            c[bit >> 3] |= u8(palIndex(s + x * 4) << (8 - depth - (bit & 7)));
          }
        }
        break;
    }
    u8* out = raw.data() + y * (stride + 1);
    if (adaptive) {
      filterRow(c, prev.data(), stride, bpp, out, cand);
    } else {
      out[0] = 0;
      std::memcpy(out + 1, c, stride);
    }
    std::swap(cur, prev);
  }

  // ---------------------------------------------------------------- файл
  std::vector<u8> z = deflate(raw, std::clamp(opt.level, 0, 9), ZFormat::Zlib);
  raw = std::vector<u8>();
  std::vector<u8> out(kSig, kSig + 8);
  out.reserve(z.size() + 1024);
  u8 ihdr[13];
  ihdr[0] = u8(w >> 24); ihdr[1] = u8(w >> 16); ihdr[2] = u8(w >> 8); ihdr[3] = u8(w);
  ihdr[4] = u8(h >> 24); ihdr[5] = u8(h >> 16); ihdr[6] = u8(h >> 8); ihdr[7] = u8(h);
  ihdr[8] = u8(depth);
  ihdr[9] = u8(ct);
  ihdr[10] = ihdr[11] = ihdr[12] = 0;
  chunk(out, "IHDR", ihdr, 13);
  if (ct == 3) {
    std::vector<u8> plte, trns;
    for (u32 c : pal) {
      plte.push_back(u8(c));
      plte.push_back(u8(c >> 8));
      plte.push_back(u8(c >> 16));
      if ((c >> 24) != 255) trns.push_back(u8(c >> 24));
    }
    chunk(out, "PLTE", plte.data(), plte.size());
    if (!trns.empty()) chunk(out, "tRNS", trns.data(), trns.size());
  }
  constexpr size_t kIdatMax = size_t(1) << 20;
  for (size_t off = 0; off < z.size(); off += kIdatMax) chunk(out, "IDAT", z.data() + off, std::min(kIdatMax, z.size() - off));
  chunk(out, "IEND", nullptr, 0);
  return out;
}

bool writePngFile(const std::string& path, const RgbaImage& img, int level) {
  std::vector<u8> data = encodePng(img, level);
  if (data.empty()) return false;
  return fs::writeFileAtomic(path, std::span<const u8>(data), nullptr);
}

}  // namespace rg::codec
