// Regnum — разбор TrueType/OpenType, контуры глифов, растеризация с точной площадью покрытия, кеш растров.
#include "gfx/font.h"

#include <atomic>
#include <bit>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <unordered_map>

namespace rg::gfx {

namespace {

// ---------------------------------------------------------------- чтение big-endian с проверкой границ
struct Bytes {
  const u8* p = nullptr;
  size_t n = 0;
  Bytes() = default;
  Bytes(const u8* p_, size_t n_) : p(p_), n(n_) {}
  bool has(size_t off, size_t len) const { return off <= n && len <= n - off; }
  u8 u8at(size_t o) const { return o < n ? p[o] : 0; }
  i8 i8at(size_t o) const { return i8(u8at(o)); }
  u16 u16at(size_t o) const { return has(o, 2) ? u16((u32(p[o]) << 8) | p[o + 1]) : 0; }
  i16 i16at(size_t o) const { return i16(u16at(o)); }
  u32 u32at(size_t o) const {
    return has(o, 4) ? (u32(p[o]) << 24) | (u32(p[o + 1]) << 16) | (u32(p[o + 2]) << 8) | u32(p[o + 3]) : 0;
  }
  Bytes sub(size_t off, size_t len) const {
    if (off > n) return {};
    return {p + off, std::min(len, n - off)};
  }
  Bytes from(size_t off) const { return off > n ? Bytes{} : Bytes{p + off, n - off}; }
  bool empty() const { return n == 0; }
};

constexpr u32 tag4(const char (&s)[5]) { return (u32(u8(s[0])) << 24) | (u32(u8(s[1])) << 16) | (u32(u8(s[2])) << 8) | u32(u8(s[3])); }

float f2dot14(i16 v) { return float(v) / 16384.f; }

std::atomic<u32> gFontIds{1};

// Пределы против «бомб» в недоверенных шрифтах.
constexpr int kMaxCompositeDepth = 8;
constexpr size_t kMaxOutlinePoints = 1u << 18;
constexpr int kMaxComponents = 4096;

// Подтаблица парного позиционирования GPOS (только X-продвижение первого глифа).
// Покрытие и классы читаются из файла двоичным поиском; для небольших шрифтов строятся плотные таблицы.
struct PairSub {
  u8 format = 0;
  Bytes table, cov, cd1, cd2;
  int recSize = 0, xAdv = 0;    // формат 1: размер записи пары и смещение XAdvance в ней
  u32 c1count = 0, c2count = 0; // формат 2
  size_t rec2 = 0;              // формат 2: размер записи пары значений
  std::vector<u16> denseCov, denseC1, denseC2;  // глиф -> индекс покрытия + 1 / класс (пусто — без таблиц)

  static u32 coverageRaw(Bytes c, u16 g) {
    u16 fmt = c.u16at(0);
    u32 n = c.u16at(2);
    if (fmt == 1) {
      n = u32(std::min<size_t>(n, c.n >= 4 ? (c.n - 4) / 2 : 0));
      u32 lo = 0, hi = n;
      while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        u16 v = c.u16at(4 + 2 * size_t(mid));
        if (v < g) lo = mid + 1;
        else if (v > g) hi = mid;
        else return mid + 1;
      }
    } else if (fmt == 2) {
      n = u32(std::min<size_t>(n, c.n >= 4 ? (c.n - 4) / 6 : 0));
      u32 lo = 0, hi = n;
      while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        size_t o = 4 + 6 * size_t(mid);
        u16 s = c.u16at(o), e = c.u16at(o + 2);
        if (g < s) hi = mid;
        else if (g > e) lo = mid + 1;
        else return u32(c.u16at(o + 4)) + (g - s) + 1;
      }
    }
    return 0;
  }
  static u32 classRaw(Bytes c, u16 g) {
    u16 fmt = c.u16at(0);
    if (fmt == 1) {
      u32 start = c.u16at(2), count = c.u16at(4);
      return g >= start && g - start < count ? c.u16at(6 + 2 * size_t(g - start)) : 0;
    }
    if (fmt == 2) {
      u32 n = c.u16at(2);
      n = u32(std::min<size_t>(n, c.n >= 4 ? (c.n - 4) / 6 : 0));
      u32 lo = 0, hi = n;
      while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        size_t o = 4 + 6 * size_t(mid);
        u16 s = c.u16at(o), e = c.u16at(o + 2);
        if (g < s) hi = mid;
        else if (g > e) lo = mid + 1;
        else return c.u16at(o + 4);
      }
    }
    return 0;
  }
  u32 coverage(u16 g) const { return denseCov.empty() ? coverageRaw(cov, g) : (g < denseCov.size() ? denseCov[g] : 0); }
  u32 class1(u16 g) const { return denseC1.empty() ? classRaw(cd1, g) : (g < denseC1.size() ? denseC1[g] : 0); }
  u32 class2(u16 g) const { return denseC2.empty() ? classRaw(cd2, g) : (g < denseC2.size() ? denseC2[g] : 0); }

  // Поправка для пары; false — подтаблица к паре не применяется.
  bool lookup(u16 l, u16 r, int& value) const {
    u32 ci = coverage(l);
    if (!ci) return false;
    if (format == 2) {
      u32 k1 = class1(l), k2 = class2(r);
      if (k1 >= c1count || k2 >= c2count) return false;
      value += table.i16at(16 + (size_t(k1) * c2count + k2) * rec2 + size_t(xAdv));
      return true;
    }
    size_t pairSets = table.u16at(8);
    if (ci - 1 >= pairSets) return false;
    Bytes ps = table.from(table.u16at(10 + 2 * size_t(ci - 1)));
    size_t rec = size_t(recSize);
    u32 n = u32(std::min<size_t>(ps.u16at(0), ps.n >= 2 ? (ps.n - 2) / rec : 0));
    u32 lo = 0, hi = n;
    while (lo < hi) {
      u32 mid = (lo + hi) / 2;
      u16 g2 = ps.u16at(2 + mid * rec);
      if (g2 < r) lo = mid + 1;
      else if (g2 > r) hi = mid;
      else { value += ps.i16at(2 + mid * rec + 2 + size_t(xAdv)); return true; }
    }
    return false;
  }
};
struct PairLookup { std::vector<PairSub> subs; };

// Суммарная длина диапазонов (формат 2 покрытия и классов): заполнение по диапазонам выгодно, только если
// диапазоны не перекрываются многократно (защита от «бомб» в недоверенном шрифте).
size_t rangeWork(Bytes c, int numGlyphs) {
  u32 count = c.u16at(2);
  size_t total = 0;
  for (u32 i = 0; i < count && c.has(4 + 6 * size_t(i), 6); i++) {
    u32 s = c.u16at(4 + 6 * size_t(i)), e = std::min<u32>(c.u16at(6 + 6 * size_t(i)), u32(numGlyphs) - 1);
    if (e >= s) total += e - s + 1;
  }
  return total;
}

