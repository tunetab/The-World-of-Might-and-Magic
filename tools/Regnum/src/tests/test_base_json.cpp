// Тесты JSON: разбор (числа, строки, \u и суррогаты, некорректный UTF-8), ошибки со строкой и столбцом,
// порядок ключей, запись (отступы, сортировка, компактные массивы), точность чисел, доступ, скорость на 10+ МБ.
#include <cstdio>
#include <cstring>

#include "base/json.h"
#include "tests/test.h"

using namespace rg;
using json::Value;

namespace {

int errLine(std::string_view text, int* col = nullptr, std::string* msg = nullptr) {
  try {
    json::parse(text);
  } catch (const json::ParseError& e) {
    if (col) *col = e.column;
    if (msg) *msg = e.what();
    return e.line;
  }
  return 0;
}

u64 bitsOf(double d) {
  u64 b;
  std::memcpy(&b, &d, 8);
  return b;
}

}  // namespace

TEST(base_json_scalars_and_numbers) {
  CHECK(json::parse("null").isNull());
  CHECK(json::parse(" true ").asBool());
  CHECK(!json::parse("false").asBool(true));
  CHECK_EQ(json::parse("0").asNum(), 0.0);
  CHECK_EQ(json::parse("-17").asNum(), -17.0);
  CHECK_EQ(json::parse("1.5").asNum(), 1.5);
  CHECK_EQ(json::parse("2.5e3").asNum(), 2500.0);
  CHECK_EQ(json::parse("1E-2").asNum(), 0.01);
  CHECK_EQ(json::parse("-0.0").asNum(), 0.0);
  CHECK_EQ(json::parse("123456789012345678").asNum(), 123456789012345678.0);
  CHECK_EQ(json::parse("9007199254740993").asNum(), 9007199254740992.0);  // округление к ближайшему double
  CHECK_EQ(json::parse("0.1").asNum(), 0.1);
  CHECK_EQ(json::parse("1e-400").asNum(), 0.0);
  CHECK_EQ(json::parse("[1e-400]")[0].asNum(), 0.0);
  CHECK_EQ(errLine("1e400"), 1);
  for (const char* bad : {"01", "1.", ".5", "-", "+1", "1e", "1e+", "0x10", "--1", "NaN", "Infinity", "1.2.3"}) CHECK_MSG(errLine(bad) == 1, bad);
  CHECK_EQ(json::parse("42").asInt(), i64(42));
  CHECK_EQ(json::parse("2.6").asInt(), i64(3));
  CHECK_EQ(json::parse("-2.5").asInt(), i64(-2));  // к чётному
  CHECK_EQ(json::parse("1e30").asInt(7), i64(7));
  CHECK_EQ(json::parse("\"x\"").asInt(5), i64(5));
}

TEST(base_json_strings_and_unicode) {
  CHECK_EQ(json::parse(R"("a\"b\\c\/d\b\f\n\r\t")").asStr(), std::string("a\"b\\c/d\b\f\n\r\t"));
  CHECK_EQ(json::parse(R"("AЖé")").asStr(), std::string("AЖé"));
  CHECK_EQ(json::parse(R"("😀")").asStr(), std::string("\xF0\x9F\x98\x80"));
  CHECK_EQ(json::parse(R"("\uD83D")").asStr(), std::string("\xEF\xBF\xBD"));
  CHECK_EQ(json::parse(R"("\uDE00x")").asStr(), std::string("\xEF\xBF\xBDx"));
  CHECK_EQ(json::parse(R"("\uD83DA")").asStr(), std::string("\xEF\xBF\xBD" "A"));
  CHECK_EQ(json::parse("\"Арден ё Ё\"").asStr(), std::string("Арден ё Ё"));
  // некорректный UTF-8 заменяется на U+FFFD
  CHECK_EQ(json::parse("\"\xC3\x28\"").asStr(), std::string("\xEF\xBF\xBD("));
  CHECK_EQ(json::parse("\"\xC0\xAF\"").asStr(), std::string("\xEF\xBF\xBD\xEF\xBF\xBD"));
  CHECK_EQ(json::parse("\"\xED\xA0\x80\"").asStr(), std::string("\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"));
  CHECK_EQ(json::parse("\"\xF4\x90\x80\x80\"").asStr().substr(0, 3), std::string("\xEF\xBF\xBD"));
  CHECK_EQ(json::parse("\"abc\xE2\x82\"").asStr(), std::string("abc\xEF\xBF\xBD\xEF\xBF\xBD"));
  // ошибки
  CHECK_EQ(errLine("\"abc"), 1);
  CHECK_EQ(errLine("\"a\x01b\""), 1);
  CHECK_EQ(errLine(R"("\x")"), 1);
  CHECK_EQ(errLine(R"("\u12")"), 1);
  CHECK_EQ(errLine(R"("\u12G4")"), 1);
  // BOM
  CHECK_EQ(json::parse("\xEF\xBB\xBF{\"a\":1}").num("a"), 1.0);
}

