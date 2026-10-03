// Regnum — разборщик путей SVG: однопроходный сканер чисел и флагов, отражение контрольных точек S/T,
// дуги через Path::arcTo. Числа читаются собственным кодом (без локали и from_chars для float).
#include "gfx/svgpath.h"

namespace rg::gfx {

namespace {

inline bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }

class Scanner {
 public:
  explicit Scanner(std::string_view s) : s_(s) {}

  size_t pos() const { return i_; }
  bool done() { skipWs(); return i_ >= s_.size(); }
  char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }
  void advance() { i_++; }

  void skipWs() { while (i_ < s_.size() && isWs(s_[i_])) i_++; }
  // Пробелы и не более одной запятой между аргументами.
  void skipSep() {
    skipWs();
    if (i_ < s_.size() && s_[i_] == ',') { i_++; skipWs(); }
  }
  // Начинается ли с текущей позиции число (после пробелов).
  bool numberAhead() {
    skipWs();
    if (i_ >= s_.size()) return false;
    char c = s_[i_];
    if (isDigit(c) || c == '.') return true;
    if (c == '+' || c == '-') {
      if (i_ + 1 >= s_.size()) return false;
      char n = s_[i_ + 1];
      return isDigit(n) || n == '.';
    }
    return false;
  }

  // Число SVG: знак? (цифры [. цифры?] | . цифры) ([eE] знак? цифры)?
  bool number(double& out) {
    skipWs();
    size_t j = i_;
    bool neg = false;
    if (j < s_.size() && (s_[j] == '+' || s_[j] == '-')) { neg = s_[j] == '-'; j++; }
    u64 mant = 0;
    int exp10 = 0, digits = 0;
    bool any = false;
    while (j < s_.size() && isDigit(s_[j])) {
      if (digits < 19) { mant = mant * 10 + u64(s_[j] - '0'); if (mant) digits++; }
      else exp10++;
      any = true;
      j++;
    }
    if (j < s_.size() && s_[j] == '.') {
      j++;
      while (j < s_.size() && isDigit(s_[j])) {
        if (digits < 19) { mant = mant * 10 + u64(s_[j] - '0'); exp10--; if (mant) digits++; }
        any = true;
        j++;
      }
    }
    if (!any) return false;
    if (j < s_.size() && (s_[j] == 'e' || s_[j] == 'E')) {
      size_t k = j + 1;
      bool eneg = false;
      if (k < s_.size() && (s_[k] == '+' || s_[k] == '-')) { eneg = s_[k] == '-'; k++; }
      if (k < s_.size() && isDigit(s_[k])) {
        int e = 0;
        while (k < s_.size() && isDigit(s_[k])) {
          if (e < 100000) e = e * 10 + (s_[k] - '0');
          k++;
        }
        exp10 += eneg ? -e : e;
        j = k;
      }
      // «1e» без цифр: число заканчивается до «e», дальше будет ошибка неизвестной команды.
    }
    double v = double(mant);
    if (mant != 0 && exp10 != 0) {
      if (exp10 < -400) v = 0;
      else if (exp10 > 400) v = kInf;
      else v *= std::pow(10.0, double(exp10));
    }
    if (!std::isfinite(v) || std::fabs(v) > 3.0e38) return false;  // вне диапазона float
    out = neg ? -v : v;
    i_ = j;
    return true;
  }

  // Флаг дуги: одиночный символ 0 или 1 (может быть слит со следующим числом).
  bool flag(bool& out) {
    skipWs();
    if (i_ < s_.size() && (s_[i_] == '0' || s_[i_] == '1')) { out = s_[i_] == '1'; i_++; return true; }
    return false;
  }

 private:
  std::string_view s_;
  size_t i_ = 0;
};

inline bool isCommand(char c) {
  switch (c) {
    case 'M': case 'm': case 'L': case 'l': case 'H': case 'h': case 'V': case 'v': case 'C': case 'c':
    case 'S': case 's': case 'Q': case 'q': case 'T': case 't': case 'A': case 'a': case 'Z': case 'z': return true;
    default: return false;
  }
}