void denseCoverage(Bytes c, int numGlyphs, std::vector<u16>& out) {
  out.assign(size_t(numGlyphs), 0);
  u16 fmt = c.u16at(0);
  if (fmt == 1) {
    u32 count = c.u16at(2);
    for (u32 i = 0; i < count && i < 0xFFFF && c.has(4 + 2 * size_t(i), 2); i++) {
      u16 g = c.u16at(4 + 2 * size_t(i));
      if (g < numGlyphs && !out[g]) out[g] = u16(i + 1);
    }
  } else if (fmt == 2 && rangeWork(c, numGlyphs) <= 2 * size_t(numGlyphs)) {
    u32 count = c.u16at(2);
    for (u32 i = 0; i < count && c.has(4 + 6 * size_t(i), 6); i++) {
      u32 s = c.u16at(4 + 6 * size_t(i)), e = c.u16at(6 + 6 * size_t(i)), ci = c.u16at(8 + 6 * size_t(i));
      for (u32 g = s; g <= e && g < u32(numGlyphs); g++) out[g] = u16(std::min<u32>(ci + (g - s) + 1, 0xFFFF));
    }
  } else if (fmt == 2) {
    for (int g = 0; g < numGlyphs; g++) out[size_t(g)] = u16(std::min<u32>(PairSub::coverageRaw(c, u16(g)), 0xFFFF));
  }
}

void denseClass(Bytes c, int numGlyphs, std::vector<u16>& out) {
  out.assign(size_t(numGlyphs), 0);
  u16 fmt = c.u16at(0);
  if (fmt == 1) {
    u32 start = c.u16at(2), count = c.u16at(4);
    for (u32 i = 0; i < count && c.has(6 + 2 * size_t(i), 2); i++)
      if (start + i < u32(numGlyphs)) out[start + i] = c.u16at(6 + 2 * size_t(i));
  } else if (fmt == 2 && rangeWork(c, numGlyphs) <= 2 * size_t(numGlyphs)) {
    u32 count = c.u16at(2);
    for (u32 i = 0; i < count && c.has(4 + 6 * size_t(i), 6); i++) {
      u32 s = c.u16at(4 + 6 * size_t(i)), e = c.u16at(6 + 6 * size_t(i));
      u16 k = c.u16at(8 + 6 * size_t(i));
      for (u32 g = s; g <= e && g < u32(numGlyphs); g++) out[g] = k;
    }
  } else if (fmt == 2) {
    for (int g = 0; g < numGlyphs; g++) out[size_t(g)] = u16(PairSub::classRaw(c, u16(g)));
  }
}

bool compilePairSub(Bytes st, int numGlyphs, PairSub& ps, size_t& denseBudget) {
  u16 fmt = st.u16at(0);
  u16 vf1 = st.u16at(4), vf2 = st.u16at(6);
  if (!(vf1 & 0x0004)) return false;  // нет XAdvance первого глифа
  int sz1 = 2 * std::popcount(unsigned(vf1 & 0xFF)), sz2 = 2 * std::popcount(unsigned(vf2 & 0xFF));
  ps.xAdv = 2 * std::popcount(unsigned(vf1 & 0x3));
  ps.table = st;
  ps.cov = st.from(st.u16at(2));
  size_t one = size_t(numGlyphs) * sizeof(u16);
  if (fmt == 1) {
    ps.format = 1;
    ps.recSize = 2 + sz1 + sz2;
    if (!st.has(10, size_t(st.u16at(8)) * 2)) return false;
  } else if (fmt == 2) {
    ps.format = 2;
    ps.c1count = st.u16at(12);
    ps.c2count = st.u16at(14);
    ps.rec2 = size_t(sz1 + sz2);
    if (ps.c1count == 0 || ps.c2count == 0) return false;
    if (!st.has(16, size_t(ps.c1count) * ps.c2count * ps.rec2)) return false;
    ps.cd1 = st.from(st.u16at(8));
    ps.cd2 = st.from(st.u16at(10));
    if (denseBudget >= 2 * one) {
      denseClass(ps.cd1, numGlyphs, ps.denseC1);
      denseClass(ps.cd2, numGlyphs, ps.denseC2);
      denseBudget -= 2 * one;
    }
  } else {
    return false;
  }
  if (denseBudget >= one) {
    denseCoverage(ps.cov, numGlyphs, ps.denseCov);
    denseBudget -= one;
  }
  return true;
}

void parseGposKern(Bytes t, int numGlyphs, std::vector<PairLookup>& out) {
  if (t.n < 10 || t.u16at(0) != 1 || numGlyphs <= 0) return;
  Bytes features = t.from(t.u16at(6));
  Bytes lookups = t.from(t.u16at(8));
  std::vector<u16> idx;
  u32 fc = features.u16at(0);
  for (u32 i = 0; i < fc && features.has(2 + 6 * size_t(i), 6); i++) {
    if (features.u32at(2 + 6 * size_t(i)) != tag4("kern")) continue;
    Bytes f = features.from(features.u16at(6 + 6 * size_t(i)));
    u32 n = f.u16at(2);
    for (u32 j = 0; j < n && f.has(4 + 2 * size_t(j), 2); j++) idx.push_back(f.u16at(4 + 2 * size_t(j)));
  }
  std::sort(idx.begin(), idx.end());
  idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
  u32 lc = lookups.u16at(0);
  int budget = 512;                  // предел числа подтаблиц
  size_t denseBudget = 4u << 20;     // предел памяти плотных таблиц на шрифт
  for (u16 li : idx) {
    if (li >= lc) continue;
    Bytes lk = lookups.from(lookups.u16at(2 + 2 * size_t(li)));
    u16 type = lk.u16at(0);
    u32 sc = lk.u16at(4);
    PairLookup pl;
    for (u32 s = 0; s < sc && lk.has(6 + 2 * size_t(s), 2) && budget > 0; s++) {
      Bytes st = lk.from(lk.u16at(6 + 2 * size_t(s)));
      u16 stype = type;
      if (stype == 9) {
        if (st.u16at(0) != 1) continue;
        stype = st.u16at(2);
        st = st.from(st.u32at(4));
      }
      if (stype != 2) continue;
      PairSub ps;
      if (compilePairSub(st, numGlyphs, ps, denseBudget)) { pl.subs.push_back(std::move(ps)); budget--; }
    }
    if (!pl.subs.empty()) out.push_back(std::move(pl));
  }
}

// Таблица kern (формат 0): пары (левый << 16 | правый) -> значение.
void parseKern(Bytes t, std::vector<std::pair<u32, i16>>& pairs) {
  auto readPairs = [&](Bytes d) {
    u32 n = d.u16at(0);
    size_t avail = d.n >= 8 ? (d.n - 8) / 6 : 0;
    n = u32(std::min<size_t>(n, avail));
    for (u32 i = 0; i < n; i++) {
      size_t o = 8 + size_t(i) * 6;
      pairs.push_back({d.u32at(o), d.i16at(o + 4)});
    }
  };
  if (t.u16at(0) == 0) {
    u32 n = t.u16at(2);
    size_t off = 4;
    for (u32 i = 0; i < n && t.has(off, 6); i++) {
      u32 len = t.u16at(off + 2);
      u16 cov = t.u16at(off + 4);
      bool horizontal = cov & 1, minimum = cov & 2, cross = cov & 4;
      if ((cov >> 8) == 0 && horizontal && !minimum && !cross) readPairs(t.from(off + 6));
      if (len < 6 || n == 1) break;
      off += len;
    }
  } else if (t.u32at(0) == 0x00010000) {
    u32 n = t.u32at(4);
    size_t off = 8;
    for (u32 i = 0; i < n && i < 64 && t.has(off, 8); i++) {
      u32 len = t.u32at(off);
      u16 cov = t.u16at(off + 4);
      bool vertical = cov & 0x8000, cross = cov & 0x4000, variation = cov & 0x2000;
      if ((cov & 0xFF) == 0 && !vertical && !cross && !variation) readPairs(t.from(off + 8));
      if (len < 8) break;
      off += len;
    }
  }
  std::sort(pairs.begin(), pairs.end(), [](auto& a, auto& b) { return a.first < b.first; });
  // Слияние повторов (суммирование).
  size_t w = 0;
  for (size_t i = 0; i < pairs.size(); i++) {
    if (w > 0 && pairs[w - 1].first == pairs[i].first) pairs[w - 1].second = i16(pairs[w - 1].second + pairs[i].second);
    else pairs[w++] = pairs[i];
  }
  pairs.resize(w);
}

