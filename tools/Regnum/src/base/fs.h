// Regnum — файловая система. Все пути — UTF-8 строки (кириллица и пробелы допустимы на всех ОС).
// Возвращаемые пути используют прямой слеш «/» как разделитель (Windows его тоже принимает).
// Функции не бросают исключений: ошибка — false/nullopt и, где есть параметр error, сообщение по-русски.
#pragma once
#include <filesystem>
#include <span>

#include "base/base.h"

namespace rg::fs {

// Преобразования UTF-8 <-> std::filesystem::path (единственный допустимый путь создания path).
std::filesystem::path path(const std::string& utf8);
std::string fromPath(const std::filesystem::path& p);  // UTF-8, разделитель «/»

// ---------------------------------------------------------------- чтение и запись
std::optional<std::string> readFile(const std::string& path, std::string* error = nullptr);
// Атомарная запись: временный файл в той же папке, сброс на диск, замена целевого файла.
// Недостающие папки создаются. При ошибке исходный файл не меняется.
bool writeFileAtomic(const std::string& path, std::string_view data, std::string* error = nullptr);
bool writeFileAtomic(const std::string& path, std::span<const u8> data, std::string* error = nullptr);

// ---------------------------------------------------------------- сведения
bool exists(const std::string& path);
bool isDir(const std::string& path);
bool isFile(const std::string& path);
std::optional<u64> fileSize(const std::string& path);
std::optional<i64> mtime(const std::string& path);  // миллисекунды с 1970-01-01 UTC

struct DirEntry {
  std::string name;  // имя без папки
  std::string path;  // полный путь
  bool dir = false;
  bool link = false; // символическая ссылка или точка соединения (listTree в них не заходит)
  u64 size = 0;      // для файлов
  i64 mtime = 0;     // миллисекунды с 1970-01-01 UTC
};
// Содержимое папки, по имени (побайтно). Нет папки — пустой список.
std::vector<DirEntry> list(const std::string& dir);
// Все файлы под папкой: относительные пути с «/», по порядку байтов.
std::vector<std::string> listTree(const std::string& dir);

// ---------------------------------------------------------------- изменения
bool makeDirs(const std::string& path, std::string* error = nullptr);
bool remove(const std::string& path, std::string* error = nullptr);     // файл или пустая папка; отсутствие — успех
bool removeAll(const std::string& path, std::string* error = nullptr);  // рекурсивно; отсутствие — успех
bool rename(const std::string& from, const std::string& to, std::string* error = nullptr);  // заменяет существующий файл
bool copyFile(const std::string& from, const std::string& to, std::string* error = nullptr);  // перезаписывает

// ---------------------------------------------------------------- системные папки
std::string exeDir();       // папка исполняемого файла
std::string homeDir();
std::string documentsDir(); // «Документы» пользователя (на Linux — XDG_DOCUMENTS_DIR), иначе домашняя
std::string userDataDir();  // Windows %APPDATA%/Regnum, Linux $XDG_CONFIG_HOME/regnum или ~/.config/regnum,
                            // macOS ~/Library/Application Support/Regnum; создаётся при необходимости
std::string tempDir();

// ---------------------------------------------------------------- работа с путями (строки)
std::string join(const std::string& a, const std::string& b);  // b абсолютный — b
std::string parent(const std::string& p);    // "a/b/c.txt" -> "a/b"
std::string filename(const std::string& p);  // "a/b/c.txt" -> "c.txt"
std::string stem(const std::string& p);      // "a/b/c.txt" -> "c"
std::string ext(const std::string& p);       // "a/b/c.txt" -> ".txt" (как есть, без смены регистра)
std::string absolute(const std::string& p);  // абсолютный нормализованный путь
bool isAbsolute(const std::string& p);

}  // namespace rg::fs
