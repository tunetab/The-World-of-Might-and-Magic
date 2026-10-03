// Regnum — минимальный тестовый фреймворк (без внешних библиотек).
//
//   TEST(geo_split_square) { CHECK(x > 0); CHECK_EQ(a, b); CHECK_NEAR(v, 1.0, 1e-9); CHECK_THROWS(f()); }
//
// Запуск: regnum-tests [фильтр ...] — выполняются тесты, в имени которых есть подстрока фильтра.
// Каталог для артефактов (PNG и т. п.): rg::test::outDir() (по умолчанию .wmma/regnum-tests).
#pragma once
#include <cstdlib>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "base/base.h"

namespace rg::test {

struct Case { const char* name; const char* file; int line; std::function<void()> fn; };
std::vector<Case>& registry();
struct Reg { Reg(const char* n, const char* f, int l, std::function<void()> fn) { registry().push_back({n, f, l, std::move(fn)}); } };

struct Failure { std::string msg; };
void fail(const char* file, int line, const std::string& msg);  // бросает Failure
std::string outDir();                                            // каталог артефактов (создаётся)
// Порог производительности: строгий при REGNUM_PERF_STRICT=1, иначе с запасом ×4 (машина может быть загружена
// параллельными сборками — тест ловит только катастрофическое замедление).
inline double perf(double limit) {
  const char* v = std::getenv("REGNUM_PERF_STRICT");
  return (v && *v && *v != '0') ? limit : limit * 4;
}

template <class T> std::string show(const T& v) {
  if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view> || std::is_same_v<T, const char*>) return "\"" + std::string(v) + "\"";
  else if constexpr (std::is_enum_v<T>) return std::to_string(int(v));
  else if constexpr (requires(std::ostream& o, const T& x) { o << x; }) { std::ostringstream s; s << v; return s.str(); }
  else return "<?>";
}

}  // namespace rg::test

#define RG_CAT2(a, b) a##b
#define RG_CAT(a, b) RG_CAT2(a, b)
#define TEST(name)                                                                              \
  static void RG_CAT(test_fn_, name)();                                                          \
  static ::rg::test::Reg RG_CAT(test_reg_, name)(#name, __FILE__, __LINE__, RG_CAT(test_fn_, name)); \
  static void RG_CAT(test_fn_, name)()

#define CHECK(cond) do { if (!(cond)) ::rg::test::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); } while (0)
#define CHECK_MSG(cond, msg) do { if (!(cond)) ::rg::test::fail(__FILE__, __LINE__, std::string("CHECK(" #cond "): ") + (msg)); } while (0)
#define CHECK_EQ(a, b) do { auto&& _a = (a); auto&& _b = (b); if (!(_a == _b)) ::rg::test::fail(__FILE__, __LINE__, std::string("CHECK_EQ(" #a ", " #b "): ") + ::rg::test::show(_a) + " != " + ::rg::test::show(_b)); } while (0)
#define CHECK_NEAR(a, b, eps) do { double _a = double(a), _b = double(b); if (!(std::fabs(_a - _b) <= (eps))) ::rg::test::fail(__FILE__, __LINE__, std::string("CHECK_NEAR(" #a ", " #b "): ") + std::to_string(_a) + " vs " + std::to_string(_b)); } while (0)
#define CHECK_THROWS(expr) do { bool _t = false; try { (void)(expr); } catch (...) { _t = true; } if (!_t) ::rg::test::fail(__FILE__, __LINE__, "CHECK_THROWS(" #expr ")"); } while (0)