std::string utf16beToUtf8(Bytes b) {
  std::string s;
  for (size_t i = 0; i + 1 < b.n; i += 2) {
    u32 c = b.u16at(i);
    if (c >= 0xD800 && c < 0xDC00 && i + 3 < b.n) {
      u32 c2 = b.u16at(i + 2);
      if (c2 >= 0xDC00 && c2 < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00); i += 2; }
      else c = 0xFFFD;
    } else if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
    if (c) utf8::append(s, c);
  }
  return s;
}

std::string macRomanToUtf8(Bytes b) {
  std::string s;
  for (size_t i = 0; i < b.n; i++) {
    u8 c = b.p[i];
    if (c >= 32 && c < 127) s.push_back(char(c));
    else if (c >= 128) utf8::append(s, 0xFFFD);
  }
  return s;
}

}  // namespace

// ================================================================ Font::Impl
struct Font::Impl {
  std::shared_ptr<const std::string> data;
  Bytes file;
  Bytes head, hhea, hmtx, maxp, loca, glyf, cmap, kern, gpos, os2, name, post, fvar;
  u32 id = 0;
  int numH = 0;
  int locFormat = 0;
  // cmap
  Bytes cmapSub;
  int cmapFormat = 0;
  bool symbolCmap = false;
  std::vector<u16> fast;  // U+0000..U+04FF -> глиф
  // кернинг
  std::vector<PairLookup> gposKern;
  std::vector<std::pair<u32, i16>> kernPairs;

  Bytes glyphData(u16 g, int numGlyphs) const {
    if (g >= numGlyphs) return {};
    size_t a, b;
    if (locFormat == 0) { a = size_t(loca.u16at(size_t(g) * 2)) * 2; b = size_t(loca.u16at(size_t(g) * 2 + 2)) * 2; }
    else { a = loca.u32at(size_t(g) * 4); b = loca.u32at(size_t(g) * 4 + 4); }
    if (b <= a || a >= glyf.n) return {};
    return glyf.sub(a, b - a);
  }

  u16 cmapLookup(u32 cp) const {
    Bytes t = cmapSub;
    switch (cmapFormat) {
      case 0: return cp < 256 ? t.u8at(6 + cp) : 0;
      case 6: {
        u32 first = t.u16at(6), count = t.u16at(8);
        if (cp < first || cp - first >= count) return 0;
        return t.u16at(10 + 2 * (cp - first));
      }
      case 4: {
        if (cp > 0xFFFF) return 0;
        u32 segX2 = t.u16at(6);
        u32 seg = segX2 / 2;
        if (seg == 0) return 0;
        size_t ends = 14, starts = 16 + segX2, deltas = 16 + 2 * size_t(segX2), ranges = 16 + 3 * size_t(segX2);
        // Первый сегмент с endCode >= cp.
        u32 lo = 0, hi = seg;
        while (lo < hi) {
          u32 mid = (lo + hi) / 2;
          if (t.u16at(ends + 2 * mid) < cp) lo = mid + 1;
          else hi = mid;
        }
        if (lo >= seg) return 0;
        u32 start = t.u16at(starts + 2 * lo);
        if (cp < start) return 0;
        u16 delta = t.u16at(deltas + 2 * lo);
        u32 ro = t.u16at(ranges + 2 * lo);
        if (ro == 0) return u16(cp + delta);
        size_t at = ranges + 2 * size_t(lo) + ro + 2 * size_t(cp - start);
        u16 g = t.u16at(at);
        return g ? u16(g + delta) : 0;
      }
      case 12: {
        u32 n = t.u32at(12);
        n = u32(std::min<size_t>(n, t.n >= 16 ? (t.n - 16) / 12 : 0));
        u32 lo = 0, hi = n;
        while (lo < hi) {
          u32 mid = (lo + hi) / 2;
          size_t o = 16 + size_t(mid) * 12;
          u32 s = t.u32at(o), e = t.u32at(o + 4);
          if (cp < s) hi = mid;
          else if (cp > e) lo = mid + 1;
          else {
            u32 g = t.u32at(o + 8) + (cp - s);
            return g <= 0xFFFF ? u16(g) : 0;
          }
        }
        return 0;
      }
      default: return 0;
    }
  }

  // Исходный контур TrueType (без нормализации) — для сопоставления точек составных глифов.
  bool rawOutline(u16 g, int numGlyphs, GlyphOutline& out, int depth, int& budget) const;
};

bool Font::Impl::rawOutline(u16 g, int numGlyphs, GlyphOutline& out, int depth, int& budget) const {
  if (depth > kMaxCompositeDepth || --budget < 0) return false;
  Bytes d = glyphData(g, numGlyphs);
  if (d.n < 10) return true;  // пустой глиф (пробел)
  int nc = d.i16at(0);
  if (nc >= 0) {
    if (nc == 0) return true;
    if (!d.has(10, size_t(nc) * 2 + 2)) return false;
    std::vector<u16> ends(static_cast<size_t>(nc));
    int prev = -1;
    for (int i = 0; i < nc; i++) {
      ends[size_t(i)] = d.u16at(10 + 2 * size_t(i));
      if (int(ends[size_t(i)]) < prev) return false;
      prev = ends[size_t(i)];
    }
    size_t np = size_t(ends.back()) + 1;
    if (out.pts.size() + np > kMaxOutlinePoints) return false;
    size_t off = 10 + size_t(nc) * 2;
    u32 instr = d.u16at(off);
    off += 2 + instr;
    // Флаги с повторами.
    std::vector<u8> flags(np);
    for (size_t i = 0; i < np;) {
      if (!d.has(off, 1)) return false;
      u8 f = d.u8at(off++);
      flags[i++] = f;
      if (f & 0x08) {
        if (!d.has(off, 1)) return false;
        u32 rep = d.u8at(off++);
        while (rep-- && i < np) flags[i++] = f;
      }
    }
    std::vector<i32> xs(np), ys(np);
    i32 v = 0;
    for (size_t i = 0; i < np; i++) {
      u8 f = flags[i];
      if (f & 0x02) {
        if (!d.has(off, 1)) return false;
        i32 dx = d.u8at(off++);
        v += (f & 0x10) ? dx : -dx;
      } else if (!(f & 0x10)) {
        if (!d.has(off, 2)) return false;
        v += d.i16at(off);
        off += 2;
      }
      xs[i] = v;
    }
    v = 0;
    for (size_t i = 0; i < np; i++) {
      u8 f = flags[i];
      if (f & 0x04) {
        if (!d.has(off, 1)) return false;
        i32 dy = d.u8at(off++);
        v += (f & 0x20) ? dy : -dy;
      } else if (!(f & 0x20)) {
        if (!d.has(off, 2)) return false;
        v += d.i16at(off);
        off += 2;
      }
      ys[i] = v;
    }
    u32 base = u32(out.pts.size());
    for (size_t i = 0; i < np; i++) {
      out.pts.push_back({float(xs[i]), float(ys[i])});
      out.onCurve.push_back(flags[i] & 1);
    }
    for (u16 e : ends) out.contourEnds.push_back(base + e);
    return true;
  }
  // Составной глиф.
  size_t off = 10;
  for (int comp = 0; comp < kMaxComponents; comp++) {
    if (!d.has(off, 4)) return false;
    u16 flags = d.u16at(off), child = d.u16at(off + 2);
    off += 4;
    i32 a1, a2;
    if (flags & 0x0001) {
      if (!d.has(off, 4)) return false;
      if (flags & 0x0002) { a1 = d.i16at(off); a2 = d.i16at(off + 2); }
      else { a1 = d.u16at(off); a2 = d.u16at(off + 2); }
      off += 4;
    } else {
      if (!d.has(off, 2)) return false;
      if (flags & 0x0002) { a1 = d.i8at(off); a2 = d.i8at(off + 1); }
      else { a1 = d.u8at(off); a2 = d.u8at(off + 1); }
      off += 2;
    }
    Affine m;
    if (flags & 0x0008) {
      if (!d.has(off, 2)) return false;
      m.a = m.d = f2dot14(d.i16at(off));
      off += 2;
    } else if (flags & 0x0040) {
      if (!d.has(off, 4)) return false;
      m.a = f2dot14(d.i16at(off));
      m.d = f2dot14(d.i16at(off + 2));
      off += 4;
    } else if (flags & 0x0080) {
      if (!d.has(off, 8)) return false;
      m.a = f2dot14(d.i16at(off));
      m.b = f2dot14(d.i16at(off + 2));
      m.c = f2dot14(d.i16at(off + 4));
      m.d = f2dot14(d.i16at(off + 6));
      off += 8;
    }
    GlyphOutline part;
    if (!rawOutline(child, numGlyphs, part, depth + 1, budget)) return false;
    for (auto& p : part.pts) p = m.applyVec(p);
    float dx = 0, dy = 0;
    if (flags & 0x0002) {
      dx = float(a1);
      dy = float(a2);
      if ((flags & 0x0800) && !(flags & 0x1000)) {
        Pt t = m.applyVec({dx, dy});
        dx = t.x;
        dy = t.y;
      }
    } else {
      // Сопоставление точек: точка a1 родителя совмещается с точкой a2 компонента.
      if (a1 < 0 || a2 < 0 || size_t(a1) >= out.pts.size() || size_t(a2) >= part.pts.size()) return false;
      dx = out.pts[size_t(a1)].x - part.pts[size_t(a2)].x;
      dy = out.pts[size_t(a1)].y - part.pts[size_t(a2)].y;
    }
    if (out.pts.size() + part.pts.size() > kMaxOutlinePoints) return false;
    u32 base = u32(out.pts.size());
    for (size_t i = 0; i < part.pts.size(); i++) {
      out.pts.push_back({part.pts[i].x + dx, part.pts[i].y + dy});
      out.onCurve.push_back(part.onCurve[i]);
    }
    for (u32 e : part.contourEnds) out.contourEnds.push_back(base + e);
    if (!(flags & 0x0020)) break;
  }
  return true;
}

