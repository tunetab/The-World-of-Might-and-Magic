// Regnum — DEFLATE: табличный распаковщик и упаковщик (LZ77 с цепочками хешей, ленивое сравнение,
// динамические коды Хаффмана с ограничением длины методом package-merge).
#include "codec/zlib.h"

#include <bit>

namespace rg::codec {

namespace {

// ================================================================ общие помощники
inline u16 ld16(const u8* p) { return u16(p[0] | (p[1] << 8)); }
inline u32 ld32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }
inline u32 ld32be(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]); }
inline u64 ld64(const u8* p) {
  u64 v;
  std::memcpy(&v, p, 8);
  if constexpr (std::endian::native == std::endian::big) {
    v = ((v & 0x00000000FFFFFFFFull) << 32) | ((v & 0xFFFFFFFF00000000ull) >> 32);
    v = ((v & 0x0000FFFF0000FFFFull) << 16) | ((v & 0xFFFF0000FFFF0000ull) >> 16);
    v = ((v & 0x00FF00FF00FF00FFull) << 8) | ((v & 0xFF00FF00FF00FF00ull) >> 8);
  }
  return v;
}

inline u32 reverseBits(u32 code, int len) {
  u32 r = 0;
  for (int i = 0; i < len; i++) { r = (r << 1) | (code & 1); code >>= 1; }
  return r;
}

// ================================================================ CRC-32 (по 8 байт за шаг) и Adler-32
struct CrcTables { u32 t[8][256]; };
constexpr CrcTables makeCrcTables() {
  CrcTables r{};
  for (u32 i = 0; i < 256; i++) {
    u32 c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    r.t[0][i] = c;
  }
  for (int k = 1; k < 8; k++)
    for (u32 i = 0; i < 256; i++) r.t[k][i] = (r.t[k - 1][i] >> 8) ^ r.t[0][r.t[k - 1][i] & 255];
  return r;
}
constexpr CrcTables kCrc = makeCrcTables();

// ================================================================ таблицы DEFLATE
constexpr u16 kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr u8 kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr u16 kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr u8 kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr u8 kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// ================================================================ распаковка
// Элемент таблицы декодирования: tag < 16 — длина/расстояние с tag дополнительными битами.
struct HEntry {
  u16 val;
  u8 len;
  u8 tag;
};
enum : u8 { kTagLit = 16, kTagEob = 17, kTagBad = 18, kTagSub = 32 };

constexpr int kLitBits = 10, kDistBits = 8, kClBits = 7;
constexpr int kLitTable = (1 << kLitBits) + 288 * (1 << (15 - kLitBits));
constexpr int kDistTable = (1 << kDistBits) + 32 * (1 << (15 - kDistBits));

struct SymInfo {
  HEntry lit[288];
  HEntry dist[32];
  HEntry cl[19];
  SymInfo() {
    for (int i = 0; i < 256; i++) lit[i] = {u16(i), 0, kTagLit};
    lit[256] = {0, 0, kTagEob};
    for (int i = 0; i < 29; i++) lit[257 + i] = {kLenBase[i], 0, kLenExtra[i]};
    lit[286] = lit[287] = {0, 0, kTagBad};
    for (int i = 0; i < 30; i++) dist[i] = {kDistBase[i], 0, kDistExtra[i]};
    dist[30] = dist[31] = {0, 0, kTagBad};
    for (int i = 0; i < 19; i++) cl[i] = {u16(i), 0, kTagLit};
  }
};
const SymInfo& symInfo() {
  static const SymInfo s;
  return s;
}

// Построить таблицу канонического кода. false — переполненный код или недопустимо неполный.
// Неполный код допустим (как в zlib) только как единственный код длины 1, и только если allowSingle.
bool buildTable(HEntry* table, int bits, const u8* lens, int n, const HEntry* info, bool allowSingle) {
  u16 count[16] = {};
  for (int i = 0; i < n; i++) count[lens[i]]++;
  count[0] = 0;
  int maxLen = 0;
  for (int l = 15; l >= 1; l--)
    if (count[l]) { maxLen = l; break; }
  const int primary = 1 << bits;
  const HEntry bad{0, 0, kTagBad};
  std::fill(table, table + primary, bad);
  if (maxLen == 0) return true;
  int left = 1;
  for (int l = 1; l <= 15; l++) {
    left = (left << 1) - count[l];
    if (left < 0) return false;
  }
  if (left > 0 && !(allowSingle && maxLen == 1 && count[1] == 1)) return false;
  u32 next[16] = {};
  u32 code = 0;
  for (int l = 1; l <= 15; l++) {
    code = (code + count[l - 1]) << 1;
    next[l] = code;
  }
  const int subBits = std::max(0, maxLen - bits);
  int subNext = primary;
  for (int s = 0; s < n; s++) {
    int len = lens[s];
    if (!len) continue;
    u32 r = reverseBits(next[len]++, len);
    HEntry e = info[s];
    if (len <= bits) {
      e.len = u8(len);
      for (u32 i = r; i < u32(primary); i += 1u << len) table[i] = e;
    } else {
      HEntry& pe = table[r & u32(primary - 1)];
      if (pe.tag < kTagSub) {
        pe = {u16(subNext), u8(bits), u8(kTagSub + subBits)};
        std::fill(table + subNext, table + subNext + (1 << subBits), bad);
        subNext += 1 << subBits;
      }
      HEntry* sub = table + pe.val;
      int rem = len - bits;
      e.len = u8(rem);
      for (u32 i = r >> bits; i < (1u << subBits); i += 1u << rem) sub[i] = e;
    }
  }
  return true;
}

