// Regnum — JSON: разбор и запись.
#include "base/json.h"

#include <charconv>
#include <new>

namespace rg::json {

namespace {

// Длина корректной последовательности UTF-8 (1..4) или 0 — строгая проверка (без «длинных» форм и суррогатов).
inline int utf8Seq(const u8* p, const u8* end) {
  u8 c = p[0];
  if (c < 0x80) return 1;
  if (c < 0xC2) return 0;
  ptrdiff_t left = end - p;
  if (c < 0xE0) return (left >= 2 && (p[1] & 0xC0) == 0x80) ? 2 : 0;
  if (c < 0xF0) {
    if (left < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
    if (c == 0xE0 && p[1] < 0xA0) return 0;
    if (c == 0xED && p[1] > 0x9F) return 0;
    return 3;
  }
  if (c < 0xF5) {
    if (left < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
    if (c == 0xF0 && p[1] < 0x90) return 0;
    if (c == 0xF4 && p[1] > 0x8F) return 0;
    return 4;
  }
  return 0;
}

constexpr char kReplacement[] = "\xEF\xBF\xBD";

inline size_t keyHash(std::string_view s) { return std::hash<std::string_view>()(s); }

const std::string kEmptyStr;
const Array kEmptyArr;
const std::vector<Member> kEmptyMembers;

}  // namespace

// Объект: члены по порядку + хеш-индекс (открытая адресация, индексы в m) для больших объектов.
struct Value::Object {
  std::vector<Member> m;
  std::vector<u32> slots;  // пусто — индекса нет; иначе степень двойки, kNone — свободно
  static constexpr u32 kNone = 0xFFFFFFFFu;
  static constexpr size_t kIndexFrom = 12;

  void rebuild() {
    slots.clear();
    if (m.size() < kIndexFrom) return;
    size_t cap = 16;
    while (cap < m.size() * 2) cap <<= 1;
    slots.assign(cap, kNone);
    for (u32 i = 0; i < u32(m.size()); i++) insertSlot(i);
  }
  void insertSlot(u32 i) {
    size_t mask = slots.size() - 1;
    size_t h = keyHash(m[i].first) & mask;
    while (slots[h] != kNone) h = (h + 1) & mask;
    slots[h] = i;
  }
  // Вызывать после push_back в m.
  void added() {
    if (slots.empty()) {
      if (m.size() >= kIndexFrom) rebuild();
      return;
    }
    if ((m.size()) * 2 > slots.size()) rebuild();
    else insertSlot(u32(m.size() - 1));
  }
  ptrdiff_t find(std::string_view key) const {
    if (slots.empty()) {
      for (size_t i = 0; i < m.size(); i++)
        if (m[i].first == key) return ptrdiff_t(i);
      return -1;
    }
    size_t mask = slots.size() - 1;
    size_t h = keyHash(key) & mask;
    while (slots[h] != kNone) {
      if (m[slots[h]].first == key) return ptrdiff_t(slots[h]);
      h = (h + 1) & mask;
    }
    return -1;
  }
  // Удалить член i: слот — удалением со сдвигом назад (без хеширования всех ключей), индексы за i уменьшаются.
  void erase(size_t i) {
    if (!slots.empty()) {
      size_t mask = slots.size() - 1;
      size_t j = keyHash(m[i].first) & mask;
      while (slots[j] != u32(i)) j = (j + 1) & mask;
      slots[j] = kNone;
      for (size_t k = (j + 1) & mask; slots[k] != kNone; k = (k + 1) & mask) {
        size_t home = keyHash(m[slots[k]].first) & mask;
        // элемент k можно перенести в дыру j, если его «дом» не лежит в (j, k]
        bool between = j <= k ? (home > j && home <= k) : (home > j || home <= k);
        if (!between) {
          slots[j] = slots[k];
          slots[k] = kNone;
          j = k;
        }
      }
      for (u32& s : slots)
        if (s != kNone && s > u32(i)) s--;
    }
    m.erase(m.begin() + ptrdiff_t(i));
    if (m.size() < kIndexFrom / 2) slots.clear();
  }
};

// ================================================================ Value: жизненный цикл
Value::Value(Array a) : t_(Type::Array) { new (&a_) Array(std::move(a)); }

Value::Value(const Value& o) : t_(Type::Null), n_(0) { copyFrom(o); }

Value::Value(Value&& o) noexcept : t_(o.t_) {
  switch (t_) {
    case Type::Null: n_ = 0; break;
    case Type::Bool: b_ = o.b_; break;
    case Type::Number: n_ = o.n_; break;
    case Type::String: new (&s_) std::string(std::move(o.s_)); o.s_.~basic_string(); break;
    case Type::Array: new (&a_) Array(std::move(o.a_)); o.a_.~Array(); break;
    case Type::Object: o_ = o.o_; break;
  }
  o.t_ = Type::Null;
  o.n_ = 0;
}

Value& Value::operator=(const Value& o) {
  if (this != &o) {
    Value tmp(o);
    *this = std::move(tmp);
  }
  return *this;
}

Value& Value::operator=(Value&& o) noexcept {
  if (this != &o) {
    Value tmp(std::move(o));  // o может быть частью *this
    destroy();
    new (this) Value(std::move(tmp));
  }
  return *this;
}

Value::~Value() { destroy(); }

void Value::destroy() noexcept {
  switch (t_) {
    case Type::String: s_.~basic_string(); break;
    case Type::Array: a_.~Array(); break;
    case Type::Object: delete o_; break;
    default: break;
  }
  t_ = Type::Null;
  n_ = 0;
}

void Value::copyFrom(const Value& o) {
  // вызывается только для пустого (null) значения
  switch (o.t_) {
    case Type::Null: break;
    case Type::Bool: b_ = o.b_; break;
    case Type::Number: n_ = o.n_; break;
    case Type::String: new (&s_) std::string(o.s_); break;
    case Type::Array: new (&a_) Array(o.a_); break;
    case Type::Object: o_ = new Object(*o.o_); break;
  }
  t_ = o.t_;
}

Value Value::array(std::initializer_list<Value> items) { return Value(Array(items)); }

Value Value::object() {
  Value v;
  v.o_ = new Object();
  v.t_ = Type::Object;
  return v;
}

Value Value::object(std::initializer_list<Member> members) {
  Value v = object();
  for (auto& m : members) v.set(m.first, m.second);
  return v;
}

// ================================================================ Value: доступ
i64 Value::asInt(i64 def) const {
  if (t_ != Type::Number || !std::isfinite(n_)) return def;
  double r = std::nearbyint(n_);
  if (r < -9223372036854775808.0 || r >= 9223372036854775808.0) return def;
  return i64(r);
}

const std::string& Value::asStr() const& { return t_ == Type::String ? s_ : kEmptyStr; }

std::string Value::asStr() && { return t_ == Type::String ? std::move(s_) : std::string(); }

size_t Value::size() const {
  if (t_ == Type::Array) return a_.size();
  if (t_ == Type::Object) return o_->m.size();
  return 0;
}

const Value& Value::nullValue() {
  static const Value kNull;
  return kNull;
}

const Value& Value::element(size_t i) const {
  if (t_ == Type::Array && i < a_.size()) return a_[i];
  return nullValue();
}

Value& Value::elementGrow(size_t i) {
  Array& a = items();
  if (i >= a.size()) a.resize(i + 1);
  return a[i];
}

Value& Value::at(size_t i) {
  if (t_ != Type::Array || i >= a_.size()) throw std::out_of_range("json: индекс вне массива");
  return a_[i];
}

const Array& Value::items() const { return t_ == Type::Array ? a_ : kEmptyArr; }

Array& Value::items() {
  if (t_ == Type::Null) { new (&a_) Array(); t_ = Type::Array; }
  if (t_ != Type::Array) throw std::logic_error("json: значение не массив");
  return a_;
}

const std::vector<Member>& Value::members() const { return t_ == Type::Object ? o_->m : kEmptyMembers; }

Value::Object& Value::objRef() {
  if (t_ == Type::Null) { o_ = new Object(); t_ = Type::Object; }
  if (t_ != Type::Object) throw std::logic_error("json: значение не объект");
  return *o_;
}

const Value* Value::find(std::string_view key) const {
  if (t_ != Type::Object) return nullptr;
  ptrdiff_t i = o_->find(key);
  return i < 0 ? nullptr : &o_->m[size_t(i)].second;
}

Value* Value::find(std::string_view key) {
  if (t_ != Type::Object) return nullptr;
  ptrdiff_t i = o_->find(key);
  return i < 0 ? nullptr : &o_->m[size_t(i)].second;
}

const Value& Value::get(std::string_view key) const {
  static const Value kNull;
  const Value* v = find(key);
  return v ? *v : kNull;
}

const Value& Value::obj(std::string_view key) const {
  static const Value kEmptyObj = Value::object();
  const Value* v = find(key);
  return v && v->isObj() ? *v : kEmptyObj;
}

Value& Value::set(std::string key, Value v) {
  Object& o = objRef();
  ptrdiff_t i = o.find(key);
  if (i >= 0) {
    o.m[size_t(i)].second = std::move(v);
    return o.m[size_t(i)].second;
  }
  o.m.emplace_back(std::move(key), std::move(v));
  o.added();
  return o.m.back().second;
}

Value& Value::operator[](std::string_view key) {
  Object& o = objRef();
  ptrdiff_t i = o.find(key);
  if (i >= 0) return o.m[size_t(i)].second;
  o.m.emplace_back(std::string(key), Value());
  o.added();
  return o.m.back().second;
}

bool Value::erase(std::string_view key) {
  if (t_ != Type::Object) return false;
  ptrdiff_t i = o_->find(key);
  if (i < 0) return false;
  o_->erase(size_t(i));
  return true;
}

Value& Value::push(Value v) {
  Array& a = items();
  a.push_back(std::move(v));
  return a.back();
}

void Value::clear() {
  if (t_ == Type::Array) a_.clear();
  else if (t_ == Type::Object) { o_->m.clear(); o_->slots.clear(); }
}

bool Value::operator==(const Value& o) const {
  if (t_ != o.t_) return false;
  switch (t_) {
    case Type::Null: return true;
    case Type::Bool: return b_ == o.b_;
    case Type::Number: return n_ == o.n_;
    case Type::String: return s_ == o.s_;
    case Type::Array: return a_ == o.a_;
    case Type::Object: {
      if (o_->m.size() != o.o_->m.size()) return false;
      for (auto& m : o_->m) {
        const Value* x = o.find(m.first);
        if (!x || !(*x == m.second)) return false;
      }
      return true;
    }
  }
  return false;
}

// ================================================================ разбор
namespace {
constexpr double kPow10[23] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                               1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
}  // namespace

// Рекурсивный спуск. Элементы массивов и члены объектов копятся в общих стеках и переносятся
// в контейнер точного размера (без перераспределений). Parser — друг Value.
struct Parser {
  const char* begin;
  const char* p;
  const char* end;
  int depth = 0;
  int maxDepth = 512;
  std::vector<Value> vstack;
  std::vector<Member> mstack;

  [[noreturn]] void error(const char* at, const char* what) {
    if (at > end) at = end;
    int line = 1;
    const char* ls = begin;
    for (const char* q = begin; q < at; q++)
      if (*q == '\n') { line++; ls = q + 1; }
    int col = 1;
    for (const char* q = ls; q < at;) {
      int n = utf8Seq(reinterpret_cast<const u8*>(q), reinterpret_cast<const u8*>(at));
      q += n ? n : 1;
      col++;
    }
    throw ParseError(strf("JSON, строка %d, столбец %d: %s", line, col, what), line, col);
  }

  void ws() {
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
  }

  static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  int hex4(const char* q) {
    if (end - q < 4) error(q, "неполная последовательность \\u");
    int v = 0;
    for (int i = 0; i < 4; i++) {
      int h = hexv(q[i]);
      if (h < 0) error(q + i, "неверная последовательность \\u");
      v = v * 16 + h;
    }
    return v;
  }

  // p указывает на открывающую кавычку.
  void string(std::string& out) {
    const char* start = p;
    p++;
    for (;;) {
      const char* run = p;
      while (p < end) {
        u8 c = u8(*p);
        if (c == '"' || c == '\\' || c < 0x20 || c >= 0x80) break;
        p++;
      }
      if (p > run) out.append(run, size_t(p - run));
      if (p >= end) error(start, "незакрытая строка");
      u8 c = u8(*p);
      if (c == '"') { p++; return; }
      if (c < 0x20) error(p, "управляющий символ внутри строки");
      if (c >= 0x80) {
        int n = utf8Seq(reinterpret_cast<const u8*>(p), reinterpret_cast<const u8*>(end));
        if (n) { out.append(p, size_t(n)); p += n; }
        else { out.append(kReplacement, 3); p++; }
        continue;
      }
      // экранирование
      if (end - p < 2) error(start, "незакрытая строка");
      char e = p[1];
      p += 2;
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          u32 cp = u32(hex4(p));
          p += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
              u32 lo = u32(hex4(p + 2));
              if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
              } else {
                cp = 0xFFFD;  // одиночная старшая половина
              }
            } else {
              cp = 0xFFFD;
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;  // одиночная младшая половина
          }
          utf8::append(out, cp);
          break;
        }
        default: error(p - 1, "неверная управляющая последовательность");
      }
    }
  }

  double number() {
    const char* s = p;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    const char* intStart = p;
    if (p >= end || !(*p >= '0' && *p <= '9')) error(s, "неверное число");
    if (*p == '0') {
      p++;
      if (p < end && *p >= '0' && *p <= '9') error(s, "неверное число: ведущий ноль");
    } else {
      while (p < end && *p >= '0' && *p <= '9') p++;
    }
    const char* intEnd = p;
    // Мантисса из значащих цифр (до 19) для быстрого точного пути.
    u64 mant = 0;
    int sig = 0, fracDigits = 0;
    bool exact = true;
    auto digit = [&](char ch) {
      if (mant == 0 && ch == '0') return;  // ведущие нули не значащие
      if (sig < 19) {
        mant = mant * 10 + u64(ch - '0');
        sig++;
      } else {
        exact = false;
      }
    };
    for (const char* q = intStart; q < intEnd; q++) digit(*q);
    bool negExp = false;
    if (p < end && *p == '.') {
      p++;
      if (p >= end || !(*p >= '0' && *p <= '9')) error(p, "неверное число: нет цифр после точки");
      while (p < end && *p >= '0' && *p <= '9') {
        digit(*p);
        fracDigits++;
        p++;
      }
    }
    int expv = 0;
    if (p < end && (*p == 'e' || *p == 'E')) {
      p++;
      if (p < end && (*p == '+' || *p == '-')) { negExp = *p == '-'; p++; }
      if (p >= end || !(*p >= '0' && *p <= '9')) error(p, "неверное число: нет цифр порядка");
      while (p < end && *p >= '0' && *p <= '9') {
        if (expv < 100000) expv = expv * 10 + (*p - '0');
        p++;
      }
      if (negExp) expv = -expv;
    }
    // Точный путь (Клингер): мантисса ≤ 2^53 и |порядок| ≤ 22 — одно умножение или деление без ошибки округления.
    int e10 = expv - fracDigits;
    if (exact && mant <= (u64(1) << 53) && e10 >= -22 && e10 <= 22) {
      double v = double(mant);
      v = e10 < 0 ? v / kPow10[-e10] : v * kPow10[e10];
      return neg ? -v : v;
    }
    double v = 0;
    auto r = std::from_chars(s, p, v);
    if (r.ec == std::errc::result_out_of_range) {
      bool zeroInt = intEnd - intStart == 1 && *intStart == '0';
      if (negExp || zeroInt) return neg ? -0.0 : 0.0;
      error(s, "число вне допустимого диапазона");
    }
    if (r.ec != std::errc() || r.ptr != p) error(s, "неверное число");
    return v;
  }

  bool lit(const char* word, size_t n) {
    if (size_t(end - p) >= n && std::memcmp(p, word, n) == 0) { p += n; return true; }
    return false;
  }

  void value(Value& out) {
    ws();
    if (p >= end) error(p, "неожиданный конец текста: ожидалось значение");
    char c = *p;
    switch (c) {
      case '{': object(out); return;
      case '[': array(out); return;
      case '"': {
        std::string s;
        string(s);
        out = Value(std::move(s));
        return;
      }
      case 't': if (lit("true", 4)) { out = Value(true); return; } break;
      case 'f': if (lit("false", 5)) { out = Value(false); return; } break;
      case 'n': if (lit("null", 4)) { out = Value(); return; } break;
      default:
        if (c == '-' || (c >= '0' && c <= '9')) { out = Value(number()); return; }
        break;
    }
    error(p, "ожидалось значение");
  }

  void enter() {
    if (++depth > maxDepth) error(p, "слишком глубокая вложенность");
  }

  void array(Value& out) {
    enter();
    p++;
    const size_t base = vstack.size();
    ws();
    if (p < end && *p == ']') {
      p++;
    } else {
      for (;;) {
        Value v;
        value(v);
        vstack.push_back(std::move(v));
        ws();
        if (p >= end) error(p, "неожиданный конец текста: ожидалась запятая или «]»");
        if (*p == ',') { p++; continue; }
        if (*p == ']') { p++; break; }
        error(p, "ожидалась запятая или «]»");
      }
    }
    Array a;
    a.reserve(vstack.size() - base);
    for (size_t i = base; i < vstack.size(); i++) a.push_back(std::move(vstack[i]));
    vstack.resize(base);
    out = Value(std::move(a));
    depth--;
  }

  void object(Value& out) {
    enter();
    p++;
    const size_t base = mstack.size();
    ws();
    if (p < end && *p == '}') {
      p++;
    } else {
      for (;;) {
        ws();
        if (p >= end) error(p, "неожиданный конец текста: ожидался ключ");
        if (*p != '"') error(p, "ожидался ключ в кавычках");
        std::string key;
        string(key);
        ws();
        if (p >= end || *p != ':') error(p, "ожидалось двоеточие");
        p++;
        Value v;
        value(v);
        mstack.emplace_back(std::move(key), std::move(v));
        ws();
        if (p >= end) error(p, "неожиданный конец текста: ожидалась запятая или «}»");
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
        error(p, "ожидалась запятая или «}»");
      }
    }
    Value obj = Value::object();
    Value::Object& o = *obj.o_;
    o.m.reserve(mstack.size() - base);
    for (size_t i = base; i < mstack.size(); i++) {
      ptrdiff_t j = o.find(mstack[i].first);
      if (j >= 0) {
        o.m[size_t(j)].second = std::move(mstack[i].second);  // повтор ключа: последнее значение, первая позиция
      } else {
        o.m.push_back(std::move(mstack[i]));
        o.added();
      }
    }
    mstack.resize(base);
    out = std::move(obj);
    depth--;
  }
};