TEST(base_json_error_positions) {
  int col = 0;
  std::string msg;
  CHECK_EQ(errLine("{\n  \"a\": 1,\n  \"b\" 2\n}", &col, &msg), 3);
  CHECK_EQ(col, 7);
  CHECK(msg.find("строка 3, столбец 7") != std::string::npos);
  CHECK(msg.find("двоеточие") != std::string::npos);
  CHECK_EQ(errLine("{\"имя\": x}", &col, &msg), 1);
  CHECK_EQ(col, 9);  // столбец в символах, не в байтах
  CHECK_EQ(errLine("[1, 2,]", &col), 1);
  CHECK_EQ(col, 7);
  CHECK_EQ(errLine("[1 2]", &col, &msg), 1);
  CHECK(msg.find("запятая") != std::string::npos);
  CHECK_EQ(errLine("{\"a\":1}\n\n  x", &col, &msg), 3);
  CHECK_EQ(col, 3);
  CHECK(msg.find("лишние") != std::string::npos);
  CHECK_EQ(errLine("", &col, &msg), 1);
  CHECK(msg.find("конец") != std::string::npos);
  for (const char* bad : {"{,}", "{\"a\"}", "{\"a\":}", "{a:1}", "[", "]", "nul", "tru", "{\"a\":1,}", "[,1]", "'x'"}) CHECK_MSG(errLine(bad) > 0, bad);
  std::string err;
  CHECK(!json::tryParse("[1,", &err));
  CHECK(!err.empty());
  CHECK(json::tryParse("[1]").has_value());
  // ParseError — пользовательская ошибка
  bool user = false;
  try {
    json::parse("{");
  } catch (const UserError&) {
    user = true;
  }
  CHECK(user);
}

TEST(base_json_depth_limit) {
  std::string deep(600, '[');
  deep += std::string(600, ']');
  int col = 0;
  std::string msg;
  CHECK_EQ(errLine(deep, &col, &msg), 1);
  CHECK(msg.find("вложенность") != std::string::npos);
  json::ParseOptions o;
  o.maxDepth = 1000;
  Value v = json::parse(deep, o);
  CHECK(v.isArr() && v.size() == 1);
  std::string ok(512, '[');
  ok += std::string(512, ']');
  CHECK(json::tryParse(ok).has_value());
}