struct FixedTables {
  HEntry lit[kLitTable];
  HEntry dist[kDistTable];
  FixedTables() {
    u8 l[288];
    for (int i = 0; i < 144; i++) l[i] = 8;
    for (int i = 144; i < 256; i++) l[i] = 9;
    for (int i = 256; i < 280; i++) l[i] = 7;
    for (int i = 280; i < 288; i++) l[i] = 8;
    buildTable(lit, kLitBits, l, 288, symInfo().lit, false);
    u8 d[32];
    std::fill(d, d + 32, u8(5));
    buildTable(dist, kDistBits, d, 32, symInfo().dist, false);
  }
};
const FixedTables& fixedTables() {
  static const FixedTables* t = new FixedTables();  // без разрушения при выходе
  return *t;
}

enum class Res { Ok, Stop, Err };

struct Inflater {
  const u8* start = nullptr;
  const u8* in = nullptr;
  const u8* end = nullptr;
  u64 bb = 0;
  u32 bc = 0;
  u32 over = 0;  // байты-нули, добавленные за концом входа

  std::vector<u8>* vec = nullptr;  // растущий выход или nullptr (фиксированный буфер)
  u8* ob = nullptr;
  size_t cap = 0, pos = 0, maxOut = 0, winStart = 0;
  bool stopAtMax = false, full = false;
  const char* err = nullptr;

  HEntry lit[kLitTable];
  HEntry dist[kDistTable];
  HEntry cl[1 << kClBits];

  Res fail(const char* m) {
    err = m;
    return Res::Err;
  }

  bool slowRefill() {
    while (bc < 56) {
      if (in < end) bb |= u64(*in++) << bc;
      else over++;
      bc += 8;
    }
    return over <= 8;
  }
  inline bool refill() {
    if (end - in >= 8) {
      bb |= ld64(in) << bc;
      in += (63 - bc) >> 3;
      bc |= 56;
      return true;
    }
    return slowRefill();
  }
  inline bool need(u32 n) { return bc >= n || refill(); }
  inline u32 take(u32 n) {
    u32 v = u32(bb & ((u64(1) << n) - 1));
    bb >>= n;
    bc -= n;
    return v;
  }
  // Позиция потока в байтах (с учётом недочитанных бит), может превышать размер входа при обрыве.
  size_t bytePos() const { return size_t(in - start) + over - bc / 8; }
  bool consumedOk() const { return u64(in - start + over) * 8 - bc <= u64(end - start) * 8; }

  // Перейти к границе байта и сбросить буфер бит. false — данные кончились раньше.
  bool alignToByte() {
    u32 drop = bc & 7;
    bb >>= drop;
    bc -= drop;
    size_t p = bytePos();
    if (p > size_t(end - start)) return false;
    in = start + p;
    bb = 0;
    bc = 0;
    over = 0;
    return true;
  }

  // Свободное место под need байт. false — предел (err не задаётся).
  bool ensure(size_t need) {
    if (cap - pos >= need) return true;
    if (!vec) return false;
    size_t want = pos + need;
    if (want > maxOut || want < pos) return false;
    size_t nc = std::max(want, std::min(maxOut, std::max<size_t>(cap * 2, size_t(1) << 16)));
    vec->resize(nc);
    ob = vec->data();
    cap = nc;
    return true;
  }
  // Предел выхода достигнут: остановка без ошибки или ошибка.
  Res limit() {
    if (stopAtMax) {
      full = true;
      return Res::Stop;
    }
    return fail("распакованные данные больше допустимого размера");
  }

  Res stored() {
    if (!alignToByte()) return fail("неожиданный конец сжатых данных");
    if (end - in < 4) return fail("неожиданный конец сжатых данных");
    u32 len = ld16(in), nlen = ld16(in + 2);
    if (len != (~nlen & 0xFFFFu)) return fail("повреждён несжатый блок");
    in += 4;
    if (size_t(end - in) < len) return fail("неожиданный конец сжатых данных");
    if (!ensure(len)) {
      size_t room = vec ? (maxOut > pos ? maxOut - pos : 0) : cap - pos;
      if (ensure(room) && room) {
        std::memcpy(ob + pos, in, room);
        pos += room;
      }
      return limit();
    }
    if (len) std::memcpy(ob + pos, in, len);
    pos += len;
    in += len;
    return Res::Ok;
  }