Value parse(std::string_view text, const ParseOptions& opt) {
  Parser ps;
  ps.begin = text.data();
  ps.p = text.data();
  ps.end = text.data() + text.size();
  ps.maxDepth = opt.maxDepth;
  if (text.size() >= 3 && std::memcmp(text.data(), "\xEF\xBB\xBF", 3) == 0) ps.p += 3;  // BOM
  Value v;
  ps.value(v);
  ps.ws();
  if (ps.p != ps.end) ps.error(ps.p, "лишние данные после значения");
  return v;
}

std::optional<Value> tryParse(std::string_view text, std::string* error, const ParseOptions& opt) {
  try {
    return parse(text, opt);
  } catch (const ParseError& e) {
    if (error) *error = e.what();
    return std::nullopt;
  }
}

// ================================================================ запись
void appendNumber(std::string& out, double v) {
  char buf[40];
  if (!std::isfinite(v)) { out += "null"; return; }
  if (v == std::floor(v) && std::fabs(v) < 9007199254740992.0) {
    auto r = std::to_chars(buf, buf + sizeof buf, i64(v));
    out.append(buf, size_t(r.ptr - buf));
    return;
  }
  auto r = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, size_t(r.ptr - buf));
}

void appendString(std::string& out, std::string_view s) {
  static const char* kHex = "0123456789abcdef";
  out.push_back('"');
  const u8* p = reinterpret_cast<const u8*>(s.data());
  const u8* e = p + s.size();
  while (p < e) {
    const u8* run = p;
    while (p < e && *p >= 0x20 && *p < 0x80 && *p != '"' && *p != '\\') p++;
    if (p > run) out.append(reinterpret_cast<const char*>(run), size_t(p - run));
    if (p >= e) break;
    u8 c = *p;
    if (c >= 0x80) {
      int n = utf8Seq(p, e);
      if (n) { out.append(reinterpret_cast<const char*>(p), size_t(n)); p += n; }
      else { out.append(kReplacement, 3); p++; }
      continue;
    }
    p++;
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: {
        char u[6] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 15]};
        out.append(u, 6);
      }
    }
  }
  out.push_back('"');
}