TEST(base_json_objects_order_and_access) {
  Value v = json::parse(R"({"b": 1, "a": {"x": [1, 2, 3], "y": "строка"}, "c": true, "b": 5})");
  CHECK_EQ(v.size(), size_t(3));
  CHECK_EQ(v.members()[0].first, std::string("b"));  // повтор ключа: первая позиция, последнее значение
  CHECK_EQ(v.num("b"), 5.0);
  CHECK_EQ(v.members()[1].first, std::string("a"));
  CHECK_EQ(v.obj("a").str("y"), std::string("строка"));
  CHECK_EQ(v.obj("a").arr("x").size(), size_t(3));
  CHECK_EQ(v.obj("a").arr("x")[2].asNum(), 3.0);
  CHECK(v.boolean("c"));
  CHECK(!v.boolean("нет", false));
  CHECK_EQ(v.num("нет", 7), 7.0);
  CHECK_EQ(v.str("нет", "def"), std::string("def"));
  CHECK_EQ(v.integer("b"), i64(5));
  CHECK(v.obj("нет").isObj() && v.obj("нет").empty());
  CHECK(v.arr("нет").empty());
  CHECK(v.get("нет").isNull());
  CHECK(v.obj("c").isObj());  // не объект — пустой объект
  CHECK(v["нет"].isNull());   // добавляется null
  CHECK(v.has("нет") && v.size() == 4);
  CHECK(v.erase("нет") && !v.has("нет"));
  CHECK(!v.erase("нет"));
  // изменение
  Value w;
  w.set("name", "Арден");
  w.set("pop", 12000);
  w["tags"].push("порт");
  w["tags"].push(3);
  CHECK(w.isObj());
  CHECK_EQ(w.str("name"), std::string("Арден"));
  CHECK_EQ(w.arr("tags").size(), size_t(2));
  CHECK_EQ(w["tags"][1].asNum(), 3.0);
  // изменяемый индекс дополняет массив null
  Value grow;
  grow[3] = "x";
  CHECK(grow.isArr() && grow.size() == 4 && grow[0].isNull() && grow[3].asStr() == "x");
  const Value& cg = grow;
  CHECK(cg[100].isNull() && cg[-1].isNull());
  CHECK_THROWS(grow[-1]);
  CHECK_EQ(cg["нет"].isNull(), true);
  w.set("tags", Value::array({1, "два", nullptr, true}));
  CHECK_EQ(w.get("tags").size(), size_t(4));
  CHECK_EQ(w.set("pop", 13000).asNum(), 13000.0);  // set возвращает сохранённое значение
  CHECK(w.get("tags")[1].asStr() == "два");
  CHECK(w.get("tags")[2].isNull());
  CHECK(w.get("tags")[9].isNull());
  CHECK_THROWS(w.at(0));
  CHECK_THROWS(w.set("pop", 1).push(2));
  Value arr = Value::array();
  CHECK_THROWS(arr.set("x", 1));
  // копия независима, перемещение
  Value copy = w;
  copy.set("name", "Другое");
  CHECK_EQ(w.str("name"), std::string("Арден"));
  Value moved = std::move(copy);
  CHECK_EQ(moved.str("name"), std::string("Другое"));
  // присваивание части самого себя
  Value nest = json::parse(R"({"a": {"b": {"c": 1}}})");
  nest = nest.get("a");
  CHECK_EQ(nest.obj("b").num("c"), 1.0);
  Value nest2 = json::parse(R"([[[1, 2]]])");
  nest2 = std::move(nest2.items()[0]);
  CHECK_EQ(json::write(nest2, {0}), std::string("[[1,2]]"));
  // равенство не зависит от порядка ключей
  CHECK(json::parse(R"({"a":1,"b":[1,{"c":2}]})") == json::parse(R"({"b":[1,{"c":2}],"a":1})"));
  CHECK(json::parse(R"({"a":1})") != json::parse(R"({"a":2})"));
  CHECK(json::parse("[1,2]") != json::parse("[2,1]"));
  Value cl = json::parse("[1,2]");
  cl.clear();
  CHECK(cl.isArr() && cl.empty());
}

TEST(base_json_large_object_index) {
  Value v = Value::object();
  for (int i = 0; i < 6000; i++) v.set("k" + std::to_string(i), i);
  for (int i = 0; i < 6000; i += 7) CHECK_EQ(v.num("k" + std::to_string(i), -1), double(i));
  for (int i = 0; i < 6000; i += 3) CHECK(v.erase("k" + std::to_string(i)));
  for (int i = 0; i < 6000; i++) CHECK_EQ(v.has("k" + std::to_string(i)), i % 3 != 0);
  CHECK_EQ(v.members()[0].first, std::string("k1"));
  v.set("k0", "снова");
  CHECK_EQ(v.members().back().first, std::string("k0"));
  Value c = v;
  CHECK(c.find("k0") && c.str("k0") == "снова");
}

TEST(base_json_write_format) {
  Value v = json::parse(R"({"b": [1, 2, 3], "a": {"z": null, "y": [{"q": 1}, "s"]}, "e": [], "o": {}, "t": "a\"\\\n\u0001\u007f"})");
  CHECK_EQ(json::write(v, {0}), std::string(R"({"b":[1,2,3],"a":{"z":null,"y":[{"q":1},"s"]},"e":[],"o":{},"t":"a\"\\\n\u0001)") + "\x7f\"}");
  std::string pretty = json::write(v, {2, true, true});
  std::string expect =
      "{\n"
      "  \"a\": {\n"
      "    \"y\": [\n"
      "      {\n"
      "        \"q\": 1\n"
      "      },\n"
      "      \"s\"\n"
      "    ],\n"
      "    \"z\": null\n"
      "  },\n"
      "  \"b\": [1, 2, 3],\n"
      "  \"e\": [],\n"
      "  \"o\": {},\n"
      "  \"t\": \"a\\\"\\\\\\n\\u0001\x7f\"\n"
      "}";
  CHECK_EQ(pretty, expect);
  std::string noCompact = json::write(json::parse("[1,[2]]"), {1, false, false});
  CHECK_EQ(noCompact, std::string("[\n 1,\n [\n  2\n ]\n]"));
  // сортировка ключей побайтно: латиница раньше кириллицы
  CHECK_EQ(json::write(json::parse(R"({"я":1,"b":2,"А":3,"a":4})"), {0, true}), std::string(R"({"a":4,"b":2,"А":3,"я":1})"));
  // некорректный UTF-8 при записи заменяется
  CHECK_EQ(json::write(Value(std::string("x\xFFy")), {0}), std::string("\"x\xEF\xBF\xBDy\""));
  // круговой путь
  std::string again = json::write(json::parse(pretty), {2, true, true});
  CHECK_EQ(again, pretty);
}