// Нормализация: каждый контур начинается точкой на кривой, подразумеваемые середины вставлены.
static void normalizeOutline(const GlyphOutline& raw, GlyphOutline& out) {
  out.clear();
  out.pts.reserve(raw.pts.size() + raw.pts.size() / 2);
  out.onCurve.reserve(out.pts.capacity());
  size_t start = 0;
  for (u32 endIdx : raw.contourEnds) {
    size_t end = size_t(endIdx);
    if (end < start || end >= raw.pts.size()) { start = end + 1; continue; }
    size_t m = end - start + 1;
    const Pt* q = raw.pts.data() + start;
    const u8* on = raw.onCurve.data() + start;
    if (m < 2) { start = end + 1; continue; }
    size_t first = m;
    for (size_t i = 0; i < m; i++) if (on[i]) { first = i; break; }
    size_t c0 = out.pts.size();
    auto emit = [&](Pt p, bool isOn) {
      if (!isOn && !out.onCurve.empty() && out.pts.size() > c0 && !out.onCurve.back()) {
        Pt prev = out.pts.back();
        out.pts.push_back({(prev.x + p.x) * 0.5f, (prev.y + p.y) * 0.5f});
        out.onCurve.push_back(1);
      }
      out.pts.push_back(p);
      out.onCurve.push_back(isOn ? 1 : 0);
    };
    if (first == m) {
      // Нет точек на кривой: начинаем с середины между последней и первой.
      out.pts.push_back({(q[m - 1].x + q[0].x) * 0.5f, (q[m - 1].y + q[0].y) * 0.5f});
      out.onCurve.push_back(1);
      for (size_t k = 0; k < m; k++) emit(q[k], false);
    } else {
      out.pts.push_back(q[first]);
      out.onCurve.push_back(1);
      for (size_t k = 1; k < m; k++) {
        size_t i = (first + k) % m;
        emit(q[i], on[i] != 0);
      }
    }
    out.contourEnds.push_back(u32(out.pts.size() - 1));
    start = end + 1;
  }
}

// ================================================================ Font
Font::Font() : d_(std::make_unique<Impl>()) {}
Font::~Font() = default;
u32 Font::uid() const { return d_->id; }
std::string_view Font::data() const { return d_->data ? std::string_view(*d_->data) : std::string_view(); }

int Font::faceCount(std::string_view data) {
  Bytes b(reinterpret_cast<const u8*>(data.data()), data.size());
  u32 v = b.u32at(0);
  if (v == tag4("ttcf")) {
    u32 n = b.u32at(8);
    if (n == 0 || !b.has(12, size_t(n) * 4)) return 0;
    return int(std::min<u32>(n, 1024));
  }
  if (v == 0x00010000 || v == tag4("true") || v == tag4("OTTO")) return 1;
  return 0;
}

std::shared_ptr<const Font> Font::loadFile(const std::string& utf8Path, int faceIndex, std::string* error) {
  std::filesystem::path p(reinterpret_cast<const char8_t*>(utf8Path.c_str()));
  std::ifstream f(p, std::ios::binary);
  if (!f) {
    if (error) *error = "не удалось открыть файл шрифта";
    return nullptr;
  }
  auto data = std::make_shared<std::string>();
  f.seekg(0, std::ios::end);
  std::streamoff sz = f.tellg();
  if (sz <= 0 || sz > (std::streamoff(256) << 20)) {
    if (error) *error = "недопустимый размер файла шрифта";
    return nullptr;
  }
  f.seekg(0, std::ios::beg);
  data->resize(size_t(sz));
  f.read(data->data(), sz);
  if (!f) {
    if (error) *error = "ошибка чтения файла шрифта";
    return nullptr;
  }
  return load(std::move(data), faceIndex, error);
}