  Res dynamicTables() {
    if (!need(14)) return fail("неожиданный конец сжатых данных");
    u32 hlit = take(5) + 257, hdist = take(5) + 1, hclen = take(4) + 4;
    if (hlit > 286 || hdist > 30) return fail("неверный заголовок блока");
    u8 cll[19] = {};
    for (u32 i = 0; i < hclen; i++) {
      if (!need(3)) return fail("неожиданный конец сжатых данных");
      cll[kClOrder[i]] = u8(take(3));
    }
    if (!buildTable(cl, kClBits, cll, 19, symInfo().cl, false)) return fail("неверный код длин");
    u8 lens[286 + 30];
    u32 total = hlit + hdist, i = 0;
    while (i < total) {
      if (!need(14)) return fail("неожиданный конец сжатых данных");
      HEntry e = cl[bb & ((1u << kClBits) - 1)];
      if (e.tag == kTagBad) return fail("неверный код длин");
      bb >>= e.len;
      bc -= e.len;
      u32 sym = e.val, rep;
      u8 v = 0;
      if (sym < 16) {
        lens[i++] = u8(sym);
        continue;
      } else if (sym == 16) {
        if (i == 0) return fail("повтор длины без предыдущей");
        v = lens[i - 1];
        rep = 3 + take(2);
      } else if (sym == 17) {
        rep = 3 + take(3);
      } else {
        rep = 11 + take(7);
      }
      if (i + rep > total) return fail("слишком много длин кодов");
      std::memset(lens + i, v, rep);
      i += rep;
    }
    if (lens[256] == 0) return fail("нет кода конца блока");
    if (!buildTable(lit, kLitBits, lens, int(hlit), symInfo().lit, true)) return fail("неверный код литералов");
    if (!buildTable(dist, kDistBits, lens + hlit, int(hdist), symInfo().dist, true)) return fail("неверный код расстояний");
    return Res::Ok;
  }

  Res huffman(const HEntry* lt, const HEntry* dt) {
    constexpr u64 lmask = (1u << kLitBits) - 1, dmask = (1u << kDistBits) - 1;
    u8* o = ob + pos;
    u8* oe = ob + cap;
    for (;;) {
      if (!refill()) {
        pos = size_t(o - ob);
        return fail("неожиданный конец сжатых данных");
      }
      HEntry e = lt[bb & lmask];
      if (e.tag >= kTagSub) {
        bb >>= e.len;
        bc -= e.len;
        e = lt[e.val + (bb & ((1u << (e.tag - kTagSub)) - 1))];
      }
      bb >>= e.len;
      bc -= e.len;
      if (e.tag == kTagLit) {
        if (o == oe) {
          pos = size_t(o - ob);
          if (!ensure(1)) return limit();
          o = ob + pos;
          oe = ob + cap;
        }
        *o++ = u8(e.val);
        continue;
      }
      if (e.tag < 16) {
        u32 len = e.val + u32(bb & ((1u << e.tag) - 1));
        bb >>= e.tag;
        bc -= e.tag;
        HEntry d = dt[bb & dmask];
        if (d.tag >= kTagSub) {
          bb >>= d.len;
          bc -= d.len;
          d = dt[d.val + (bb & ((1u << (d.tag - kTagSub)) - 1))];
        }
        if (d.tag >= 16) {
          pos = size_t(o - ob);
          return fail("неверный код расстояния");
        }
        bb >>= d.len;
        bc -= d.len;
        u32 dst = d.val + u32(bb & ((1u << d.tag) - 1));
        bb >>= d.tag;
        bc -= d.tag;
        if (dst > size_t(o - (ob + winStart))) {
          pos = size_t(o - ob);
          return fail("ссылка за пределы окна");
        }
        if (size_t(oe - o) >= len + 8) {
          const u8* src = o - dst;
          u8* stop = o + len;
          if (dst >= 8) {
            do {
              std::memcpy(o, src, 8);
              o += 8;
              src += 8;
            } while (o < stop);
          } else if (dst == 1) {
            std::memset(o, *src, len);
          } else {
            // Повтор короткого образца: расстояние копирования удваивается.
            size_t step = dst, done = 0;
            while (done < len) {
              size_t k = std::min<size_t>(step, len - done);
              std::memcpy(o + done, src, k);
              done += k;
              step *= 2;
            }
          }
          o = stop;
        } else {
          pos = size_t(o - ob);
          bool ok = ensure(len);
          if (!ok) {
            // записать, сколько поместится, и остановиться
            size_t room = vec ? (maxOut > pos ? maxOut - pos : 0) : cap - pos;
            if (room > len) room = len;
            if (room && ensure(room)) {
              for (size_t k = 0; k < room; k++) ob[pos + k] = ob[pos + k - dst];
              pos += room;
            }
            return limit();
          }
          o = ob + pos;
          oe = ob + cap;
          const u8* src = o - dst;
          for (u32 k = 0; k < len; k++) o[k] = src[k];
          o += len;
        }
        continue;
      }
      pos = size_t(o - ob);
      if (e.tag == kTagEob) return Res::Ok;
      return fail("неверный код литерала или длины");
    }
  }

  // Поток DEFLATE от текущей позиции до последнего блока.
  Res raw() {
    for (;;) {
      if (!need(3)) return fail("неожиданный конец сжатых данных");
      u32 final = take(1), type = take(2);
      Res r;
      if (type == 0) r = stored();
      else if (type == 1) r = huffman(fixedTables().lit, fixedTables().dist);
      else if (type == 2) {
        r = dynamicTables();
        if (r == Res::Ok) r = huffman(lit, dist);
      } else {
        return fail("неверный тип блока");
      }
      if (r != Res::Ok) return r;
      if (!consumedOk()) return fail("неожиданный конец сжатых данных");
      if (final) return Res::Ok;
    }
  }
};