// Число аргументов команды (без учёта повторов).
inline int argCount(char upper) {
  switch (upper) {
    case 'M': case 'L': case 'T': return 2;
    case 'H': case 'V': return 1;
    case 'C': return 6;
    case 'S': case 'Q': return 4;
    case 'A': return 7;
    default: return 0;
  }
}

}  // namespace

bool parseSvgPath(std::string_view d, Path& out, SvgPathError* err) {
  Scanner sc(d);
  auto fail = [&](size_t pos, const char* msg) {
    if (err) { err->pos = pos; err->msg = msg; }
    return false;
  };
  Pt cur{0, 0}, start{0, 0};
  Pt lastCtrl{0, 0};
  char prev = 0;            // предыдущая команда (верхний регистр) — для отражения S/T
  bool open = false;        // есть начатый подпуть
  bool closed = false;      // последней была Z (следующая команда рисования начинает новый подпуть)
  char cmd = 0;
  bool first = true;

  // Новый подпуть после Z без M начинается в начальной точке предыдущего.
  auto ensureStart = [&]() {
    if (closed || !open) {
      out.moveTo(start.x, start.y);
      cur = start;
      open = true;
      closed = false;
    }
  };

  while (!sc.done()) {
    size_t cmdPos = sc.pos();
    char c = sc.peek();
    if (isCommand(c)) {
      cmd = c;
      sc.advance();
    } else if (sc.numberAhead() && cmd != 0 && cmd != 'Z' && cmd != 'z') {
      // Неявный повтор: после M/m — L/l.
      if (cmd == 'M') cmd = 'L';
      else if (cmd == 'm') cmd = 'l';
    } else {
      return fail(cmdPos, cmd == 'Z' || cmd == 'z' ? "после Z ожидается команда" : "ожидается команда пути");
    }
    if (first && cmd != 'M' && cmd != 'm') return fail(cmdPos, "путь должен начинаться с M");
    first = false;

    const char up = char(cmd >= 'a' ? cmd - 32 : cmd);
    const bool rel = cmd >= 'a';
    if (up == 'Z') {
      if (open && !closed) {
        out.close();
        cur = start;
        closed = true;
      }
      prev = 'Z';
      continue;
    }

    // Аргументы читаются целиком до изменения контура: неполная команда отбрасывается.
    double a[7] = {0, 0, 0, 0, 0, 0, 0};
    bool fl[2] = {false, false};
    const int n = argCount(up);
    for (int k = 0; k < n; k++) {
      if (k > 0) sc.skipSep();
      size_t ap = sc.pos();
      if (up == 'A' && (k == 3 || k == 4)) {
        if (!sc.flag(fl[k - 3])) return fail(ap, "ожидается флаг дуги 0 или 1");
        continue;
      }
      if (!sc.number(a[k])) return fail(ap, "ожидается число");
    }
    sc.skipWs();
    if (sc.peek() == ',') {
      // Запятая после последнего аргумента допустима только перед следующим числом.
      sc.advance();
      if (!sc.numberAhead()) return fail(sc.pos(), "лишняя запятая");
    }

    const float ox = rel ? cur.x : 0.f, oy = rel ? cur.y : 0.f;
    switch (up) {
      case 'M': {
        Pt p{float(a[0]) + ox, float(a[1]) + oy};
        out.moveTo(p.x, p.y);
        cur = start = p;
        open = true;
        closed = false;
        lastCtrl = cur;
        break;
      }
      case 'L': {
        ensureStart();
        Pt p{float(a[0]) + ox, float(a[1]) + oy};
        out.lineTo(p.x, p.y);
        cur = p;
        break;
      }
      case 'H': {
        ensureStart();
        Pt p{float(a[0]) + ox, cur.y};
        out.lineTo(p.x, p.y);
        cur = p;
        break;
      }
      case 'V': {
        ensureStart();
        Pt p{cur.x, float(a[0]) + (rel ? cur.y : 0.f)};
        out.lineTo(p.x, p.y);
        cur = p;
        break;
      }
      case 'C': {
        ensureStart();
        Pt c1{float(a[0]) + ox, float(a[1]) + oy}, c2{float(a[2]) + ox, float(a[3]) + oy}, p{float(a[4]) + ox, float(a[5]) + oy};
        out.cubicTo(c1.x, c1.y, c2.x, c2.y, p.x, p.y);
        lastCtrl = c2;
        cur = p;
        break;
      }
      case 'S': {
        ensureStart();
        Pt c1 = (prev == 'C' || prev == 'S') ? Pt{2 * cur.x - lastCtrl.x, 2 * cur.y - lastCtrl.y} : cur;
        Pt c2{float(a[0]) + ox, float(a[1]) + oy}, p{float(a[2]) + ox, float(a[3]) + oy};
        out.cubicTo(c1.x, c1.y, c2.x, c2.y, p.x, p.y);
        lastCtrl = c2;
        cur = p;
        break;
      }
      case 'Q': {
        ensureStart();
        Pt q{float(a[0]) + ox, float(a[1]) + oy}, p{float(a[2]) + ox, float(a[3]) + oy};
        out.quadTo(q.x, q.y, p.x, p.y);
        lastCtrl = q;
        cur = p;
        break;
      }
      case 'T': {
        ensureStart();
        Pt q = (prev == 'Q' || prev == 'T') ? Pt{2 * cur.x - lastCtrl.x, 2 * cur.y - lastCtrl.y} : cur;
        Pt p{float(a[0]) + ox, float(a[1]) + oy};
        out.quadTo(q.x, q.y, p.x, p.y);
        lastCtrl = q;
        cur = p;
        break;
      }
      case 'A': {
        ensureStart();
        Pt p{float(a[5]) + ox, float(a[6]) + oy};
        // Path::arcTo: нулевой радиус — отрезок, совпадающие концы — пропуск, малые радиусы увеличиваются.
        out.arcTo(float(std::fabs(a[0])), float(std::fabs(a[1])), float(a[2]), fl[0], fl[1], p.x, p.y);
        cur = p;
        break;
      }
      default: break;
    }
    prev = up;
  }
  return true;
}