std::shared_ptr<const Font> Font::load(std::shared_ptr<const std::string> data, int faceIndex, std::string* error) {
  auto bad = [&](const char* msg) -> std::shared_ptr<const Font> {
    if (error) *error = msg;
    return nullptr;
  };
  if (!data) return bad("нет данных шрифта");
  Bytes file(reinterpret_cast<const u8*>(data->data()), data->size());
  size_t faceOff = 0;
  u32 v = file.u32at(0);
  if (v == tag4("ttcf")) {
    u32 n = file.u32at(8);
    if (faceIndex < 0 || u32(faceIndex) >= n || !file.has(12 + size_t(faceIndex) * 4, 4)) return bad("нет такого начертания в коллекции");
    faceOff = file.u32at(12 + size_t(faceIndex) * 4);
    v = file.u32at(faceOff);
  } else if (faceIndex != 0) {
    return bad("нет такого начертания в файле");
  }
  if (v != 0x00010000 && v != tag4("true") && v != tag4("OTTO")) return bad("неизвестный формат шрифта");
  u32 numTables = file.u16at(faceOff + 4);
  if (!file.has(faceOff + 12, size_t(numTables) * 16)) return bad("повреждён каталог таблиц");

  std::shared_ptr<Font> font(new Font());
  Impl& d = *font->d_;
  d.data = data;
  d.file = file;
  d.id = gFontIds.fetch_add(1);
  bool hasCff = false;
  for (u32 i = 0; i < numTables; i++) {
    size_t r = faceOff + 12 + size_t(i) * 16;
    u32 tg = file.u32at(r), off = file.u32at(r + 8), len = file.u32at(r + 12);
    if (!file.has(off, 0)) continue;
    Bytes t = file.sub(off, len);
    if (tg == tag4("head")) d.head = t;
    else if (tg == tag4("hhea")) d.hhea = t;
    else if (tg == tag4("hmtx")) d.hmtx = t;
    else if (tg == tag4("maxp")) d.maxp = t;
    else if (tg == tag4("loca")) d.loca = t;
    else if (tg == tag4("glyf")) d.glyf = t;
    else if (tg == tag4("cmap")) d.cmap = t;
    else if (tg == tag4("kern")) d.kern = t;
    else if (tg == tag4("GPOS")) d.gpos = t;
    else if (tg == tag4("OS/2")) d.os2 = t;
    else if (tg == tag4("name")) d.name = t;
    else if (tg == tag4("post")) d.post = t;
    else if (tg == tag4("fvar")) d.fvar = t;
    else if (tg == tag4("CFF ") || tg == tag4("CFF2")) hasCff = true;
  }
  if (d.glyf.empty() || d.loca.empty()) return bad(hasCff ? "шрифт с контурами CFF не поддерживается" : "в шрифте нет контуров glyf");
  if (d.head.n < 54 || d.hhea.n < 36 || d.maxp.n < 6 || d.cmap.n < 4) return bad("в шрифте нет обязательных таблиц");

  Font& f = *font;
  f.upem_ = d.head.u16at(18);
  if (f.upem_ < 16 || f.upem_ > 16384) return bad("недопустимое число единиц на кегль");
  d.locFormat = d.head.i16at(50);
  if (d.locFormat != 0 && d.locFormat != 1) return bad("неизвестный формат loca");
  f.numGlyphs_ = d.maxp.u16at(4);
  // loca должна вмещать numGlyphs + 1 смещений.
  size_t locaEntry = d.locFormat == 0 ? 2 : 4;
  size_t maxByLoca = d.loca.n / locaEntry;
  if (maxByLoca == 0) return bad("повреждена таблица loca");
  f.numGlyphs_ = int(std::min<size_t>(size_t(f.numGlyphs_), maxByLoca - 1));
  if (f.numGlyphs_ <= 0) return bad("в шрифте нет глифов");
  d.numH = d.hhea.u16at(34);
  d.numH = int(std::min<size_t>(size_t(d.numH), d.hmtx.n / 4));
  if (d.numH <= 0) return bad("повреждена таблица hmtx");

  // Метрики.
  int hAsc = d.hhea.i16at(4), hDesc = d.hhea.i16at(6), hGap = d.hhea.i16at(8);
  f.ascender_ = hAsc;
  f.descender_ = hDesc;
  f.lineGap_ = hGap;
  if (d.os2.n >= 78) {
    u16 ver = d.os2.u16at(0);
    f.weightClass_ = d.os2.u16at(4);
    if (f.weightClass_ < 1 || f.weightClass_ > 1000) f.weightClass_ = 400;
    f.widthClass_ = d.os2.u16at(6);
    if (f.widthClass_ < 1 || f.widthClass_ > 9) f.widthClass_ = 5;
    u16 sel = d.os2.u16at(62);
    f.italic_ = (sel & 0x0001) || (sel & 0x0200);
    int tAsc = d.os2.i16at(68), tDesc = d.os2.i16at(70), tGap = d.os2.i16at(72);
    int wAsc = d.os2.u16at(74), wDesc = d.os2.u16at(76);
    if ((sel & 0x0080) && (tAsc != 0 || tDesc != 0)) {
      f.ascender_ = tAsc;
      f.descender_ = tDesc;
      f.lineGap_ = tGap;
    } else if (hAsc == 0 && hDesc == 0) {
      if (wAsc || wDesc) { f.ascender_ = wAsc; f.descender_ = -wDesc; f.lineGap_ = 0; }
      else { f.ascender_ = tAsc; f.descender_ = tDesc; f.lineGap_ = tGap; }
    }
    if (ver >= 2 && d.os2.n >= 90) {
      int xh = d.os2.i16at(86), ch = d.os2.i16at(88);
      if (xh > 0) f.xHeight_ = xh; else f.xHeight_ = 0;
      if (ch > 0) f.capHeight_ = ch; else f.capHeight_ = 0;
    } else {
      f.xHeight_ = f.capHeight_ = 0;
    }
  } else {
    f.xHeight_ = f.capHeight_ = 0;
  }
  if (d.head.u16at(44) & 0x0002) f.italic_ = true;
  if (f.ascender_ <= 0) f.ascender_ = f.upem_ * 4 / 5;
  if (f.descender_ > 0) f.descender_ = -f.descender_;
  if (f.descender_ == 0) f.descender_ = -f.upem_ / 5;
  if (f.lineGap_ < 0) f.lineGap_ = 0;
  f.monospace_ = d.post.n >= 16 && d.post.u32at(12) != 0;
  f.variable_ = !d.fvar.empty();

  // cmap: предпочтение полного Юникода (формат 12), затем BMP (формат 4), затем прочие.
  {
    Bytes c = d.cmap;
    u32 n = c.u16at(2);
    int bestScore = -1;
    for (u32 i = 0; i < n && c.has(4 + 8 * i, 8); i++) {
      u16 pid = c.u16at(4 + 8 * i), eid = c.u16at(6 + 8 * i);
      u32 off = c.u32at(8 + 8 * i);
      Bytes st = c.from(off);
      u16 fmt = st.u16at(0);
      int score = -1;
      bool sym = false;
      bool unicode = pid == 0 || (pid == 3 && (eid == 1 || eid == 10));
      if (pid == 3 && eid == 0) sym = true;
      if (fmt == 12 && unicode) score = 10;
      else if (fmt == 4 && unicode) score = 8;
      else if (fmt == 6 && unicode) score = 5;
      else if (fmt == 4 && sym) score = 4;
      else if (fmt == 0 && unicode) score = 3;
      else if (fmt == 0 && pid == 1 && eid == 0) score = 1;
      if (score < 0) continue;
      // Проверка минимального размера подтаблицы.
      size_t need = fmt == 12 ? 16 : fmt == 4 ? 14 : fmt == 6 ? 10 : 262;
      if (!st.has(0, need)) continue;
      if (score > bestScore) {
        bestScore = score;
        d.cmapSub = st;
        d.cmapFormat = fmt;
        d.symbolCmap = sym;
      }
    }
    if (bestScore < 0) return bad("в шрифте нет таблицы символов Юникода");
    if (d.cmapFormat == 4) {
      u32 segX2 = d.cmapSub.u16at(6);
      if (segX2 & 1) return bad("повреждена таблица cmap");
    }
    d.fast.assign(0x500, 0);
    for (u32 cp = 0; cp < 0x500; cp++) {
      u32 q = cp;
      if (d.symbolCmap && cp >= 0x20 && cp < 0x100) q = 0xF000 + cp;
      u16 g = d.cmapLookup(q);
      d.fast[cp] = g < f.numGlyphs_ ? g : 0;
    }
  }

  // Имена.
  if (d.name.n >= 6) {
    Bytes nm = d.name;
    u32 count = nm.u16at(2);
    size_t strOff = nm.u16at(4);
    std::string best[18];
    int bestRank[18];
    for (int& r : bestRank) r = -1;
    for (u32 i = 0; i < count && nm.has(6 + 12 * i, 12); i++) {
      size_t r = 6 + 12 * size_t(i);
      u16 pid = nm.u16at(r), eid = nm.u16at(r + 2), lang = nm.u16at(r + 4), nid = nm.u16at(r + 6);
      u16 len = nm.u16at(r + 8), off = nm.u16at(r + 10);
      if (nid >= 18) continue;
      int rank = -1;
      if (pid == 3 && (eid == 1 || eid == 10)) rank = lang == 0x0409 ? 4 : 3;
      else if (pid == 0) rank = 2;
      else if (pid == 1 && eid == 0) rank = lang == 0 ? 1 : 0;
      if (rank <= bestRank[nid]) continue;
      Bytes s = nm.sub(strOff + off, len);
      std::string text = pid == 1 ? macRomanToUtf8(s) : utf16beToUtf8(s);
      if (text.empty()) continue;
      bestRank[nid] = rank;
      best[nid] = std::move(text);
    }
    f.family_ = !best[16].empty() ? best[16] : best[1];
    f.subfamily_ = !best[17].empty() ? best[17] : best[2];
    f.fullName_ = !best[4].empty() ? best[4] : (f.family_ + (f.subfamily_.empty() ? "" : " " + f.subfamily_));
  }

  // Кернинг: GPOS 'kern', иначе таблица kern.
  if (!d.gpos.empty()) parseGposKern(d.gpos, f.numGlyphs_, d.gposKern);
  if (d.gposKern.empty() && !d.kern.empty()) parseKern(d.kern, d.kernPairs);

  // Высоты прописных и строчных по контурам, если OS/2 их не дала.
  auto glyphTop = [&](u32 cp) -> int {
    u16 g = f.glyphIndex(cp);
    Bytes gd = d.glyphData(g, f.numGlyphs_);
    return gd.n >= 10 ? gd.i16at(8) : 0;
  };
  if (f.capHeight_ <= 0) { int h = glyphTop('H'); f.capHeight_ = h > 0 ? h : f.upem_ * 7 / 10; }
  if (f.xHeight_ <= 0) { int h = glyphTop('x'); f.xHeight_ = h > 0 ? h : f.upem_ / 2; }
  return font;
}

