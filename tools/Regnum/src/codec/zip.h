// Regnum — архивы ZIP: чтение (stored, deflate, дескрипторы данных, UTF-8 и CP437 имена) и запись.
// ZIP64, шифрование и прочие методы сжатия не поддерживаются — понятная ошибка.
//
//   codec::ZipReader zr;
//   if (!zr.openFile(path, &err)) ...;
//   for (auto& e : zr.entries()) if (!e.dir) auto text = zr.read(e.name, &err);
//
//   codec::ZipWriter zw;
//   zw.add("world/meta.json", json);
//   fs::writeFileAtomic(path, zw.finish());
#pragma once
#include <span>
#include <unordered_map>

#include "base/base.h"

namespace rg::codec {

struct ZipEntry {
  std::string name;     // UTF-8, разделитель «/»; у папок оканчивается на «/»
  u64 size = 0;         // распакованный размер
  u64 packedSize = 0;   // сжатый размер
  u32 crc = 0;
  u16 method = 0;       // 0 — без сжатия, 8 — deflate
  bool dir = false;
  bool encrypted = false;
  i64 mtime = 0;        // секунды с 1970-01-01 (время DOS считается UTC; расширенная метка — если есть)
  u64 localOffset = 0;  // смещение локального заголовка
};

class ZipReader {
 public:
  bool open(std::string archive, std::string* error = nullptr);  // архив в памяти (данные перемещаются внутрь)
  bool openFile(const std::string& path, std::string* error = nullptr);
  const std::vector<ZipEntry>& entries() const { return entries_; }
  const std::string& comment() const { return comment_; }
  int find(std::string_view name) const;  // индекс записи или −1
  std::optional<std::string> read(size_t index, std::string* error = nullptr) const;
  std::optional<std::string> read(std::string_view name, std::string* error = nullptr) const;

  u64 maxEntrySize = u64(1) << 31;  // предел распакованного размера записи

 private:
  std::string data_;
  std::vector<ZipEntry> entries_;
  std::string comment_;
  std::unordered_map<std::string, int> index_;
  u64 base_ = 0;  // поправка смещений (данные перед архивом)
  u64 cdStart_ = 0;
};

// Безопасное относительное имя для распаковки на диск: без «..», абсолютных путей, дисков и «\».
bool safeZipPath(std::string_view name);

class ZipWriter {
 public:
  explicit ZipWriter(int level = 6) : level_(level) {}
  // Уровень сжатия следующих записей (0 — без сжатия: уже сжатые данные, например .gz).
  void setLevel(int level) { level_ = level; }
  // Добавить файл. mtime — секунды с 1970-01-01 (0 — 1980-01-01). false — недопустимое или повторное имя, предел ZIP.
  bool add(std::string_view name, std::string_view data, i64 mtime = 0, std::string* error = nullptr);
  bool add(std::string_view name, std::span<const u8> data, i64 mtime = 0, std::string* error = nullptr);
  bool addDir(std::string_view name, i64 mtime = 0, std::string* error = nullptr);
  size_t count() const { return central_.size(); }
  // Завершить архив и вернуть его байты; писатель очищается.
  std::string finish(std::string_view comment = {});

 private:
  struct Central {
    std::string name;
    u32 crc;
    u32 packed, size;
    u16 method, time, date;
    u32 offset;
    bool dir;
  };
  bool addImpl(std::string_view name, const u8* data, size_t n, bool dir, i64 mtime, std::string* error);
  int level_;
  std::string out_;
  std::vector<Central> central_;
  std::unordered_map<std::string, int> names_;
};

}  // namespace rg::codec