const char* formatName(ZFormat f) { return f == ZFormat::Gzip ? "gzip" : f == ZFormat::Zlib ? "zlib" : "deflate"; }

// Общий разбор обёрток. Возвращает число байт выхода или nullopt.
std::optional<size_t> inflateImpl(Inflater& z, std::span<const u8> data, ZFormat fmt, bool verify, std::string* error) {
  auto failMsg = [&](const char* m) -> std::optional<size_t> {
    if (error) *error = strf("Сжатые данные (%s) повреждены: %s", formatName(fmt), m);
    return std::nullopt;
  };
  z.start = data.data();
  z.in = data.data();
  z.end = data.data() + data.size();
  const size_t n = data.size();

  if (fmt == ZFormat::Raw) {
    Res r = z.raw();
    if (r == Res::Err) return failMsg(z.err);
    return z.pos;
  }

  if (fmt == ZFormat::Zlib) {
    if (n < 2) return failMsg("нет заголовка zlib");
    u8 cmf = data[0], flg = data[1];
    if ((cmf & 15) != 8 || (cmf >> 4) > 7 || ((u32(cmf) << 8) | flg) % 31 != 0) return failMsg("неверный заголовок zlib");
    if (flg & 0x20) return failMsg("словарь zlib не поддерживается");
    z.in = z.start + 2;
    Res r = z.raw();
    if (r == Res::Err) return failMsg(z.err);
    if (r == Res::Stop) return z.pos;
    if (!z.alignToByte() || z.end - z.in < 4) return failMsg("нет контрольной суммы");
    if (verify && ld32be(z.in) != adler32(z.ob, z.pos)) return failMsg("контрольная сумма не совпадает");
    return z.pos;
  }

  // gzip: один или несколько участников подряд
  for (;;) {
    const u8* p = z.in;
    size_t left = size_t(z.end - p);
    if (left < 18) return failMsg("нет заголовка gzip");
    if (p[0] != 0x1F || p[1] != 0x8B || p[2] != 8) return failMsg("неверный заголовок gzip");
    u8 flg = p[3];
    if (flg & 0xE0) return failMsg("неверные флаги gzip");
    const u8* q = p + 10;
    if (flg & 4) {
      if (z.end - q < 2) return failMsg("обрыв заголовка gzip");
      size_t xlen = ld16(q);
      q += 2;
      if (size_t(z.end - q) < xlen) return failMsg("обрыв заголовка gzip");
      q += xlen;
    }
    for (int bit : {8, 16}) {
      if (flg & bit) {
        while (q < z.end && *q) q++;
        if (q >= z.end) return failMsg("обрыв заголовка gzip");
        q++;
      }
    }
    if (flg & 2) {
      if (z.end - q < 2) return failMsg("обрыв заголовка gzip");
      if (verify && ld16(q) != (crc32(p, size_t(q - p)) & 0xFFFF)) return failMsg("контрольная сумма заголовка не совпадает");
      q += 2;
    }
    z.in = q;
    z.bb = 0;
    z.bc = 0;
    z.over = 0;
    size_t memberStart = z.pos;
    z.winStart = memberStart;
    Res r = z.raw();
    if (r == Res::Err) return failMsg(z.err);
    if (r == Res::Stop) return z.pos;
    if (!z.alignToByte() || z.end - z.in < 8) return failMsg("нет контрольной суммы gzip");
    u32 crc = ld32(z.in), isize = ld32(z.in + 4);
    z.in += 8;
    size_t msize = z.pos - memberStart;
    if (verify && crc != crc32(z.ob + memberStart, msize)) return failMsg("контрольная сумма не совпадает");
    if (isize != u32(msize)) return failMsg("размер данных не совпадает");
    if (z.end - z.in >= 2 && z.in[0] == 0x1F && z.in[1] == 0x8B) continue;
    for (const u8* t = z.in; t < z.end; t++)
      if (*t) return failMsg("лишние данные после потока gzip");
    return z.pos;
  }
}

// ================================================================ упаковка
struct LevelCfg {
  u16 good, lazy, nice, chain;
};
constexpr LevelCfg kLevels[10] = {
    {0, 0, 0, 0},         // 0 — без сжатия
    {4, 4, 8, 4},         // 1..3 — быстрый режим (lazy = предел вставки)
    {4, 5, 16, 8},
    {4, 6, 32, 32},
    {4, 4, 16, 16},       // 4..9 — ленивое сравнение
    {8, 16, 32, 32},
    {8, 16, 128, 128},
    {8, 32, 128, 256},
    {32, 128, 258, 1024},
    {32, 258, 258, 4096},
};

constexpr int kHashBits = 15;
constexpr size_t kWin = 32768;
constexpr u32 kTooFar = 4096;
constexpr size_t kBlockSyms = 16384;

