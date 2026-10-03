// Regnum — реализация базовых утилит.
#include "base/base.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace rg {

// ---------------------------------------------------------------- Color
Color Color::mix(Color x, Color y, float t) {
  t = std::clamp(t, 0.f, 1.f);
  auto m = [t](u8 a, u8 b) { return u8(std::lround(a + (b - a) * t)); };
  return Color(m(x.r, y.r), m(x.g, y.g), m(x.b, y.b), m(x.a, y.a));
}

float Color::luminance() const {
  auto f = [](u8 c) { double x = c / 255.0; return x <= 0.03928 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4); };
  return float(0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b));
}

Color Color::textOn() const { return luminance() > 0.42f ? Color::hex(0x14161c) : Color(255, 255, 255); }

std::string Color::toHex() const {
  char buf[8];
  std::snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
  return buf;
}

std::optional<Color> Color::parse(std::string_view s) {
  if (s.empty() || s[0] != '#') return std::nullopt;
  s.remove_prefix(1);
  auto hv = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (char c : s) if (hv(c) < 0) return std::nullopt;
  if (s.size() == 3) return Color(u8(hv(s[0]) * 17), u8(hv(s[1]) * 17), u8(hv(s[2]) * 17));
  if (s.size() == 6 || s.size() == 8) {
    auto byte = [&](size_t i) { return u8(hv(s[i]) * 16 + hv(s[i + 1])); };
    return Color(byte(0), byte(2), byte(4), s.size() == 8 ? byte(6) : u8(255));
  }
  return std::nullopt;
}

Color Color::hsl(double h, double s, double l) {
  h = std::fmod(std::fmod(h, 360.0) + 360.0, 360.0);
  s = std::clamp(s, 0.0, 1.0);
  l = std::clamp(l, 0.0, 1.0);
  double a = s * std::min(l, 1 - l);
  auto f = [&](double n) {
    double k = std::fmod(n + h / 30.0, 12.0);
    return l - a * std::max(-1.0, std::min({k - 3.0, 9.0 - k, 1.0}));
  };
  return Color(u8(std::lround(255 * f(0))), u8(std::lround(255 * f(8))), u8(std::lround(255 * f(4))));
}

Color Color::palette(int n) { return hsl(std::fmod(n * 137.508 + 12.0, 360.0), 0.62, 0.52); }

Color Color::scale(const std::vector<Color>& stops, double t) {
  if (stops.empty()) return Color();
  if (stops.size() == 1) return stops[0];
  t = std::clamp(t, 0.0, 1.0);
  double seg = (stops.size() - 1) * t;
  size_t i = std::min(stops.size() - 2, size_t(seg));
  return mix(stops[i], stops[i + 1], float(seg - double(i)));
}

// ---------------------------------------------------------------- UTF-8
namespace utf8 {

u32 decode(std::string_view s, size_t& i) {
  if (i >= s.size()) return 0;
  u8 c = u8(s[i]);
  if (c < 0x80) { i++; return c; }
  int n = 0;
  u32 cp = 0;
  if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
  else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
  else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
  else { i++; return 0xFFFD; }
  for (int k = 1; k <= n; k++) {
    if (i + k >= s.size()) { i = s.size(); return 0xFFFD; }
    u8 cc = u8(s[i + k]);
    if ((cc & 0xC0) != 0x80) { i += k; return 0xFFFD; }
    cp = (cp << 6) | (cc & 0x3F);
  }
  i += n + 1;
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
  return cp;
}

void append(std::string& out, u32 cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) out.push_back(char(cp));
  else if (cp < 0x800) { out.push_back(char(0xC0 | (cp >> 6))); out.push_back(char(0x80 | (cp & 0x3F))); }
  else if (cp < 0x10000) { out.push_back(char(0xE0 | (cp >> 12))); out.push_back(char(0x80 | ((cp >> 6) & 0x3F))); out.push_back(char(0x80 | (cp & 0x3F))); }
  else { out.push_back(char(0xF0 | (cp >> 18))); out.push_back(char(0x80 | ((cp >> 12) & 0x3F))); out.push_back(char(0x80 | ((cp >> 6) & 0x3F))); out.push_back(char(0x80 | (cp & 0x3F))); }
}

std::string encode(u32 cp) { std::string s; append(s, cp); return s; }