u16 Font::glyphIndex(u32 cp) const {
  if (cp < 0x500) return d_->fast[cp];
  if (cp > 0x10FFFF) return 0;
  u16 g = d_->cmapLookup(cp);
  return g < numGlyphs_ ? g : 0;
}

int Font::advance(u16 g) const {
  const Impl& d = *d_;
  int i = g < d.numH ? g : d.numH - 1;
  return d.hmtx.u16at(size_t(i) * 4);
}

int Font::leftSideBearing(u16 g) const {
  const Impl& d = *d_;
  if (g < d.numH) return d.hmtx.i16at(size_t(g) * 4 + 2);
  return d.hmtx.i16at(size_t(d.numH) * 4 + size_t(g - d.numH) * 2);
}

bool Font::hasKerning() const { return !d_->gposKern.empty() || !d_->kernPairs.empty(); }

int Font::kerning(u16 l, u16 r) const {
  const Impl& d = *d_;
  if (!d.gposKern.empty()) {
    if (l >= numGlyphs_ || r >= numGlyphs_) return 0;
    // Каждый поиск применяет первую подходящую подтаблицу; поправки поисков складываются.
    int sum = 0;
    for (const PairLookup& lk : d.gposKern)
      for (const PairSub& s : lk.subs)
        if (s.lookup(l, r, sum)) break;
    return sum;
  }
  if (!d.kernPairs.empty()) {
    u32 key = (u32(l) << 16) | r;
    auto it = std::lower_bound(d.kernPairs.begin(), d.kernPairs.end(), key, [](const std::pair<u32, i16>& p, u32 k) { return p.first < k; });
    if (it != d.kernPairs.end() && it->first == key) return it->second;
  }
  return 0;
}

bool Font::outline(u16 glyph, GlyphOutline& out) const {
  out.clear();
  GlyphOutline raw;
  int budget = kMaxComponents;
  if (!d_->rawOutline(glyph, numGlyphs_, raw, 0, budget)) return false;
  if (raw.contourEnds.empty()) return false;
  normalizeOutline(raw, out);
  return !out.empty();
}

// ================================================================ GlyphOutline
RectF GlyphOutline::bounds() const {
  if (pts.empty()) return {};
  float x0 = pts[0].x, y0 = pts[0].y, x1 = x0, y1 = y0;
  for (const Pt& p : pts) {
    x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
    x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
  }
  return {x0, y0, x1 - x0, y1 - y0};
}

void GlyphOutline::transform(const Affine& m) {
  for (Pt& p : pts) p = m.apply(p);
}

void GlyphOutline::embolden(float sx, float sy) {
  if ((sx == 0 && sy == 0) || pts.empty()) return;
  // Ориентация по суммарной площади: внешняя нормаль зависит от направления обхода.
  double area = 0;
  size_t start = 0;
  for (u32 e : contourEnds) {
    for (size_t i = start; i <= e; i++) {
      const Pt& a = pts[i];
      const Pt& b = pts[i == e ? start : i + 1];
      area += double(a.x) * b.y - double(b.x) * a.y;
    }
    start = size_t(e) + 1;
  }
  float sign = area < 0 ? 1.f : -1.f;
  std::vector<Pt> res(pts.size());
  start = 0;
  for (u32 e : contourEnds) {
    size_t n = size_t(e) - start + 1;
    for (size_t k = 0; k < n; k++) {
      size_t i = start + k;
      Pt p = pts[i];
      // Соседи, пропуская совпадающие точки.
      Pt prev = p, next = p;
      for (size_t j = 1; j < n; j++) {
        prev = pts[start + (k + n - j) % n];
        if (prev.x != p.x || prev.y != p.y) break;
      }
      for (size_t j = 1; j < n; j++) {
        next = pts[start + (k + j) % n];
        if (next.x != p.x || next.y != p.y) break;
      }
      float ix = p.x - prev.x, iy = p.y - prev.y, ox = next.x - p.x, oy = next.y - p.y;
      float il = std::sqrt(ix * ix + iy * iy), ol = std::sqrt(ox * ox + oy * oy);
      if (il <= 0 || ol <= 0) { res[i] = p; continue; }
      ix /= il; iy /= il; ox /= ol; oy /= ol;
      // Внешние нормали входящего и исходящего отрезков.
      float n1x = sign * -iy, n1y = sign * ix, n2x = sign * -oy, n2y = sign * ox;
      float mx = n1x + n2x, my = n1y + n2y;
      float dot = n1x * n2x + n1y * n2y;
      float k2 = 1 + dot;
      if (k2 < 0.08f) {
        // Почти разворот: смещение по одной нормали.
        res[i] = {p.x + n1x * sx, p.y + n1y * sy};
        continue;
      }
      // Единичное смещение биссектрисы (не длиннее 3), затем масштаб по осям.
      float kk = 1 / k2;
      float len = kk * std::sqrt(mx * mx + my * my);
      if (len > 3) kk *= 3 / len;
      res[i] = {p.x + mx * kk * sx, p.y + my * kk * sy};
    }
    start = size_t(e) + 1;
  }
  pts.swap(res);
}