struct CodeTables {
  u8 lenCode[256];   // длина − 3 -> номер кода длины (0..28)
  u8 distCode[512];  // расстояние − 1 (< 256) или 256 + ((расстояние − 1) >> 7)
  CodeTables() {
    for (int c = 0; c < 29; c++) {
      int lo = kLenBase[c] - 3, hi = c == 28 ? 255 : kLenBase[c + 1] - 4;
      if (c == 27) hi = 254;  // длина 258 кодируется только кодом 285
      for (int l = lo; l <= hi; l++) lenCode[l] = u8(c);
    }
    lenCode[255] = 28;
    for (int c = 0; c < 30; c++) {
      int lo = kDistBase[c] - 1, hi = (c == 29 ? 32768 : kDistBase[c + 1] - 1) - 1;
      for (int d = lo; d <= hi; d++) {
        if (d < 256) distCode[d] = u8(c);
        else distCode[256 + (d >> 7)] = u8(c);
      }
    }
  }
  int dist(u32 d) const { return d - 1 < 256 ? distCode[d - 1] : distCode[256 + ((d - 1) >> 7)]; }
};
const CodeTables& codeTables() {
  static const CodeTables t;
  return t;
}

// Длины кодов Хаффмана с ограничением maxBits (package-merge). Ноль частоты — длина 0.
void huffLengths(const u32* freq, int n, int maxBits, u8* lens) {
  std::fill(lens, lens + n, u8(0));
  struct Node {
    u64 w;
    int a, b;  // b < 0: лист с символом a
  };
  std::vector<Node> nodes;
  std::vector<int> leaves;
  for (int i = 0; i < n; i++)
    if (freq[i]) {
      nodes.push_back({freq[i], i, -1});
      leaves.push_back(int(nodes.size() - 1));
    }
  size_t m = leaves.size();
  if (m == 0) return;
  if (m == 1) {
    lens[nodes[0].a] = 1;
    return;
  }
  std::stable_sort(leaves.begin(), leaves.end(), [&](int x, int y) { return nodes[x].w < nodes[y].w; });
  std::vector<int> list = leaves, merged, packs;
  const size_t keep = 2 * m - 2;
  for (int level = 1; level < maxBits; level++) {
    packs.clear();
    for (size_t i = 0; i + 1 < list.size(); i += 2) {
      nodes.push_back({nodes[list[i]].w + nodes[list[i + 1]].w, list[i], list[i + 1]});
      packs.push_back(int(nodes.size() - 1));
    }
    merged.clear();
    size_t i = 0, j = 0;
    while (merged.size() < keep && (i < m || j < packs.size())) {
      if (j >= packs.size() || (i < m && nodes[leaves[i]].w <= nodes[packs[j]].w)) merged.push_back(leaves[i++]);
      else merged.push_back(packs[j++]);
    }
    list.swap(merged);
  }
  // Длина символа — число его вхождений в первые 2m−2 элементов.
  std::vector<int> stack;
  for (size_t k = 0; k < keep && k < list.size(); k++) {
    stack.push_back(list[k]);
    while (!stack.empty()) {
      const Node& nd = nodes[stack.back()];
      stack.pop_back();
      if (nd.b < 0) lens[nd.a]++;
      else {
        stack.push_back(nd.a);
        stack.push_back(nd.b);
      }
    }
  }
}

// Канонические коды (развёрнутые для записи младшим битом вперёд).
void huffCodes(const u8* lens, int n, u16* codes) {
  u16 count[16] = {};
  for (int i = 0; i < n; i++) count[lens[i]]++;
  count[0] = 0;
  u32 next[16] = {};
  u32 code = 0;
  for (int l = 1; l <= 15; l++) {
    code = (code + count[l - 1]) << 1;
    next[l] = code;
  }
  for (int i = 0; i < n; i++) codes[i] = lens[i] ? u16(reverseBits(next[lens[i]]++, lens[i])) : 0;
}

struct BitWriter {
  std::vector<u8>& out;
  u64 bb = 0;
  u32 bc = 0;
  explicit BitWriter(std::vector<u8>& o) : out(o) {}
  inline void put(u32 v, u32 n) {
    bb |= u64(v) << bc;
    bc += n;
    if (bc >= 32) {
      size_t s = out.size();
      out.resize(s + 4);
      u8* p = out.data() + s;
      p[0] = u8(bb);
      p[1] = u8(bb >> 8);
      p[2] = u8(bb >> 16);
      p[3] = u8(bb >> 24);
      bb >>= 32;
      bc -= 32;
    }
  }
  void align() {
    while (bc > 0) {
      out.push_back(u8(bb));
      bb >>= 8;
      bc = bc > 8 ? bc - 8 : 0;
    }
    bb = 0;
  }
};

struct Deflater {
  const u8* d;
  size_t n;
  LevelCfg cfg;
  int level;
  BitWriter bw;
  std::vector<i32> head, prev;
  std::vector<u32> syms;  // литерал: байт; совпадение: 0x80000000 | (длина−3) << 16 | (расстояние−1)
  u32 litFreq[286] = {};
  u32 distFreq[30] = {};
  u64 extraBits = 0;
  size_t blockStart = 0, emitted = 0;

  Deflater(const u8* data, size_t size, int lvl, std::vector<u8>& out)
      : d(data), n(size), cfg(kLevels[lvl]), level(lvl), bw(out) {
    syms.reserve(kBlockSyms + 2);
  }