size_t count(std::string_view s) {
  size_t n = 0;
  for (size_t i = 0; i < s.size();) { decode(s, i); n++; }
  return n;
}

size_t next(std::string_view s, size_t i) {
  if (i >= s.size()) return s.size();
  decode(s, i);
  return i;
}

size_t prev(std::string_view s, size_t i) {
  if (i == 0) return 0;
  if (i > s.size()) i = s.size();
  size_t j = i - 1;
  while (j > 0 && (u8(s[j]) & 0xC0) == 0x80 && i - j < 4) j--;
  return j;
}

bool valid(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    size_t before = i;
    u32 cp = decode(s, i);
    if (cp == 0xFFFD) {
      // настоящий U+FFFD в тексте кодируется как EF BF BD
      if (!(i - before == 3 && u8(s[before]) == 0xEF && u8(s[before + 1]) == 0xBF && u8(s[before + 2]) == 0xBD)) return false;
    }
  }
  return true;
}

std::u32string toU32(std::string_view s) {
  std::u32string r;
  r.reserve(s.size());
  for (size_t i = 0; i < s.size();) r.push_back(decode(s, i));
  return r;
}

std::string fromU32(std::u32string_view s) {
  std::string r;
  r.reserve(s.size());
  for (u32 cp : s) append(r, cp);
  return r;
}

std::wstring toWide(std::string_view s) {
  std::wstring w;
  w.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    u32 cp = decode(s, i);
    if (sizeof(wchar_t) == 2 && cp >= 0x10000) {
      cp -= 0x10000;
      w.push_back(wchar_t(0xD800 + (cp >> 10)));
      w.push_back(wchar_t(0xDC00 + (cp & 0x3FF)));
    } else {
      w.push_back(wchar_t(cp));
    }
  }
  return w;
}

std::string fromWide(std::wstring_view w) {
  std::string s;
  s.reserve(w.size());
  for (size_t i = 0; i < w.size(); i++) {
    u32 c = u32(w[i]);
    if (sizeof(wchar_t) == 2 && c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size()) {
      u32 d = u32(w[i + 1]);
      if (d >= 0xDC00 && d <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i++; }
    }
    append(s, c);
  }
  return s;
}

u32 lowerCp(u32 c) {
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (c >= 0x0410 && c <= 0x042F) return c + 32;     // А-Я
  if (c >= 0x0400 && c <= 0x040F) return c + 80;     // Ѐ-Џ (Ё)
  if (c >= 0x00C0 && c <= 0x00DE && c != 0x00D7) return c + 32;
  return c;
}

u32 upperCp(u32 c) {
  if (c >= 'a' && c <= 'z') return c - 32;
  if (c >= 0x0430 && c <= 0x044F) return c - 32;
  if (c >= 0x0450 && c <= 0x045F) return c - 80;
  if (c >= 0x00E0 && c <= 0x00FE && c != 0x00F7) return c - 32;
  return c;
}

std::string lower(std::string_view s) {
  std::string r;
  r.reserve(s.size());
  for (size_t i = 0; i < s.size();) append(r, lowerCp(decode(s, i)));
  return r;
}

std::string upper(std::string_view s) {
  std::string r;
  r.reserve(s.size());
  for (size_t i = 0; i < s.size();) append(r, upperCp(decode(s, i)));
  return r;
}

std::string searchKey(std::string_view s) {
  std::string r;
  r.reserve(s.size());
  bool space = true;
  for (size_t i = 0; i < s.size();) {
    u32 c = lowerCp(decode(s, i));
    if (c == 0x0451) c = 0x0435;  // ё -> е
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0 || c == 0x202F) {
      if (!space) { r.push_back(' '); space = true; }
      continue;
    }
    space = false;
    append(r, c);
  }
  while (!r.empty() && r.back() == ' ') r.pop_back();
  return r;
}

bool matches(std::string_view text, std::string_view query) {
  std::string q = searchKey(query);
  if (q.empty()) return true;
  std::string t = searchKey(text);
  size_t start = 0;
  while (start < q.size()) {
    size_t end = q.find(' ', start);
    if (end == std::string::npos) end = q.size();
    if (end > start && t.find(q.substr(start, end - start)) == std::string::npos) return false;
    start = end + 1;
  }
  return true;
}

