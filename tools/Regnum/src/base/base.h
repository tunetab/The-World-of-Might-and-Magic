// Regnum — базовые типы и утилиты (только стандартная библиотека C++20).
// Строки везде UTF-8 (std::string). Координаты карты — double, экранные — float.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rg {

using i8 = int8_t; using i16 = int16_t; using i32 = int32_t; using i64 = int64_t;
using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;
using f32 = float; using f64 = double;

constexpr double kPi = 3.14159265358979323846;
constexpr double kInf = std::numeric_limits<double>::infinity();

template <class T> constexpr T clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
template <class T> constexpr T lerp(T a, T b, T t) { return a + (b - a) * t; }

// ---------------------------------------------------------------- геометрия
struct Vec2 {
  double x = 0, y = 0;
  constexpr Vec2() = default;
  constexpr Vec2(double x_, double y_) : x(x_), y(y_) {}
  constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
  constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
  constexpr Vec2 operator*(double k) const { return {x * k, y * k}; }
  constexpr Vec2 operator/(double k) const { return {x / k, y / k}; }
  constexpr Vec2 operator-() const { return {-x, -y}; }
  Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
  Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
  Vec2& operator*=(double k) { x *= k; y *= k; return *this; }
  constexpr bool operator==(const Vec2& o) const = default;
  constexpr double dot(Vec2 o) const { return x * o.x + y * o.y; }
  constexpr double cross(Vec2 o) const { return x * o.y - y * o.x; }
  constexpr double len2() const { return x * x + y * y; }
  double len() const { return std::sqrt(len2()); }
  Vec2 norm() const { double l = len(); return l > 0 ? Vec2{x / l, y / l} : Vec2{}; }
  constexpr Vec2 perp() const { return {-y, x}; }
};
inline double dist(Vec2 a, Vec2 b) { return (a - b).len(); }
inline double dist2(Vec2 a, Vec2 b) { return (a - b).len2(); }

// Ограничивающий прямоугольник (координаты карты). Пустой: x0 > x1.
struct Box2 {
  double x0 = kInf, y0 = kInf, x1 = -kInf, y1 = -kInf;
  constexpr Box2() = default;
  constexpr Box2(double ax, double ay, double bx, double by) : x0(ax), y0(ay), x1(bx), y1(by) {}
  bool empty() const { return x0 > x1 || y0 > y1; }
  void add(Vec2 p) { x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y); }
  void add(const Box2& b) { if (b.empty()) return; x0 = std::min(x0, b.x0); y0 = std::min(y0, b.y0); x1 = std::max(x1, b.x1); y1 = std::max(y1, b.y1); }
  bool contains(Vec2 p) const { return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1; }
  bool intersects(const Box2& b) const { return !(b.x0 > x1 || b.x1 < x0 || b.y0 > y1 || b.y1 < y0); }
  Box2 inflated(double d) const { return empty() ? *this : Box2{x0 - d, y0 - d, x1 + d, y1 + d}; }
  double w() const { return empty() ? 0 : x1 - x0; }
  double h() const { return empty() ? 0 : y1 - y0; }
  Vec2 center() const { return {(x0 + x1) * 0.5, (y0 + y1) * 0.5}; }
};

// Экранный прямоугольник (пиксели интерфейса, float).
struct RectF {
  float x = 0, y = 0, w = 0, h = 0;
  constexpr RectF() = default;
  constexpr RectF(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}
  constexpr float right() const { return x + w; }
  constexpr float bottom() const { return y + h; }
  constexpr float cx() const { return x + w * 0.5f; }
  constexpr float cy() const { return y + h * 0.5f; }
  constexpr bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
  constexpr bool empty() const { return w <= 0 || h <= 0; }
  RectF inset(float d) const { return {x + d, y + d, std::max(0.f, w - 2 * d), std::max(0.f, h - 2 * d)}; }
  RectF inset(float dx, float dy) const { return {x + dx, y + dy, std::max(0.f, w - 2 * dx), std::max(0.f, h - 2 * dy)}; }
  RectF expand(float d) const { return {x - d, y - d, w + 2 * d, h + 2 * d}; }
  RectF intersect(const RectF& o) const {
    float ax = std::max(x, o.x), ay = std::max(y, o.y), bx = std::min(right(), o.right()), by = std::min(bottom(), o.bottom());
    return {ax, ay, std::max(0.f, bx - ax), std::max(0.f, by - ay)};
  }
  // Отрезать полосу от края (для раскладки): возвращает полосу, сам прямоугольник уменьшается.
  RectF cutLeft(float a) { a = std::min(a, w); RectF r{x, y, a, h}; x += a; w -= a; return r; }
  RectF cutRight(float a) { a = std::min(a, w); RectF r{right() - a, y, a, h}; w -= a; return r; }
  RectF cutTop(float a) { a = std::min(a, h); RectF r{x, y, w, a}; y += a; h -= a; return r; }
  RectF cutBottom(float a) { a = std::min(a, h); RectF r{x, bottom() - a, w, a}; h -= a; return r; }
  constexpr bool operator==(const RectF& o) const = default;
};