namespace {

struct Writer {
  std::string& out;
  const WriteOptions& opt;

  void newline(int level) {
    out.push_back('\n');
    out.append(size_t(level) * size_t(opt.indent), ' ');
  }

  static bool scalar(const Value& v) { return !v.isArr() && !v.isObj(); }

  void value(const Value& v, int level) {
    switch (v.type()) {
      case Type::Null: out += "null"; break;
      case Type::Bool: out += v.asBool() ? "true" : "false"; break;
      case Type::Number: appendNumber(out, v.asNum()); break;
      case Type::String: appendString(out, v.asStr()); break;
      case Type::Array: array(v.items(), level); break;
      case Type::Object: object(v.members(), level); break;
    }
  }

  void array(const Array& a, int level) {
    if (a.empty()) { out += "[]"; return; }
    bool pretty = opt.indent > 0;
    bool oneLine = !pretty || (opt.compactNumbersArrays && std::all_of(a.begin(), a.end(), scalar));
    out.push_back('[');
    for (size_t i = 0; i < a.size(); i++) {
      if (i) out += (pretty && oneLine) ? ", " : ",";
      if (!oneLine) newline(level + 1);
      value(a[i], level + 1);
    }
    if (!oneLine) newline(level);
    out.push_back(']');
  }

  void object(const std::vector<Member>& m, int level) {
    if (m.empty()) { out += "{}"; return; }
    bool pretty = opt.indent > 0;
    std::vector<const Member*> order;
    order.reserve(m.size());
    for (auto& x : m) order.push_back(&x);
    if (opt.sortKeys) std::stable_sort(order.begin(), order.end(), [](const Member* a, const Member* b) { return a->first < b->first; });
    out.push_back('{');
    for (size_t i = 0; i < order.size(); i++) {
      if (i) out.push_back(',');
      if (pretty) newline(level + 1);
      appendString(out, order[i]->first);
      out += pretty ? ": " : ":";
      value(order[i]->second, level + 1);
    }
    if (pretty) newline(level);
    out.push_back('}');
  }
};

}  // namespace

void write(std::string& out, const Value& v, const WriteOptions& opt) {
  Writer w{out, opt};
  w.value(v, 0);
}

std::string write(const Value& v, const WriteOptions& opt) {
  std::string out;
  write(out, v, opt);
  return out;
}

}  // namespace rg::json