  inline u32 hashAt(size_t p) const {
    u32 v = u32(d[p]) | (u32(d[p + 1]) << 8) | (u32(d[p + 2]) << 16);
    return (v * 2654435761u) >> (32 - kHashBits);
  }
  inline i32 insert(size_t p) {
    u32 h = hashAt(p);
    i32 c = head[h];
    prev[p & (kWin - 1)] = c;
    head[h] = i32(p);
    return c;
  }

  // Длина совпадения s и m, не более maxLen.
  static inline u32 matchLen(const u8* m, const u8* s, u32 maxLen) {
    u32 i = 0;
    while (i + 8 <= maxLen) {
      u64 x = ld64(m + i) ^ ld64(s + i);
      if (x) return i + u32(std::countr_zero(x) >> 3);
      i += 8;
    }
    while (i < maxLen && m[i] == s[i]) i++;
    return i;
  }

  // Лучшее совпадение длиннее best среди цепочки cand. Возвращает длину (best, если лучше нет).
  u32 longest(size_t pos, i32 cand, u32 best, u32& dist) const {
    u32 maxLen = u32(std::min<size_t>(258, n - pos));
    if (maxLen < 3 || best >= maxLen) return best;
    u32 chain = cfg.chain;
    if (best >= cfg.good) chain >>= 2;
    u32 nice = std::min<u32>(cfg.nice, maxLen);
    const u8* s = d + pos;
    while (cand >= 0 && pos - size_t(cand) < kWin && chain-- > 0) {
      const u8* m = d + cand;
      if (m[best] == s[best] && m[0] == s[0] && m[1] == s[1]) {
        u32 len = matchLen(m, s, maxLen);
        if (len > best) {
          best = len;
          dist = u32(pos - size_t(cand));
          if (len >= nice) break;
        }
      }
      cand = prev[size_t(cand) & (kWin - 1)];
    }
    return best;
  }

  inline void lit(u8 c) {
    syms.push_back(c);
    litFreq[c]++;
    emitted++;
    if (syms.size() >= kBlockSyms) flushBlock(false);
  }
  inline void match(u32 len, u32 dist) {
    const CodeTables& ct = codeTables();
    syms.push_back(0x80000000u | ((len - 3) << 16) | (dist - 1));
    int lc = ct.lenCode[len - 3], dc = ct.dist(dist);
    litFreq[257 + lc]++;
    distFreq[dc]++;
    extraBits += kLenExtra[lc] + kDistExtra[dc];
    emitted += len;
    if (syms.size() >= kBlockSyms) flushBlock(false);
  }

  void storedBlocks(size_t from, size_t to, bool final) {
    do {
      size_t k = std::min<size_t>(65535, to - from);
      bool last = final && from + k == to;
      bw.put(last ? 1 : 0, 1);
      bw.put(0, 2);
      bw.align();
      std::vector<u8>& o = bw.out;
      o.push_back(u8(k));
      o.push_back(u8(k >> 8));
      o.push_back(u8(~k));
      o.push_back(u8(~k >> 8));
      o.insert(o.end(), d + from, d + from + k);
      from += k;
    } while (from < to);
  }