// ---------------------------------------------------------------- цвет
// Неpremultiplied RGBA 8 бит. В изображениях gfx хранится premultiplied (см. gfx.h).
struct Color {
  u8 r = 0, g = 0, b = 0, a = 255;
  constexpr Color() = default;
  constexpr Color(u8 r_, u8 g_, u8 b_, u8 a_ = 255) : r(r_), g(g_), b(b_), a(a_) {}
  // 0xRRGGBB (непрозрачный)
  static constexpr Color hex(u32 rgb) { return Color(u8(rgb >> 16), u8(rgb >> 8), u8(rgb), 255); }
  // 0xRRGGBBAA
  static constexpr Color hexa(u32 rgba) { return Color(u8(rgba >> 24), u8(rgba >> 16), u8(rgba >> 8), u8(rgba)); }
  constexpr Color alpha(float k) const { return Color(r, g, b, u8(std::clamp(a * k, 0.f, 255.f) + 0.5f)); }
  constexpr Color withA(u8 na) const { return Color(r, g, b, na); }
  constexpr bool operator==(const Color& o) const = default;
  static Color mix(Color x, Color y, float t);      // линейно по каналам, включая альфу
  Color lighten(float t) const { return mix(*this, Color(255, 255, 255, a), t); }
  Color darken(float t) const { return mix(*this, Color(0, 0, 0, a), t); }
  float luminance() const;                           // относительная яркость WCAG, 0..1
  Color textOn() const;                              // контрастный цвет текста (тёмный/светлый)
  std::string toHex() const;                         // "#rrggbb"
  static std::optional<Color> parse(std::string_view s);  // "#rgb", "#rrggbb", "#rrggbbaa"
  static Color hsl(double h, double s, double l);    // h 0..360, s,l 0..1
  static Color palette(int n);                       // различимый цвет для n-й сущности
  static Color scale(const std::vector<Color>& stops, double t); // градиент по шкале t∈[0,1]
};

// ---------------------------------------------------------------- UTF-8
namespace utf8 {
// Декодировать кодовую точку с позиции i и сдвинуть i. Некорректный байт -> U+FFFD.
u32 decode(std::string_view s, size_t& i);
void append(std::string& out, u32 cp);
std::string encode(u32 cp);
size_t count(std::string_view s);                    // число кодовых точек
size_t next(std::string_view s, size_t i);           // байтовая позиция следующего символа
size_t prev(std::string_view s, size_t i);           // байтовая позиция предыдущего символа
bool valid(std::string_view s);
std::u32string toU32(std::string_view s);
std::string fromU32(std::u32string_view s);
std::wstring toWide(std::string_view s);             // для API Windows (UTF-16)
std::string fromWide(std::wstring_view s);
u32 lowerCp(u32 cp);                                 // латиница, кириллица, ё
u32 upperCp(u32 cp);
std::string lower(std::string_view s);
std::string upper(std::string_view s);
std::string searchKey(std::string_view s);           // нижний регистр, ё→е, сжатые пробелы
bool matches(std::string_view text, std::string_view query); // все слова запроса входят в текст
bool isWordChar(u32 cp);
}  // namespace utf8

// ---------------------------------------------------------------- строки и числа
std::string trim(std::string_view s);
bool startsWith(std::string_view s, std::string_view p);
bool endsWith(std::string_view s, std::string_view p);
std::vector<std::string> split(std::string_view s, char sep);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string replaceAll(std::string s, std::string_view from, std::string_view to);
int compareRu(std::string_view a, std::string_view b);  // алфавитное сравнение без учёта регистра, числа по значению

// Русское форматирование чисел: разделитель тысяч — узкий пробел, дробная часть через запятую, минус «−».
std::string fmtNum(double v, int digits = 0);
std::string fmtInt(i64 v);
std::string fmtSigned(double v, int digits = 0);      // +12, −5, 0
std::string fmtPct(double v, int digits = 0, bool sign = false);
std::string fmtShort(double v);                       // 12,5 тыс., 3,1 млн
const char* plural(i64 n, const char* one, const char* few, const char* many);
std::string nTurns(i64 n);                            // «3 хода»
std::optional<double> parseNum(std::string_view s);   // принимает пробелы, запятую, «−»
std::string strf(const char* fmt, ...);               // printf в std::string

// ---------------------------------------------------------------- время, хеши, случайность
double nowSeconds();                                  // монотонные секунды
std::string nowIso();                                 // «2026-10-01T12:00:00Z»
u64 hash64(const void* data, size_t n, u64 seed = 0);
inline u64 hash64(std::string_view s, u64 seed = 0) { return hash64(s.data(), s.size(), seed); }
inline u64 hashMix(u64 a, u64 b) { a ^= b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2); return a; }

// Детерминированный ГПСЧ (splitmix64).
struct Rng {
  u64 s;
  explicit Rng(u64 seed = 0x1234567) : s(seed) {}
  u64 next() { u64 z = (s += 0x9e3779b97f4a7c15ull); z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull; z = (z ^ (z >> 27)) * 0x94d049bb133111ebull; return z ^ (z >> 31); }
  double uniform() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
  int range(int lo, int hi) { return lo + int(next() % u64(hi - lo + 1)); }
};

// ---------------------------------------------------------------- ошибки и журнал
// Ошибка, сообщение которой можно показать пользователю как есть (по-русски).
struct UserError : std::runtime_error { using std::runtime_error::runtime_error; };
[[noreturn]] void fail(const std::string& userMessage);

enum class LogLevel { Debug, Info, Warn, Error };
void logMsg(LogLevel lvl, const std::string& msg);
void logInfo(const char* fmt, ...);
void logWarn(const char* fmt, ...);
void logError(const char* fmt, ...);
void setLogFile(const std::string& path);             // дублировать журнал в файл (UTF-8)

}  // namespace rg
