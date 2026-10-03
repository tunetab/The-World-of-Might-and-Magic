// Regnum — внутренние объявления core/io (не для других модулей).
#pragma once
#include "base/json.h"
#include "core/io.h"
#include "core/schema.h"

namespace rg::io::detail {

// Файлы проекта (порядок записи: данные, затем world.json).
enum FileId : int {
  F_CATALOGS, F_GEO, F_PROVINCES, F_FACTIONS, F_CHARACTERS, F_RELATIONS, F_MODIFIERS,
  F_BUILDINGS, F_TECHS, F_ARMIES, F_ROUTES, F_DEALS, F_LOG, F_WORLD, F_COUNT
};
struct FileDef {
  const char* path;  // относительный путь
  const char* key;   // ключ массива записей (пусто — особая структура)
  u32 tables;
};
extern const FileDef kFiles[F_COUNT];
int fileIndex(std::string_view rel);  // -1 — не файл таблиц

// Прочитанное содержимое файлов до сборки мира.
struct Parts {
  bool present[F_COUNT] = {};
  Meta meta;
  Settings settings;
  Catalogs catalogs;
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::vector<Province> provinces;
  std::vector<Faction> factions;
  std::vector<Character> characters;
  RelMap relations;
  std::vector<Modifier> modifiers;
  std::vector<Building> buildings;
  std::vector<Tech> techs;
  std::vector<Army> armies;
  std::vector<Route> routes;
  std::vector<Deal> deals;
  std::vector<LogEntry> log;
};

// Значение JSON одного файла.
json::Value encode(const World& w, FileId f);
// Текст файла: отступ 2, ключи по алфавиту, LF, перевод строки в конце; geo и relations — запись на строку.
std::string format(const json::Value& v, FileId f);
// Разобрать значение файла в parts (файлы независимы — можно параллельно). world.json: версия — checkVersion.
void decode(const json::Value& v, FileId f, Parts& parts, Warnings& warns);
// Проверить format/version world.json; чужой формат или более новая версия — UserError.
void checkVersion(const json::Value& world, const std::string& origin, Warnings& warns);
// Собрать мир: ID без значения и повторные ID получают новые, затем нормализация.
// fixed — таблицы, упомянутые в предупреждениях warns (только этого чтения) или изменённые нормализацией.
World assemble(Parts&& parts, Warnings& warns, u32* fixed);

// Разобрать текст файла: ошибка разбора — UserError с именем файла, строкой и столбцом.
json::Value parseFile(std::string_view text, const std::string& origin);

std::string refStr(Seq s, Id id);                 // "p12"; 0 — пусто
std::optional<Id> parseRef(const json::Value& v, Seq s);

}  // namespace rg::io::detail