  void flushBlock(bool final) {
    litFreq[256]++;
    // RLE длин кодов (RFC 1951, 3.2.7)
    u8 llen[286], dlen[30];
    u32 lf[286], df[30];
    std::memcpy(lf, litFreq, sizeof lf);
    std::memcpy(df, distFreq, sizeof df);
    // Не меньше двух кодов в каждом дереве — полный код для любого распаковщика.
    auto ensureTwo = [](u32* f, int cnt) {
      int used = 0;
      for (int i = 0; i < cnt; i++) used += f[i] != 0;
      for (int i = 0; i < cnt && used < 2; i++)
        if (!f[i]) { f[i] = 1; used++; }
    };
    ensureTwo(lf, 286);
    ensureTwo(df, 30);
    huffLengths(lf, 286, 15, llen);
    huffLengths(df, 30, 15, dlen);
    int hlit = 286;
    while (hlit > 257 && llen[hlit - 1] == 0) hlit--;
    int hdist = 30;
    while (hdist > 1 && dlen[hdist - 1] == 0) hdist--;
    u8 all[316];
    std::memcpy(all, llen, size_t(hlit));
    std::memcpy(all + hlit, dlen, size_t(hdist));
    int total = hlit + hdist;
    // кодирование длин: (символ, доп. значение)
    std::vector<std::pair<u8, u8>> rle;
    rle.reserve(size_t(total));
    u32 clFreq[19] = {};
    for (int i = 0; i < total;) {
      u8 v = all[i];
      int run = 1;
      while (i + run < total && all[i + run] == v) run++;
      int r = run;
      if (v == 0) {
        while (r >= 11) { int k = std::min(r, 138); rle.push_back({18, u8(k - 11)}); clFreq[18]++; r -= k; }
        if (r >= 3) { rle.push_back({17, u8(r - 3)}); clFreq[17]++; r = 0; }
      } else {
        rle.push_back({v, 0});
        clFreq[v]++;
        r--;
        while (r >= 3) { int k = std::min(r, 6); rle.push_back({16, u8(k - 3)}); clFreq[16]++; r -= k; }
      }
      while (r-- > 0) { rle.push_back({v, 0}); clFreq[v]++; }
      i += run;
    }
    u8 clLen[19];
    huffLengths(clFreq, 19, 7, clLen);
    int hclen = 19;
    while (hclen > 4 && clLen[kClOrder[hclen - 1]] == 0) hclen--;

    // стоимость вариантов в битах
    u64 dynBits = 3 + 14 + 3 * u64(hclen) + extraBits;
    for (int s = 0; s < 19; s++) dynBits += u64(clFreq[s]) * clLen[s];
    dynBits += u64(clFreq[16]) * 2 + u64(clFreq[17]) * 3 + u64(clFreq[18]) * 7;
    for (int s = 0; s < 286; s++) dynBits += u64(litFreq[s]) * llen[s];
    for (int s = 0; s < 30; s++) dynBits += u64(distFreq[s]) * dlen[s];
    u64 fixBits = 3 + extraBits;
    for (int s = 0; s < 286; s++) fixBits += u64(litFreq[s]) * (s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8);
    for (int s = 0; s < 30; s++) fixBits += u64(distFreq[s]) * 5;
    size_t bytes = emitted - blockStart;
    u64 storedBits = u64(bytes) * 8 + u64(std::max<size_t>(1, (bytes + 65534) / 65535)) * 42;

    if (storedBits < dynBits && storedBits < fixBits) {
      storedBlocks(blockStart, emitted, final);
    } else {
      // Фиксированный код определён на 288 символах (286, 287 участвуют в канонической нумерации).
      u16 lcode[288], dcode[30];
      u8 fl[288], fd[30];
      const u8* L = llen;
      const u8* D = dlen;
      int nl = 286;
      if (fixBits <= dynBits) {
        for (int s = 0; s < 288; s++) fl[s] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
        std::fill(fd, fd + 30, u8(5));
        nl = 288;
        L = fl;
        D = fd;
        bw.put(final ? 1 : 0, 1);
        bw.put(1, 2);
      } else {
        bw.put(final ? 1 : 0, 1);
        bw.put(2, 2);
        bw.put(u32(hlit - 257), 5);
        bw.put(u32(hdist - 1), 5);
        bw.put(u32(hclen - 4), 4);
        for (int i = 0; i < hclen; i++) bw.put(clLen[kClOrder[i]], 3);
        u16 clCode[19];
        huffCodes(clLen, 19, clCode);
        for (auto& [s, x] : rle) {
          bw.put(clCode[s], clLen[s]);
          if (s == 16) bw.put(x, 2);
          else if (s == 17) bw.put(x, 3);
          else if (s == 18) bw.put(x, 7);
        }
      }
      huffCodes(L, nl, lcode);
      huffCodes(D, 30, dcode);
      const CodeTables& ct = codeTables();
      for (u32 s : syms) {
        if (!(s & 0x80000000u)) {
          bw.put(lcode[s], L[s]);
          continue;
        }
        u32 len = ((s >> 16) & 0xFF) + 3, dist = (s & 0xFFFF) + 1;
        int lc = ct.lenCode[len - 3];
        bw.put(lcode[257 + lc], L[257 + lc]);
        if (kLenExtra[lc]) bw.put(len - kLenBase[lc], kLenExtra[lc]);
        int dc = ct.dist(dist);
        bw.put(dcode[dc], D[dc]);
        if (kDistExtra[dc]) bw.put(dist - kDistBase[dc], kDistExtra[dc]);
      }
      bw.put(lcode[256], L[256]);
    }
    syms.clear();
    std::memset(litFreq, 0, sizeof litFreq);
    std::memset(distFreq, 0, sizeof distFreq);
    extraBits = 0;
    blockStart = emitted;
  }

  void run() {
    if (level == 0) {
      storedBlocks(0, n, true);
      return;
    }
    head.assign(size_t(1) << kHashBits, -1);
    prev.assign(kWin, -1);
    if (level <= 3) fast();
    else lazy();
    flushBlock(true);
  }

  void fast() {
    size_t pos = 0;
    while (pos < n) {
      u32 len = 0, dist = 0;
      if (pos + 3 <= n) {
        i32 cand = insert(pos);
        if (cand >= 0 && pos - size_t(cand) < kWin) len = longest(pos, cand, 2, dist);
      }
      if (len >= 3) {
        match(len, dist);
        if (len <= cfg.lazy) {
          for (size_t p = pos + 1; p < pos + len && p + 3 <= n; p++) insert(p);
        }
        pos += len;
      } else {
        lit(d[pos]);
        pos++;
      }
    }
  }