void GlyphOutline::appendTo(Path& out, const Affine& m) const {
  size_t start = 0;
  for (u32 e : contourEnds) {
    size_t end = e;
    if (end < start || end >= pts.size()) break;
    Pt p0 = m.apply(pts[start]);
    out.moveTo(p0.x, p0.y);
    size_t i = start + 1;
    while (i <= end) {
      if (onCurve[i]) {
        Pt p = m.apply(pts[i]);
        out.lineTo(p.x, p.y);
        i++;
      } else {
        Pt c = m.apply(pts[i]);
        Pt p = i + 1 <= end ? m.apply(pts[i + 1]) : p0;
        out.quadTo(c.x, c.y, p.x, p.y);
        i += 2;
      }
    }
    out.close();
    start = end + 1;
  }
}

// ================================================================ растеризация
namespace {

// Накопитель площади: в ячейку строки пишется приращение покрытия; префиксная сумма по строке даёт покрытие.
struct Accum {
  int w = 0, h = 0, stride = 0;
  std::vector<float> a;

  void reset(int w_, int h_) {
    w = w_; h = h_; stride = w_ + 2;
    a.assign(size_t(stride) * size_t(h), 0.f);
  }

  // Вклад отрезка внутри одной строки: x от xa до xb, высота dy со знаком направления.
  void cell(float* r, float xa, float xb, float d) {
    float x0 = std::min(xa, xb), x1 = std::max(xa, xb);
    x0 = std::clamp(x0, 0.f, float(w));
    x1 = std::clamp(x1, 0.f, float(w));
    int i0 = int(x0), i1 = int(x1);
    if (i1 > w) i1 = w;
    if (i0 == i1 || x1 - x0 < 1e-5f) {
      // Отрезок в одном столбце: доля ячейки справа от отрезка = (i0 + 1) − средний x.
      float c = d * (float(i0 + 1) - 0.5f * (x0 + x1));
      r[i0] += c;
      r[i0 + 1] += d - c;
      return;
    }
    // G(c) = ∫ max(0, c − x(y)) dy / dy — площадь между отрезком и вертикалью c (x равномерен на [x0, x1]).
    float inv = 1.f / (x1 - x0);
    float mid = 0.5f * (x0 + x1);
    auto G = [&](float c) -> float {
      if (c <= x0) return 0.f;
      if (c >= x1) return c - mid;
      float t = c - x0;
      return 0.5f * t * t * inv;
    };
    float prevContrib = 0, gi = G(float(i0));
    for (int i = i0; i <= i1; i++) {
      float gn = G(float(i + 1));
      float contrib = gn - gi;  // доля ячейки i справа от отрезка
      r[i] += d * (contrib - prevContrib);
      prevContrib = contrib;
      gi = gn;
    }
    r[i1 + 1] += d * (1.f - prevContrib);
  }

  void line(Pt p0, Pt p1) {
    if (p0.y == p1.y) return;
    float dir = 1;
    if (p0.y > p1.y) { std::swap(p0, p1); dir = -1; }
    float ys = std::max(p0.y, 0.f), ye = std::min(p1.y, float(h));
    if (ys >= ye) return;
    float dxdy = (p1.x - p0.x) / (p1.y - p0.y);
    int r0 = int(ys), r1 = std::min(h, int(std::ceil(ye)));
    float x = p0.x + (ys - p0.y) * dxdy;
    for (int row = r0; row < r1; row++) {
      float ya = std::max(ys, float(row)), yb = std::min(ye, float(row + 1));
      float dy = yb - ya;
      if (dy <= 0) continue;
      float xn = x + dxdy * dy;
      cell(a.data() + size_t(row) * size_t(stride), x, xn, dy * dir);
      x = xn;
    }
  }

  void quad(Pt p0, Pt c, Pt p1) {
    // Число отрезков из оценки отклонения: ошибка ≈ |p0 − 2c + p1| / (8 n²) ≤ 0,02 px.
    float ddx = p0.x - 2 * c.x + p1.x, ddy = p0.y - 2 * c.y + p1.y;
    float dd = std::sqrt(ddx * ddx + ddy * ddy);
    int n = 1 + int(std::sqrt(dd * 6.25f));
    n = std::min(n, 64);
    Pt prev = p0;
    float inv = 1.f / float(n);
    for (int i = 1; i <= n; i++) {
      float t = float(i) * inv, mt = 1 - t;
      Pt p = i == n ? p1 : Pt{mt * mt * p0.x + 2 * mt * t * c.x + t * t * p1.x, mt * mt * p0.y + 2 * mt * t * c.y + t * t * p1.y};
      line(prev, p);
      prev = p;
    }
  }
};

// Кривые усиления покрытия по тону текста (10 бит -> 8 бит).
struct ToneLut {
  u8 v[3][1025];
  ToneLut() {
    // Показатель: светлому тексту на тёмном фоне нужно большее усиление (смешивание в sRGB его истончает).
    const float gamma[3] = {1.25f, 1.4f, 1.55f};
    for (int t = 0; t < 3; t++) {
      for (int i = 0; i <= 1024; i++) {
        float c = float(i) / 1024.f;
        float g = std::pow(c, 1.f / gamma[t]);
        // Лёгкий контраст: подавление самой бледной «бахромы».
        if (c < 0.02f) g *= c / 0.02f;
        v[t][i] = u8(std::clamp(g * 255.f + 0.5f, 0.f, 255.f));
      }
    }
  }
};
const ToneLut& toneLut() {
  static const ToneLut lut;
  return lut;
}

}  // namespace

TextTone toneOf(Color c) {
  float l = c.luminance();
  if (l < 0.18f) return TextTone::Dark;
  if (l < 0.45f) return TextTone::Mid;
  return TextTone::Light;
}

float synthStrength(Synth s, float size) {
  switch (s) {
    case Synth::Semibold: return size * 0.016f;
    case Synth::Bold: return size * 0.028f;
    default: return 0;
  }
}
float synthAdvance(Synth s, float size) { return 2 * synthStrength(s, size); }
float synthAdvance(const Font& f, Synth s, float size) { return f.monospace() ? 0.f : synthAdvance(s, size); }

GlyphBitmap rasterizeOutline(const GlyphOutline& o, TextTone tone, bool applyTone) {
  GlyphBitmap out;
  if (o.empty() || o.onCurve.size() != o.pts.size()) return out;
  RectF b = o.bounds();
  constexpr float kLim = 1e7f;
  if (!(std::fabs(b.x) < kLim && std::fabs(b.y) < kLim && b.w < kLim && b.h < kLim)) return out;
  int x0 = int(std::floor(b.x)), y0 = int(std::floor(b.y));
  int x1 = int(std::ceil(b.right())), y1 = int(std::ceil(b.bottom()));
  int w = x1 - x0, h = y1 - y0;
  if (w <= 0 || h <= 0) return out;
  if (w > 8192 || h > 8192 || size_t(w) * size_t(h) > (size_t(1) << 24)) return out;
  thread_local Accum acc;
  acc.reset(w, h);
  float ox = float(x0), oy = float(y0);
  size_t start = 0;
  for (u32 e : o.contourEnds) {
    size_t end = e;
    if (end < start || end >= o.pts.size()) break;
    auto P = [&](size_t i) { return Pt{o.pts[i].x - ox, o.pts[i].y - oy}; };
    Pt first = P(start), cur = first;
    size_t i = start + 1;
    while (i <= end) {
      if (o.onCurve[i]) {
        Pt p = P(i);
        acc.line(cur, p);
        cur = p;
        i++;
      } else {
        Pt c = P(i);
        Pt p = i + 1 <= end ? P(i + 1) : first;
        acc.quad(cur, c, p);
        cur = p;
        i += 2;
      }
    }
    acc.line(cur, first);
    start = end + 1;
  }
  out.mask = Mask(w, h);
  out.left = x0;
  out.top = y0;
  const u8* lut = toneLut().v[int(tone)];
  for (int y = 0; y < h; y++) {
    const float* r = acc.a.data() + size_t(y) * size_t(acc.stride);
    u8* dst = out.mask.row(y);
    float s = 0;
    for (int x = 0; x < w; x++) {
      s += r[x];
      float c = std::min(1.f, std::fabs(s));
      dst[x] = applyTone ? lut[int(c * 1024.f + 0.5f)] : u8(c * 255.f + 0.5f);
    }
  }
  // Крупный буфер не держим в потоке.
  if (acc.a.size() > (size_t(1) << 20)) {
    acc.a.clear();
    acc.a.shrink_to_fit();
  }
  return out;
}