TEST(base_json_number_format_roundtrip) {
  auto w = [](double d) { std::string s; json::appendNumber(s, d); return s; };
  CHECK_EQ(w(0), std::string("0"));
  CHECK_EQ(w(-0.0), std::string("0"));
  CHECK_EQ(w(123), std::string("123"));
  CHECK_EQ(w(-45), std::string("-45"));
  CHECK_EQ(w(0.1), std::string("0.1"));
  CHECK_EQ(w(1.5), std::string("1.5"));
  CHECK_EQ(w(1e21), std::string("1e+21"));
  CHECK_EQ(w(9007199254740992.0), std::string("9007199254740992"));
  CHECK_EQ(w(9007199254740991.0), std::string("9007199254740991"));
  CHECK_EQ(w(5e-324), std::string("5e-324"));
  CHECK_EQ(w(std::nan("")), std::string("null"));
  CHECK_EQ(w(kInf), std::string("null"));
  CHECK_EQ(w(1.0 / 3), std::string("0.3333333333333333"));
  Rng rng(42);
  for (int i = 0; i < 20000; i++) {
    u64 b = rng.next();
    double d;
    std::memcpy(&d, &b, 8);
    if (!std::isfinite(d)) continue;
    if (i % 3 == 0) d = double(i64(rng.next() >> 20)) * (rng.next() & 1 ? -1 : 1);
    if (i % 3 == 1) d = (rng.uniform() - 0.5) * 1e6;
    double back = json::parse(w(d)).asNum();
    if (d == 0) CHECK(back == 0);
    else CHECK_MSG(bitsOf(back) == bitsOf(d), w(d));
  }
}

TEST(base_json_large_document_speed) {
  // ~12 МБ: провинции с полилиниями (типичная форма данных мира).
  Value root = Value::object();
  Value& provs = root["provinces"];
  Rng rng(1);
  for (int i = 0; i < 13000; i++) {
    Value p = Value::object();
    p.set("id", "p" + std::to_string(i));
    p.set("name", "Провинция «" + std::to_string(i) + "»");
    p.set("owner", int(rng.next() % 300));
    p.set("pop", double(rng.next() % 1000000));
    p.set("tax", rng.uniform() * 30);
    Value pts = Value::array();
    for (int k = 0; k < 40; k++) pts.push(Value::array({std::round(rng.uniform() * 8000 * 100) / 100, std::round(rng.uniform() * 4500 * 100) / 100}));
    p.set("pts", std::move(pts));
    p.set("tags", Value::array({"порт", "столица", true, nullptr}));
    provs.push(std::move(p));
  }
  // лучшее из пяти измерений (машина может быть занята другими процессами)
  auto best = [](auto&& fn) {
    double b = 1e9;
    for (int i = 0; i < 5; i++) {
      double t0 = nowSeconds();
      fn();
      b = std::min(b, nowSeconds() - t0);
    }
    return b * 1000;
  };
  std::string compact, pretty;
  double wc = best([&] { compact = json::write(root, {0}); });
  double wp = best([&] { pretty = json::write(root, {2, true, true}); });
  Value a, b;
  double pc = best([&] { a = json::parse(compact); });
  double pp = best([&] { b = json::parse(pretty); });
  CHECK(a == root);
  CHECK(b == root);
  double parse10 = pc * 10.0 / (compact.size() / 1e6), parse10p = pp * 10.0 / (pretty.size() / 1e6);
  std::printf("       JSON %.1f MB компактно / %.1f MB с отступами: запись %.0f / %.0f ms, разбор %.0f / %.0f ms (%.0f / %.0f ms на 10 МБ)\n",
              compact.size() / 1e6, pretty.size() / 1e6, wc, wp, pc, pp, parse10, parse10p);
  CHECK(compact.size() > 10'000'000);
#ifdef NDEBUG
  CHECK(parse10 < rg::test::perf(150));
  CHECK(parse10p < rg::test::perf(150));
#endif
}