  void lazy() {
    size_t pos = 0;
    u32 prevLen = 2, prevDist = 0;
    bool pending = false;
    while (pos < n) {
      u32 len = 2, dist = 0;
      if (pos + 3 <= n) {
        i32 cand = insert(pos);
        if (cand >= 0 && prevLen < cfg.lazy && pos - size_t(cand) < kWin) {
          len = longest(pos, cand, prevLen, dist);
          if (len == prevLen) len = 2;  // не лучше предыдущего — совпадения нет
          if (len == 3 && dist > kTooFar) len = 2;
        }
      }
      if (prevLen >= 3 && len <= prevLen) {
        match(prevLen, prevDist);
        size_t endPos = pos - 1 + prevLen;
        for (size_t p = pos + 1; p < endPos && p + 3 <= n; p++) insert(p);
        pos = endPos;
        pending = false;
        prevLen = 2;
      } else if (pending) {
        lit(d[pos - 1]);
        prevLen = len;
        prevDist = dist;
        pos++;
      } else {
        pending = true;
        prevLen = len;
        prevDist = dist;
        pos++;
      }
    }
    if (pending) {
      if (prevLen >= 3) match(prevLen, prevDist);
      else lit(d[n - 1]);
    }
  }
};

}  // namespace

// ================================================================ API
u32 crc32(const void* data, size_t n, u32 crc) {
  const u8* p = static_cast<const u8*>(data);
  u32 c = ~crc;
  const auto& t = kCrc.t;
  while (n >= 8) {
    u32 a = ld32(p) ^ c, b = ld32(p + 4);
    c = t[7][a & 255] ^ t[6][(a >> 8) & 255] ^ t[5][(a >> 16) & 255] ^ t[4][a >> 24] ^
        t[3][b & 255] ^ t[2][(b >> 8) & 255] ^ t[1][(b >> 16) & 255] ^ t[0][b >> 24];
    p += 8;
    n -= 8;
  }
  while (n--) c = t[0][(c ^ *p++) & 255] ^ (c >> 8);
  return ~c;
}

u32 adler32(const void* data, size_t n, u32 adler) {
  const u8* p = static_cast<const u8*>(data);
  u32 a = adler & 0xFFFF, b = adler >> 16;
  while (n > 0) {
    size_t k = std::min<size_t>(n, 5552);
    n -= k;
    while (k >= 8) {
      a += p[0]; b += a;
      a += p[1]; b += a;
      a += p[2]; b += a;
      a += p[3]; b += a;
      a += p[4]; b += a;
      a += p[5]; b += a;
      a += p[6]; b += a;
      a += p[7]; b += a;
      p += 8;
      k -= 8;
    }
    while (k--) { a += *p++; b += a; }
    a %= 65521;
    b %= 65521;
  }
  return (b << 16) | a;
}

std::optional<std::vector<u8>> inflate(std::span<const u8> data, ZFormat fmt, std::string* error, const InflateOptions& opt) {
  auto z = std::make_unique<Inflater>();
  std::vector<u8> out;
  size_t first = opt.sizeHint ? opt.sizeHint : std::max<size_t>(size_t(1) << 12, data.size() * 4);
  first = std::min(first, opt.maxOutput);
  out.resize(first);
  z->vec = &out;
  z->ob = out.data();
  z->cap = out.size();
  z->maxOut = opt.maxOutput;
  z->stopAtMax = opt.stopAtMax;
  auto r = inflateImpl(*z, data, fmt, opt.verifyChecksum, error);
  if (!r) return std::nullopt;
  out.resize(*r);
  return out;
}

std::optional<size_t> inflateInto(std::span<const u8> data, ZFormat fmt, std::span<u8> out, std::string* error, bool stopAtMax,
                                  bool verifyChecksum) {
  auto z = std::make_unique<Inflater>();
  z->ob = out.data();
  z->cap = out.size();
  z->maxOut = out.size();
  z->stopAtMax = stopAtMax;
  return inflateImpl(*z, data, fmt, verifyChecksum, error);
}

std::vector<u8> deflate(std::span<const u8> data, int level, ZFormat fmt) {
  if (level < 0) level = 6;
  if (level > 9) level = 9;
  std::vector<u8> out;
  out.reserve(level == 0 ? data.size() + data.size() / 65535 * 5 + 32 : data.size() / 2 + 64);
  if (fmt == ZFormat::Zlib) {
    u8 cmf = 0x78;
    u8 flevel = level <= 1 ? 0 : level <= 5 ? 1 : level == 6 ? 2 : 3;
    u8 flg = u8(flevel << 6);
    flg = u8(flg + (31 - ((u32(cmf) << 8) | flg) % 31) % 31);
    out.push_back(cmf);
    out.push_back(flg);
  } else if (fmt == ZFormat::Gzip) {
    const u8 hdr[10] = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, u8(level == 9 ? 2 : level == 1 ? 4 : 0), 255};
    out.insert(out.end(), hdr, hdr + 10);
  }
  {
    Deflater z(data.data(), data.size(), level, out);
    z.run();
    z.bw.align();
  }
  auto be = [&](u32 v) { out.push_back(u8(v >> 24)); out.push_back(u8(v >> 16)); out.push_back(u8(v >> 8)); out.push_back(u8(v)); };
  auto le = [&](u32 v) { out.push_back(u8(v)); out.push_back(u8(v >> 8)); out.push_back(u8(v >> 16)); out.push_back(u8(v >> 24)); };
  if (fmt == ZFormat::Zlib) be(adler32(data.data(), data.size()));
  else if (fmt == ZFormat::Gzip) {
    le(crc32(data.data(), data.size()));
    le(u32(data.size()));
  }
  return out;
}

}  // namespace rg::codec
