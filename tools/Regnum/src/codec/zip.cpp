// Regnum — ZIP: чтение и запись.
#include "codec/zip.h"

#include "base/fs.h"
#include "codec/zlib.h"

namespace rg::codec {

namespace {

constexpr u32 kSigLocal = 0x04034B50, kSigCentral = 0x02014B50, kSigEnd = 0x06054B50, kSigLocator64 = 0x07064B50;

inline u16 rd16(const u8* p) { return u16(p[0] | (p[1] << 8)); }
inline u32 rd32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }

constexpr u16 kCp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

std::string fromCp437(std::string_view s) {
  std::string r;
  for (char c : s) {
    u8 b = u8(c);
    if (b < 0x80) r.push_back(c);
    else utf8::append(r, kCp437[b - 0x80]);
  }
  return r;
}

// Строгая проверка UTF-8 (без «длинных» форм).
bool validUtf8(std::string_view s) {
  const u8* p = reinterpret_cast<const u8*>(s.data());
  const u8* e = p + s.size();
  while (p < e) {
    u8 c = *p;
    int n = c < 0x80 ? 1 : c < 0xC2 ? 0 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : c < 0xF5 ? 4 : 0;
    if (!n || e - p < n) return false;
    for (int k = 1; k < n; k++)
      if ((p[k] & 0xC0) != 0x80) return false;
    if (n == 3 && ((c == 0xE0 && p[1] < 0xA0) || (c == 0xED && p[1] > 0x9F))) return false;
    if (n == 4 && ((c == 0xF0 && p[1] < 0x90) || (c == 0xF4 && p[1] > 0x8F))) return false;
    p += n;
  }
  return true;
}

// Дни от 1970-01-01 для даты григорианского календаря и обратно.
i64 daysFromCivil(i64 y, unsigned m, unsigned d) {
  y -= m <= 2;
  const i64 era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = unsigned(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + i64(doe) - 719468;
}

void civilFromDays(i64 z, i64& y, unsigned& m, unsigned& d) {
  z += 719468;
  const i64 era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = unsigned(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = i64(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
}

i64 fromDos(u16 time, u16 date) {
  unsigned day = date & 31, mon = (date >> 5) & 15;
  i64 year = 1980 + (date >> 9);
  if (mon < 1 || mon > 12 || day < 1) return 0;
  i64 days = daysFromCivil(year, mon, day);
  return days * 86400 + i64((time >> 11) & 31) * 3600 + i64((time >> 5) & 63) * 60 + i64(time & 31) * 2;
}

void toDos(i64 t, u16& time, u16& date) {
  const i64 lo = 315532800;               // 1980-01-01
  const i64 hi = 4354819198;              // 2107-12-31 23:59:58
  if (t < lo) t = lo;
  if (t > hi) t = hi;
  i64 days = t / 86400, sec = t % 86400;
  i64 y;
  unsigned m, d;
  civilFromDays(days, y, m, d);
  date = u16(((y - 1980) << 9) | (m << 5) | d);
  time = u16(((sec / 3600) << 11) | (((sec / 60) % 60) << 5) | ((sec % 60) / 2));
}

bool fail(std::string* error, const std::string& m) {
  if (error) *error = "ZIP: " + m;
  return false;
}

}  // namespace

// ================================================================ чтение
bool ZipReader::openFile(const std::string& path, std::string* error) {
  std::string err;
  auto data = fs::readFile(path, &err);
  if (!data) return fail(error, err);
  return open(std::move(*data), error);
}

bool ZipReader::open(std::string archive, std::string* error) {
  data_ = std::move(archive);
  entries_.clear();
  comment_.clear();
  index_.clear();
  const u8* d = reinterpret_cast<const u8*>(data_.data());
  const u64 n = data_.size();
  if (n < 22) return fail(error, "файл слишком мал или не является архивом");
  // Запись конца каталога: последняя, у которой длина комментария помещается в файл.
  i64 eocd = -1;
  u64 stop = n > 22 + 65535 ? n - 22 - 65535 : 0;
  for (u64 p = n - 22 + 1; p-- > stop;) {
    if (rd32(d + p) == kSigEnd && p + 22 + rd16(d + p + 20) <= n) {
      eocd = i64(p);
      break;
    }
  }
  if (eocd < 0) return fail(error, "не найден конец каталога — файл не является архивом или повреждён");
  const u8* e = d + eocd;
  u16 disk = rd16(e + 4), cdDisk = rd16(e + 6), onDisk = rd16(e + 8), total = rd16(e + 10);
  u32 cdSize = rd32(e + 12), cdOff = rd32(e + 16);
  u16 clen = rd16(e + 20);
  comment_.assign(reinterpret_cast<const char*>(e + 22), clen);
  if (!validUtf8(comment_)) comment_ = fromCp437(comment_);
  bool z64 = eocd >= 20 && rd32(d + eocd - 20) == kSigLocator64;
  if (z64 || total == 0xFFFF || onDisk == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu)
    return fail(error, "формат ZIP64 не поддерживается");
  if (disk != 0 || cdDisk != 0 || onDisk != total) return fail(error, "многотомные архивы не поддерживаются");
  if (u64(cdSize) > u64(eocd)) return fail(error, "повреждён центральный каталог");
  // Каталог ожидается прямо перед концом; разница со смещением — данные перед архивом (самораспаковывающиеся).
  auto centralAt = [&](u64 pos) { return total == 0 || (pos + 4 <= u64(eocd) && rd32(d + pos) == kSigCentral); };
  u64 cdPos = u64(eocd) - cdSize;
  if (cdPos >= cdOff && centralAt(cdPos)) {
    base_ = cdPos - cdOff;
  } else if (u64(cdOff) + cdSize <= u64(eocd) && centralAt(cdOff)) {
    cdPos = cdOff;
    base_ = 0;
  } else {
    return fail(error, "повреждён центральный каталог");
  }
  cdStart_ = cdPos;
  u64 p = cdPos;
  const u64 cdEnd = cdPos + cdSize;
  entries_.reserve(total);
  for (u32 i = 0; i < total; i++) {
    if (p + 46 > cdEnd || rd32(d + p) != kSigCentral) return fail(error, "повреждён центральный каталог");
    const u8* c = d + p;
    u16 flags = rd16(c + 8), method = rd16(c + 10), tm = rd16(c + 12), dt = rd16(c + 14);
    u32 crc = rd32(c + 16), packed = rd32(c + 20), size = rd32(c + 24);
    u16 nlen = rd16(c + 28), xlen = rd16(c + 30), mlen = rd16(c + 32);
    u32 ext = rd32(c + 38), off = rd32(c + 42);
    if (p + 46 + nlen + xlen + mlen > cdEnd) return fail(error, "повреждён центральный каталог");
    if (packed == 0xFFFFFFFFu || size == 0xFFFFFFFFu || off == 0xFFFFFFFFu) return fail(error, "формат ZIP64 не поддерживается");
    std::string raw(reinterpret_cast<const char*>(c + 46), nlen);
    ZipEntry en;
    en.method = method;
    en.crc = crc;
    en.packedSize = packed;
    en.size = size;
    en.encrypted = flags & 1;
    en.localOffset = off;
    en.mtime = fromDos(tm, dt);
    // Имя: UTF-8 по флагу или по содержимому, иначе CP437; поле Info-ZIP Unicode Path (0x7075) имеет приоритет.
    if ((flags & 0x800) || validUtf8(raw)) en.name = validUtf8(raw) ? raw : fromCp437(raw);
    else en.name = fromCp437(raw);
    const u8* x = c + 46 + nlen;
    for (u32 q = 0; q + 4 <= xlen;) {
      u16 id = rd16(x + q), sz = rd16(x + q + 2);
      if (q + 4 + sz > xlen) break;
      const u8* f = x + q + 4;
      if (id == 0x7075 && sz >= 5 && f[0] == 1 && rd32(f + 1) == crc32(raw)) {
        std::string un(reinterpret_cast<const char*>(f + 5), sz - 5u);
        if (validUtf8(un)) en.name = un;
      } else if (id == 0x5455 && sz >= 5 && (f[0] & 1)) {
        en.mtime = i64(i32(rd32(f + 1)));
      } else if (id == 0x0001) {
        return fail(error, "формат ZIP64 не поддерживается");
      }
      q += 4u + sz;
    }
    for (char& ch : en.name)
      if (ch == '\\') ch = '/';
    en.dir = !en.name.empty() && en.name.back() == '/';
    if (!en.dir && (ext & 0x10) && size == 0 && packed == 0) {
      en.dir = true;
      en.name.push_back('/');
    }
    if (!index_.count(en.name)) index_.emplace(en.name, int(entries_.size()));
    entries_.push_back(std::move(en));
    p += 46u + nlen + xlen + mlen;
  }
  return true;
}

int ZipReader::find(std::string_view name) const {
  auto it = index_.find(std::string(name));
  return it == index_.end() ? -1 : it->second;
}

std::optional<std::string> ZipReader::read(std::string_view name, std::string* error) const {
  int i = find(name);
  if (i < 0) {
    fail(error, "нет записи «" + std::string(name) + "»");
    return std::nullopt;
  }
  return read(size_t(i), error);
}

std::optional<std::string> ZipReader::read(size_t index, std::string* error) const {
  if (index >= entries_.size()) {
    fail(error, "неверный номер записи");
    return std::nullopt;
  }
  const ZipEntry& en = entries_[index];
  auto bad = [&](const std::string& m) -> std::optional<std::string> {
    fail(error, "«" + en.name + "»: " + m);
    return std::nullopt;
  };
  if (en.encrypted) return bad("зашифрованные записи не поддерживаются");
  if (en.method != 0 && en.method != 8) return bad(strf("метод сжатия %d не поддерживается", int(en.method)));
  if (en.size > maxEntrySize) return bad("запись слишком большая");
  const u8* d = reinterpret_cast<const u8*>(data_.data());
  const u64 lh = en.localOffset + base_;
  if (lh + 30 > cdStart_ || rd32(d + lh) != kSigLocal) return bad("повреждён локальный заголовок");
  u64 start = lh + 30 + rd16(d + lh + 26) + rd16(d + lh + 28);
  if (start + en.packedSize > cdStart_) return bad("данные выходят за пределы архива");
  std::span<const u8> packed(d + start, size_t(en.packedSize));
  std::string out;
  if (en.method == 0) {
    if (en.packedSize != en.size) return bad("неверный размер несжатой записи");
    out.assign(reinterpret_cast<const char*>(packed.data()), packed.size());
  } else {
    out.resize(size_t(en.size));
    std::string zerr;
    auto got = inflateInto(packed, ZFormat::Raw, std::span<u8>(reinterpret_cast<u8*>(out.data()), out.size()), &zerr);
    if (!got) return bad(zerr);
    if (*got != en.size) return bad("размер распакованных данных не совпадает");
  }
  if (crc32(out) != en.crc) return bad("контрольная сумма не совпадает");
  return out;
}

bool safeZipPath(std::string_view name) {
  if (name.empty() || name.size() > 4096) return false;
  if (name[0] == '/' || name[0] == '\\') return false;
  if (name.size() >= 2 && name[1] == ':') return false;  // диск Windows
  if (name.find('\\') != std::string_view::npos || name.find('\0') != std::string_view::npos) return false;
  size_t s = 0;
  while (s <= name.size()) {
    size_t e = name.find('/', s);
    if (e == std::string_view::npos) e = name.size();
    std::string_view part = name.substr(s, e - s);
    if (part == "..") return false;
    s = e + 1;
  }
  return validUtf8(name);
}

// ================================================================ запись
bool ZipWriter::add(std::string_view name, std::string_view data, i64 mtime, std::string* error) {
  return addImpl(name, reinterpret_cast<const u8*>(data.data()), data.size(), false, mtime, error);
}

bool ZipWriter::add(std::string_view name, std::span<const u8> data, i64 mtime, std::string* error) {
  return addImpl(name, data.data(), data.size(), false, mtime, error);
}

bool ZipWriter::addDir(std::string_view name, i64 mtime, std::string* error) {
  std::string n(name);
  if (!n.empty() && n.back() != '/') n.push_back('/');
  return addImpl(n, nullptr, 0, true, mtime, error);
}

bool ZipWriter::addImpl(std::string_view nameIn, const u8* data, size_t n, bool dir, i64 mtime, std::string* error) {
  std::string name(nameIn);
  std::string_view check = dir && !name.empty() ? std::string_view(name).substr(0, name.size() - 1) : std::string_view(name);
  if (!safeZipPath(check) || name.size() > 65535) return fail(error, "недопустимое имя записи «" + name + "»");
  if (names_.count(name)) return fail(error, "повторное имя записи «" + name + "»");
  if (central_.size() >= 65535) return fail(error, "слишком много записей");
  if (n >= 0xFFFFFFFFu) return fail(error, "запись больше 4 ГБ");

  u32 crc = n ? crc32(data, n) : 0;
  std::vector<u8> z;
  u16 method = 0;
  const u8* payload = data;
  size_t plen = n;
  if (!dir && n > 0 && level_ > 0) {
    z = deflate(std::span<const u8>(data, n), level_, ZFormat::Raw);
    if (z.size() < n) {
      method = 8;
      payload = z.data();
      plen = z.size();
    }
  }
  if (u64(out_.size()) + 30 + name.size() + plen + 46 * (central_.size() + 1) > 0xFFFFFFFEull)
    return fail(error, "архив больше 4 ГБ");

  bool utf = false;
  for (char c : name)
    if (u8(c) >= 0x80) utf = true;
  u16 flags = utf ? 0x800 : 0;
  u16 tm, dt;
  toDos(mtime, tm, dt);
  Central c{name, crc, u32(plen), u32(n), method, tm, dt, u32(out_.size()), dir};

  auto p16 = [&](u16 v) { out_.push_back(char(v)); out_.push_back(char(v >> 8)); };
  auto p32 = [&](u32 v) { p16(u16(v)); p16(u16(v >> 16)); };
  p32(kSigLocal);
  p16(20);
  p16(flags);
  p16(method);
  p16(tm);
  p16(dt);
  p32(crc);
  p32(u32(plen));
  p32(u32(n));
  p16(u16(name.size()));
  p16(0);
  out_ += name;
  if (plen) out_.append(reinterpret_cast<const char*>(payload), plen);
  names_.emplace(name, int(central_.size()));
  central_.push_back(std::move(c));
  return true;
}

std::string ZipWriter::finish(std::string_view comment) {
  std::string out = std::move(out_);
  auto p16 = [&](u16 v) { out.push_back(char(v)); out.push_back(char(v >> 8)); };
  auto p32 = [&](u32 v) { p16(u16(v)); p16(u16(v >> 16)); };
  u32 cdOff = u32(out.size());
  for (auto& c : central_) {
    bool utf = false;
    for (char ch : c.name)
      if (u8(ch) >= 0x80) utf = true;
    p32(kSigCentral);
    p16(0x0314);  // создан: Unix, версия 2.0
    p16(20);
    p16(utf ? 0x800 : 0);
    p16(c.method);
    p16(c.time);
    p16(c.date);
    p32(c.crc);
    p32(c.packed);
    p32(c.size);
    p16(u16(c.name.size()));
    p16(0);
    p16(0);
    p16(0);
    p16(0);
    p32(c.dir ? ((0040755u << 16) | 0x10) : (0100644u << 16));
    p32(c.offset);
    out += c.name;
  }
  u32 cdSize = u32(out.size() - cdOff);
  if (comment.size() > 65535) comment = comment.substr(0, 65535);
  p32(kSigEnd);
  p16(0);
  p16(0);
  p16(u16(central_.size()));
  p16(u16(central_.size()));
  p32(cdSize);
  p32(cdOff);
  p16(u16(comment.size()));
  out += comment;
  central_.clear();
  names_.clear();
  out_.clear();
  return out;
}

}  // namespace rg::codec