bool isWordChar(u32 c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
         (c >= 0x0400 && c <= 0x04FF) || (c >= 0x00C0 && c <= 0x024F);
}

}  // namespace utf8

// ---------------------------------------------------------------- строки
std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  while (a < b && ws(s[a])) a++;
  while (b > a && ws(s[b - 1])) b--;
  return std::string(s.substr(a, b - a));
}

bool startsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }
bool endsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(s.size() - p.size()) == p; }

std::vector<std::string> split(std::string_view s, char sep) {
  std::vector<std::string> r;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); i++) {
    if (i == s.size() || s[i] == sep) { r.emplace_back(s.substr(start, i - start)); start = i + 1; }
  }
  return r;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string r;
  for (size_t i = 0; i < parts.size(); i++) { if (i) r += sep; r += parts[i]; }
  return r;
}

std::string replaceAll(std::string s, std::string_view from, std::string_view to) {
  if (from.empty()) return s;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
  return s;
}

int compareRu(std::string_view a, std::string_view b) {
  size_t i = 0, j = 0;
  int tie = 0;
  while (i < a.size() && j < b.size()) {
    size_t ii = i, jj = j;
    u32 ca = utf8::decode(a, ii), cb = utf8::decode(b, jj);
    bool da = ca >= '0' && ca <= '9', db = cb >= '0' && cb <= '9';
    if (da && db) {
      // сравнение числовых участков по значению
      size_t ea = i, eb = j;
      while (ea < a.size() && a[ea] >= '0' && a[ea] <= '9') ea++;
      while (eb < b.size() && b[eb] >= '0' && b[eb] <= '9') eb++;
      std::string_view na = a.substr(i, ea - i), nb = b.substr(j, eb - j);
      while (na.size() > 1 && na[0] == '0') na.remove_prefix(1);
      while (nb.size() > 1 && nb[0] == '0') nb.remove_prefix(1);
      if (na.size() != nb.size()) return na.size() < nb.size() ? -1 : 1;
      if (int c = na.compare(nb)) return c < 0 ? -1 : 1;
      i = ea; j = eb;
      continue;
    }
    u32 la = utf8::lowerCp(ca), lb = utf8::lowerCp(cb);
    // ё стоит сразу после е
    auto key = [](u32 c) -> double { if (c == 0x0451) return 0x0435 + 0.5; return double(c); };
    double ka = key(la), kb = key(lb);
    if (ka != kb) return ka < kb ? -1 : 1;
    if (!tie && ca != cb) tie = ca < cb ? -1 : 1;
    i = ii; j = jj;
  }
  if (i < a.size()) return 1;
  if (j < b.size()) return -1;
  return tie;
}

// ---------------------------------------------------------------- числа
static const char* kThin = "\xE2\x80\xAF";   // U+202F
static const char* kMinus = "\xE2\x88\x92";  // U+2212

std::string fmtNum(double v, int digits) {
  if (!std::isfinite(v)) return "\xE2\x80\x94";  // —
  digits = std::clamp(digits, 0, 6);
  double k = std::pow(10.0, digits);
  double r = std::round(v * k) / k;
  if (r == 0) r = 0;  // убрать −0
  bool neg = r < 0;
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", digits, std::fabs(r));
  std::string s(buf);
  std::string ip = s, fp;
  if (size_t dot = s.find('.'); dot != std::string::npos) { ip = s.substr(0, dot); fp = s.substr(dot + 1); }
  while (!fp.empty() && fp.back() == '0') fp.pop_back();
  std::string out;
  if (neg) out += kMinus;
  int n = int(ip.size());
  for (int i = 0; i < n; i++) {
    out.push_back(ip[i]);
    int left = n - 1 - i;
    if (left > 0 && left % 3 == 0 && n > 4) out += kThin;  // 4-значные числа без разделителя
  }
  if (!fp.empty()) { out.push_back(','); out += fp; }
  return out;
}

std::string fmtInt(i64 v) { return fmtNum(double(v), 0); }

std::string fmtSigned(double v, int digits) {
  double k = std::pow(10.0, std::clamp(digits, 0, 6));
  double r = std::round(v * k) / k;
  return (r > 0 ? std::string("+") : std::string()) + fmtNum(r, digits);
}