GlyphBitmap renderGlyph(const Font& f, u16 glyph, float sizePx, int subX, Synth synth, TextTone tone) {
  GlyphOutline o;
  if (!f.outline(glyph, o)) return {};
  float size = std::max(0.25f, std::round(sizePx * 4) / 4);
  float k = size / float(f.unitsPerEm());
  float strength = synthStrength(synth, size);
  float dx = float(std::clamp(subX, 0, 3)) * 0.25f + synthAdvance(f, synth, size) * 0.5f;
  // Мелкий кегль: высота строчных и прописных притягивается к целым пикселям (кусочно-линейно по y),
  // чтобы верхние края букв не размывались на две строки пикселей. Ширины и продвижения не меняются.
  float xh = float(f.xHeight()), cap = float(f.capHeight());
  float X = std::round(xh * k), C = std::round(cap * k);
  bool snap = size >= 8 && size <= 30 && xh > 0 && cap > xh && X >= 1 && C > X;
  if (snap) {
    float s1 = X / xh, s2 = (C - X) / (cap - xh);
    snap = s2 > k * 0.6f && s2 < k * 1.6f;
    if (snap) {
      for (Pt& p : o.pts) {
        float y = p.y, ny;
        if (y <= 0) ny = y * k;
        else if (y <= xh) ny = y * s1;
        else if (y <= cap) ny = X + (y - xh) * s2;
        else ny = C + (y - cap) * k;
        p = {p.x * k + dx, -ny};
      }
    }
  }
  if (!snap) o.transform(Affine{k, 0, 0, -k, dx, 0});
  // При притяжении высот утолщение только по горизонтали — края остаются на целых пикселях.
  if (strength > 0) o.embolden(strength, snap ? 0.f : strength);
  return rasterizeOutline(o, tone, true);
}

// ================================================================ кеш растров
struct GlyphCache::Impl {
  struct Key {
    u32 font, a, b;
    bool operator==(const Key&) const = default;
  };
  struct KeyHash {
    size_t operator()(const Key& k) const { return size_t(hashMix(hashMix(k.font, k.a), k.b)); }
  };
  struct Entry {
    Key key;
    std::shared_ptr<const GlyphBitmap> bmp;
    size_t bytes;
  };
  mutable std::mutex mu;
  std::list<Entry> lru;  // начало — самые свежие
  std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> map;
  size_t bytes = 0, capacity;
  u64 hits = 0, misses = 0;

  static Key keyOf(const GlyphRequest& r) {
    return {r.font ? r.font->uid() : 0u, (u32(r.glyph) << 16) | r.size4, u32(r.subX) | (u32(r.synth) << 8) | (u32(r.tone) << 16)};
  }
  std::shared_ptr<const GlyphBitmap> findLocked(const Key& k) {
    auto it = map.find(k);
    if (it == map.end()) return nullptr;
    if (it->second != lru.begin()) lru.splice(lru.begin(), lru, it->second);
    hits++;
    return it->second->bmp;
  }
  void insertLocked(const Key& k, std::shared_ptr<const GlyphBitmap> bmp) {
    auto it = map.find(k);
    if (it != map.end()) return;
    size_t sz = bmp->mask.a.size() + sizeof(GlyphBitmap) + 64;
    lru.push_front({k, std::move(bmp), sz});
    map.emplace(k, lru.begin());
    bytes += sz;
    while (bytes > capacity && lru.size() > 1) {
      Entry& e = lru.back();
      bytes -= e.bytes;
      map.erase(e.key);
      lru.pop_back();
    }
  }
};

GlyphCache::GlyphCache(size_t cap) : d_(std::make_unique<Impl>()) { d_->capacity = cap; }
GlyphCache::~GlyphCache() = default;

static std::shared_ptr<const GlyphBitmap> renderRequest(const GlyphRequest& r) {
  auto bmp = std::make_shared<GlyphBitmap>();
  if (r.font) *bmp = renderGlyph(*r.font, r.glyph, float(r.size4) / 4.f, r.subX, r.synth, r.tone);
  return bmp;
}

std::shared_ptr<const GlyphBitmap> GlyphCache::get(const GlyphRequest& r) {
  auto k = Impl::keyOf(r);
  {
    std::lock_guard lk(d_->mu);
    if (auto b = d_->findLocked(k)) return b;
    d_->misses++;
  }
  auto bmp = renderRequest(r);
  std::lock_guard lk(d_->mu);
  if (auto b = d_->findLocked(k)) return b;
  d_->insertLocked(k, bmp);
  return bmp;
}

void GlyphCache::getMany(const GlyphRequest* req, size_t n, std::shared_ptr<const GlyphBitmap>* out) {
  thread_local std::vector<size_t> miss;
  miss.clear();
  {
    std::lock_guard lk(d_->mu);
    for (size_t i = 0; i < n; i++) {
      out[i] = d_->findLocked(Impl::keyOf(req[i]));
      if (!out[i]) miss.push_back(i);
    }
    d_->misses += miss.size();
  }
  if (miss.empty()) return;
  // Растеризация промахов вне блокировки; повторы внутри пакета — один раз.
  for (size_t m = 0; m < miss.size(); m++) {
    size_t i = miss[m];
    if (out[i]) continue;
    auto k = Impl::keyOf(req[i]);
    out[i] = renderRequest(req[i]);
    for (size_t m2 = m + 1; m2 < miss.size(); m2++) {
      size_t j = miss[m2];
      if (!out[j] && Impl::keyOf(req[j]) == k) out[j] = out[i];
    }
  }
  std::lock_guard lk(d_->mu);
  for (size_t i : miss) {
    auto k = Impl::keyOf(req[i]);
    if (auto b = d_->findLocked(k)) { out[i] = b; d_->hits--; continue; }
    d_->insertLocked(k, out[i]);
  }
}

void GlyphCache::setCapacity(size_t b) {
  std::lock_guard lk(d_->mu);
  d_->capacity = b;
  while (d_->bytes > d_->capacity && !d_->lru.empty()) {
    auto& e = d_->lru.back();
    d_->bytes -= e.bytes;
    d_->map.erase(e.key);
    d_->lru.pop_back();
  }
}

void GlyphCache::clear() {
  std::lock_guard lk(d_->mu);
  d_->lru.clear();
  d_->map.clear();
  d_->bytes = 0;
}

GlyphCache::Stats GlyphCache::stats() const {
  std::lock_guard lk(d_->mu);
  return {d_->map.size(), d_->bytes, d_->capacity, d_->hits, d_->misses};
}

GlyphCache& glyphCache() {
  static GlyphCache cache(32u << 20);
  return cache;
}

}  // namespace rg::gfx
