// Regnum — JSON: значение с сохранением порядка ключей, строгий разбор, запись для диффов в Git.
//
//   json::Value v = json::parse(text);              // ParseError (UserError) со строкой и столбцом
//   double turn = v.obj("meta").num("turn", 1);
//   v.set("name", "Арден"); v["tags"].push(3);
//   std::string s = json::write(v, {2, true, true});  // отступ 2, сортировка ключей, скаляры массива в строку
//
// Числа — double. Целые до 2^53 записываются без дробной части, остальные — кратчайшим точным
// представлением (std::to_chars). Строки — UTF-8; некорректные байты заменяются на U+FFFD.
#pragma once
#include <initializer_list>
#include <new>
#include <type_traits>

#include "base/base.h"

namespace rg::json {

enum class Type : u8 { Null, Bool, Number, String, Array, Object };

class Value;
using Array = std::vector<Value>;
using Member = std::pair<std::string, Value>;

// Ошибка разбора: сообщение по-русски вида «JSON, строка 3, столбец 15: ожидалась запятая».
struct ParseError : UserError {
  int line, column;  // с 1; столбец — в символах
  ParseError(const std::string& msg, int line_, int column_) : UserError(msg), line(line_), column(column_) {}
};

class Value {
 public:
  Value() noexcept : t_(Type::Null), n_(0) {}
  Value(std::nullptr_t) noexcept : Value() {}
  Value(bool b) noexcept : t_(Type::Bool), b_(b) {}
  template <class T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, int> = 0>
  Value(T v) noexcept : t_(Type::Number), n_(double(v)) {}
  Value(std::string s) : t_(Type::String) { new (&s_) std::string(std::move(s)); }
  Value(std::string_view s) : Value(std::string(s)) {}
  Value(const char* s) : Value(std::string(s ? s : "")) {}
  Value(Array a);
  Value(const Value& o);
  Value(Value&& o) noexcept;
  Value& operator=(const Value& o);
  Value& operator=(Value&& o) noexcept;
  ~Value();

  static Value array(std::initializer_list<Value> items = {});
  static Value object();
  static Value object(std::initializer_list<Member> members);

  // ---------------------------------------------------------------- тип
  Type type() const { return t_; }
  bool isNull() const { return t_ == Type::Null; }
  bool isBool() const { return t_ == Type::Bool; }
  bool isNum() const { return t_ == Type::Number; }
  bool isStr() const { return t_ == Type::String; }
  bool isArr() const { return t_ == Type::Array; }
  bool isObj() const { return t_ == Type::Object; }

  // ---------------------------------------------------------------- значения (с запасным значением при другом типе)
  bool asBool(bool def = false) const { return t_ == Type::Bool ? b_ : def; }
  double asNum(double def = 0) const { return t_ == Type::Number ? n_ : def; }
  i64 asInt(i64 def = 0) const;  // округление до ближайшего целого; вне диапазона i64 — def
  const std::string& asStr() const&;  // пустая строка, если не строка
  std::string asStr() &&;              // у временного значения — по значению (без висячей ссылки)
  std::string asStr(std::string_view def) const { return t_ == Type::String ? s_ : std::string(def); }

  // ---------------------------------------------------------------- массив и объект
  size_t size() const;                        // элементов массива или членов объекта, иначе 0
  bool empty() const { return size() == 0; }
  // Индекс массива: const — вне диапазона null; изменяемый — массив дополняется null до i (null становится массивом).
  template <class I, std::enable_if_t<std::is_integral_v<I> && !std::is_same_v<I, bool>, int> = 0>
  const Value& operator[](I i) const { return i < 0 ? nullValue() : element(size_t(i)); }
  template <class I, std::enable_if_t<std::is_integral_v<I> && !std::is_same_v<I, bool>, int> = 0>
  Value& operator[](I i) {
    if (i < 0) throw std::out_of_range("json: отрицательный индекс");
    return elementGrow(size_t(i));
  }
  Value& at(size_t i);                         // вне диапазона — std::out_of_range
  const Array& items() const;                  // элементы массива (пустой, если не массив)
  Array& items();                              // null превращается в массив; иной тип — std::logic_error
  const std::vector<Member>& members() const;  // члены объекта по порядку (пусто, если не объект)

  // Объект: поиск по ключу. Отсутствующий ключ — null.
  bool has(std::string_view key) const { return find(key) != nullptr; }
  const Value* find(std::string_view key) const;
  Value* find(std::string_view key);
  const Value& get(std::string_view key) const;
  double num(std::string_view key, double def = 0) const { return get(key).asNum(def); }
  i64 integer(std::string_view key, i64 def = 0) const { return get(key).asInt(def); }
  std::string str(std::string_view key, std::string_view def = {}) const { return get(key).asStr(def); }
  bool boolean(std::string_view key, bool def = false) const { return get(key).asBool(def); }
  const Array& arr(std::string_view key) const { return get(key).items(); }
  const Value& obj(std::string_view key) const;  // член-объект или пустой объект

  // Изменение. null превращается в объект/массив; иной тип — std::logic_error.
  Value& set(std::string key, Value v);            // заменить значение (порядок сохраняется) или добавить в конец
  Value& operator[](std::string_view key);          // найти или добавить null
  const Value& operator[](std::string_view key) const { return get(key); }
  bool erase(std::string_view key);
  Value& push(Value v);
  void clear();                                      // очистить массив/объект, сохранив тип

  bool operator==(const Value& o) const;
  bool operator!=(const Value& o) const { return !(*this == o); }

 private:
  friend struct Parser;  // разбор собирает объекты без перераспределений
  struct Object;
  static const Value& nullValue();
  const Value& element(size_t i) const;
  Value& elementGrow(size_t i);
  void destroy() noexcept;
  void copyFrom(const Value& o);
  Object& objRef();

  Type t_;
  union {
    bool b_;
    double n_;
    std::string s_;
    Array a_;
    Object* o_;
  };
};

// ---------------------------------------------------------------- разбор и запись
struct ParseOptions {
  int maxDepth = 512;  // глубина вложенности
};
Value parse(std::string_view text, const ParseOptions& opt = {});
std::optional<Value> tryParse(std::string_view text, std::string* error = nullptr, const ParseOptions& opt = {});

struct WriteOptions {
  int indent = 2;                     // 0 — компактно в одну строку
  bool sortKeys = false;              // ключи объектов по байтам UTF-8
  bool compactNumbersArrays = true;   // массив из скаляров — в одну строку (при indent > 0)
};
std::string write(const Value& v, const WriteOptions& opt = {});
void write(std::string& out, const Value& v, const WriteOptions& opt = {});

// Число в кратчайшем точном виде (целые — без «.0»; NaN и бесконечность — «null»).
void appendNumber(std::string& out, double v);
// Строка в кавычках с экранированием.
void appendString(std::string& out, std::string_view s);

}  // namespace rg::json