std::string fmtPct(double v, int digits, bool sign) { return (sign ? fmtSigned(v, digits) : fmtNum(v, digits)) + "%"; }

std::string fmtShort(double v) {
  if (!std::isfinite(v)) return "\xE2\x80\x94";
  double a = std::fabs(v);
  if (a >= 1e9) return fmtNum(v / 1e9, 1) + " \xD0\xBC\xD0\xBB\xD1\x80\xD0\xB4";  // млрд
  if (a >= 1e6) return fmtNum(v / 1e6, 1) + " \xD0\xBC\xD0\xBB\xD0\xBD";          // млн
  if (a >= 1e4) return fmtNum(v / 1e3, 1) + " \xD1\x82\xD1\x8B\xD1\x81.";         // тыс.
  return fmtNum(v, (a < 10 && std::fmod(a, 1.0) != 0) ? 1 : 0);
}

const char* plural(i64 n, const char* one, const char* few, const char* many) {
  i64 a = (n < 0 ? -n : n) % 100, b = a % 10;
  if (a > 10 && a < 20) return many;
  if (b > 1 && b < 5) return few;
  if (b == 1) return one;
  return many;
}

std::string nTurns(i64 n) { return fmtInt(n) + " " + plural(n, "ход", "хода", "ходов"); }

std::optional<double> parseNum(std::string_view s) {
  std::string t;
  t.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    u32 c = utf8::decode(s, i);
    if (c == ' ' || c == 0xA0 || c == 0x202F || c == 0x2009 || c == '\t') continue;
    if (c == ',') c = '.';
    if (c == 0x2212 || c == 0x2013) c = '-';
    if (c > 127) return std::nullopt;
    t.push_back(char(c));
  }
  if (t.empty() || t == "-" || t == "+" || t == ".") return std::nullopt;
  char* end = nullptr;
  double v = std::strtod(t.c_str(), &end);
  if (!end || *end != 0 || !std::isfinite(v)) return std::nullopt;
  return v;
}

std::string strf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = std::vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  std::string s;
  if (n > 0) {
    s.resize(size_t(n) + 1);
    std::vsnprintf(s.data(), s.size(), fmt, ap2);
    s.resize(size_t(n));
  }
  va_end(ap2);
  return s;
}

// ---------------------------------------------------------------- время, хеш
double nowSeconds() {
  using namespace std::chrono;
  static const auto t0 = steady_clock::now();
  return duration<double>(steady_clock::now() - t0).count();
}

std::string nowIso() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

u64 hash64(const void* data, size_t n, u64 seed) {
  const u8* p = static_cast<const u8*>(data);
  u64 h = 0xcbf29ce484222325ull ^ seed;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
  h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
  return h;
}

// ---------------------------------------------------------------- ошибки и журнал
void fail(const std::string& userMessage) { throw UserError(userMessage); }

static std::mutex gLogMutex;
static std::FILE* gLogFile = nullptr;

void setLogFile(const std::string& path) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  if (gLogFile) { std::fclose(gLogFile); gLogFile = nullptr; }
  if (path.empty()) return;
#ifdef _WIN32
  gLogFile = _wfopen(utf8::toWide(path).c_str(), L"ab");
#else
  gLogFile = std::fopen(path.c_str(), "ab");
#endif
}

void logMsg(LogLevel lvl, const std::string& msg) {
  static const char* names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
  std::lock_guard<std::mutex> lock(gLogMutex);
  std::string line = nowIso() + " [" + names[int(lvl)] + "] " + msg + "\n";
  std::fwrite(line.data(), 1, line.size(), stderr);
  if (gLogFile) { std::fwrite(line.data(), 1, line.size(), gLogFile); std::fflush(gLogFile); }
}

#define RG_LOG_IMPL(level)                    \
  va_list ap;                                 \
  va_start(ap, fmt);                          \
  char buf[2048];                             \
  std::vsnprintf(buf, sizeof buf, fmt, ap);   \
  va_end(ap);                                 \
  logMsg(level, buf);

void logInfo(const char* fmt, ...) { RG_LOG_IMPL(LogLevel::Info) }
void logWarn(const char* fmt, ...) { RG_LOG_IMPL(LogLevel::Warn) }
void logError(const char* fmt, ...) { RG_LOG_IMPL(LogLevel::Error) }

}  // namespace rg