Path svgPath(std::string_view d) {
  Path p;
  parseSvgPath(d, p);
  return p;
}

std::string svgNum(double v, int digits) {
  if (!std::isfinite(v)) return "0";
  digits = std::clamp(digits, 0, 9);
  double scale = 1;
  for (int i = 0; i < digits; i++) scale *= 10;
  double r = std::round(std::fabs(v) * scale);
  if (r == 0) return "0";
  std::string s;
  if (v < 0) s += '-';
  // Целая часть и дробь из округлённого целого (без printf и локали).
  if (r >= 9.0e18) {
    // Огромные значения: только целая часть.
    double ip = std::floor(std::fabs(v));
    std::string t;
    while (ip >= 1) { double q = std::floor(ip / 10); t += char('0' + int(ip - q * 10)); ip = q; }
    std::reverse(t.begin(), t.end());
    return s + (t.empty() ? "0" : t);
  }
  u64 n = u64(r), div = u64(scale);
  u64 ip = n / div, fp = n % div;
  s += std::to_string(ip);
  if (fp) {
    std::string f = std::to_string(fp);
    f.insert(0, size_t(digits) - f.size(), '0');
    while (!f.empty() && f.back() == '0') f.pop_back();
    s += '.';
    s += f;
  }
  return s;
}

std::string toSvgPath(const Path& p, int digits) {
  std::string s;
  auto pt = [&](Pt q) {
    s += svgNum(q.x, digits);
    s += ' ';
    s += svgNum(q.y, digits);
  };
  size_t pi = 0;
  const size_t np = p.pts.size();
  for (Path::Verb v : p.verbs) {
    if (!s.empty()) s += ' ';
    switch (v) {
      case Path::Move: if (pi + 1 > np) return s; s += 'M'; pt(p.pts[pi++]); break;
      case Path::Line: if (pi + 1 > np) return s; s += 'L'; pt(p.pts[pi++]); break;
      case Path::Quad:
        if (pi + 2 > np) return s;
        s += 'Q'; pt(p.pts[pi]); s += ' '; pt(p.pts[pi + 1]);
        pi += 2;
        break;
      case Path::Cubic:
        if (pi + 3 > np) return s;
        s += 'C'; pt(p.pts[pi]); s += ' '; pt(p.pts[pi + 1]); s += ' '; pt(p.pts[pi + 2]);
        pi += 3;
        break;
      case Path::Close: s += 'Z'; break;
    }
  }
  return s;
}

}  // namespace rg::gfx
