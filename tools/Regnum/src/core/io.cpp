// Regnum — папка проекта, архив .regnum, снимки ходов, резервные копии, внешние изменения,
// недавние проекты и автосохранение.
#include <charconv>
#include <chrono>
#include <ctime>

#include "base/fs.h"
#include "base/jobs.h"
#include "codec/zip.h"
#include "codec/zlib.h"
#include "core/io_internal.h"

namespace rg::io {

using namespace detail;
using json::Value;

// ================================================================ общее
std::string Warning::text() const {
  std::string s = file;
  if (!where.empty()) {
    if (!s.empty()) s += ": ";
    s += where;
  }
  if (!s.empty()) s += ": ";
  return s + msg;
}

const std::vector<ProjectFile>& projectFiles() {
  static const std::vector<ProjectFile> list = [] {
    std::vector<ProjectFile> v;
    for (const FileDef& f : kFiles) v.push_back({f.path, f.tables});
    return v;
  }();
  return list;
}

u32 tablesOfFile(std::string_view rel) {
  int i = fileIndex(rel);
  return i < 0 ? 0 : kFiles[i].tables;
}

bool isBundlePath(const std::string& path) { return utf8::lower(fs::ext(path)) == kBundleExt; }

namespace {

constexpr const char* kWorldFile = "world.json";
constexpr const char* kHistoryDir = "history";
constexpr const char* kHistoryIndex = "history/index.json";
constexpr size_t kMaxUnpacked = size_t(1) << 30;  // предел распаковки снимка (1 ГБ)

std::string bytesStr(std::span<const u8> b) { return std::string(reinterpret_cast<const char*>(b.data()), b.size()); }

i64 wallMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Окно «гонки» времени файла: запись другой программой в ту же единицу времени файловой системы
// (FAT — 2 с) не меняет mtime, поэтому свежие файлы сверяются по содержимому.
constexpr i64 kRacyMs = 2000;

FileStamp stampOf(const std::string& data, std::optional<i64> mtime) {
  FileStamp s;
  s.exists = true;
  s.size = data.size();
  s.crc = codec::crc32(data);
  s.mtime = mtime.value_or(0);
  s.checked = wallMs();
  return s;
}

// ---------------------------------------------------------------- источник файлов: папка или архив
class Source {
 public:
  virtual ~Source() = default;
  virtual bool bundle() const = 0;
  virtual const std::string& path() const = 0;
  virtual bool has(const std::string& rel) const = 0;
  virtual std::optional<std::string> read(const std::string& rel, std::string* err) const = 0;
  virtual u64 size(const std::string& rel) const = 0;
  // Файлы папки dir (без вложенных): относительные пути от корня проекта.
  virtual std::vector<std::string> list(const std::string& dir) const = 0;
  std::string origin(const std::string& rel) const { return bundle() ? fs::filename(path()) + ": " + rel : rel; }
};

class FolderSource final : public Source {
 public:
  explicit FolderSource(std::string root) : root_(std::move(root)) {}
  bool bundle() const override { return false; }
  const std::string& path() const override { return root_; }
  bool has(const std::string& rel) const override { return fs::isFile(fs::join(root_, rel)); }
  std::optional<std::string> read(const std::string& rel, std::string* err) const override { return fs::readFile(fs::join(root_, rel), err); }
  u64 size(const std::string& rel) const override { return fs::fileSize(fs::join(root_, rel)).value_or(0); }
  std::vector<std::string> list(const std::string& dir) const override {
    std::vector<std::string> out;
    for (auto& e : fs::list(fs::join(root_, dir)))
      if (!e.dir) out.push_back(dir + "/" + e.name);
    return out;
  }

 private:
  std::string root_;
};

class ZipSource final : public Source {
 public:
  explicit ZipSource(std::string path) : path_(std::move(path)) {
    std::string err;
    if (!zr_.openFile(path_, &err)) fail("Не удалось открыть архив «" + fs::filename(path_) + "»: " + err);
    // Мир в корне архива или в единственной вложенной папке (архив, созданный упаковкой папки).
    if (zr_.find(kWorldFile) < 0) {
      std::string best;
      int found = 0;
      for (auto& e : zr_.entries()) {
        const std::string& n = e.name;
        if (n.size() > 11 && n.compare(n.size() - 11, 11, "/world.json") == 0 &&
            std::count(n.begin(), n.end(), '/') == 1) {
          best = n.substr(0, n.size() - 10);
          found++;
        }
      }
      if (found != 1) fail("Архив «" + fs::filename(path_) + "» не содержит мир Regnum (нет world.json)");
      prefix_ = best;
    }
  }
  bool bundle() const override { return true; }
  const std::string& path() const override { return path_; }
  bool has(const std::string& rel) const override { return zr_.find(prefix_ + rel) >= 0; }
  std::optional<std::string> read(const std::string& rel, std::string* err) const override { return zr_.read(prefix_ + rel, err); }
  u64 size(const std::string& rel) const override {
    int i = zr_.find(prefix_ + rel);
    return i < 0 ? 0 : zr_.entries()[size_t(i)].size;
  }
  std::vector<std::string> list(const std::string& dir) const override {
    std::vector<std::string> out;
    std::string p = prefix_ + dir + "/";
    for (auto& e : zr_.entries()) {
      if (e.dir || e.name.size() <= p.size() || e.name.compare(0, p.size(), p) != 0) continue;
      if (e.name.find('/', p.size()) != std::string::npos) continue;
      out.push_back(e.name.substr(prefix_.size()));
    }
    std::sort(out.begin(), out.end());
    return out;
  }

 private:
  std::string path_, prefix_;
  codec::ZipReader zr_;
};

std::unique_ptr<Source> openSource(std::string path) {
  if (path.empty()) fail("Не указан путь к миру");
  if (fs::isFile(path) && fs::filename(path) == kWorldFile) path = fs::parent(path);
  if (fs::isDir(path)) {
    if (!fs::isFile(fs::join(path, kWorldFile))) fail("Папка «" + path + "» не является миром Regnum: нет файла world.json");
    return std::make_unique<FolderSource>(path);
  }
  if (fs::isFile(path)) return std::make_unique<ZipSource>(path);
  fail("Мир не найден: «" + path + "»");
}

// ---------------------------------------------------------------- чтение
LoadResult loadFrom(const Source& src) {
  LoadResult r;
  r.bundle = src.bundle();
  if (!r.bundle) r.files.folder = fs::absolute(src.path());

  std::string err;
  // world.json — первым: проверка версии до разбора остального.
  std::optional<i64> worldTime = r.bundle ? std::nullopt : fs::mtime(fs::join(src.path(), kWorldFile));
  auto worldText = src.read(kWorldFile, &err);
  if (!worldText) fail("Не удалось прочитать " + src.origin(kWorldFile) + ": " + err);
  Value world = parseFile(*worldText, src.origin(kWorldFile));
  Warnings warnFiles[F_COUNT];
  checkVersion(world, src.origin(kWorldFile), warnFiles[F_WORLD]);
  if (!r.bundle) r.files.files[kWorldFile] = stampOf(*worldText, worldTime);

  Parts parts;
  std::string errors[F_COUNT];
  std::string texts[F_COUNT];
  bool present[F_COUNT] = {};
  FileStamp stamps[F_COUNT];
  // Архив читается последовательно, папка — параллельно; разбор — параллельно.
  auto readOne = [&](size_t i) {
    const std::string rel = kFiles[i].path;
    std::optional<i64> t = r.bundle ? std::nullopt : fs::mtime(fs::join(src.path(), rel));
    if (!src.has(rel)) return;
    std::string e;
    auto text = src.read(rel, &e);
    if (!text) {
      errors[i] = "Не удалось прочитать " + src.origin(rel) + ": " + e;
      return;
    }
    if (!r.bundle) stamps[i] = stampOf(*text, t);
    texts[i] = std::move(*text);
    present[i] = true;
  };
  if (r.bundle) {
    for (size_t i = 0; i < F_WORLD; i++) readOne(i);
  }
  jobs::parallelFor(size_t(F_WORLD), [&](size_t i) {
    if (!r.bundle) readOne(i);
    if (!present[i] || !errors[i].empty()) return;
    try {
      Value v = parseFile(texts[i], src.origin(kFiles[i].path));
      std::string().swap(texts[i]);
      decode(v, FileId(i), parts, warnFiles[i]);
    } catch (const std::exception& e) {
      errors[i] = e.what();
    }
  }, 1);
  for (size_t i = 0; i < F_WORLD; i++)
    if (!errors[i].empty()) fail(errors[i]);
  decode(world, F_WORLD, parts, warnFiles[F_WORLD]);
  for (size_t i = 0; i < F_WORLD; i++) {
    if (!present[i]) warnFiles[i].push_back({kFiles[i].path, "", "файл не найден — таблица пуста"});
    if (!r.bundle) r.files.files[kFiles[i].path] = stamps[i];
  }
  r.warnings = std::move(warnFiles[F_WORLD]);
  for (size_t i = 0; i < F_WORLD; i++) r.warnings.insert(r.warnings.end(), warnFiles[i].begin(), warnFiles[i].end());
  // Лишние файлы JSON в data/ (например, таблица под неверным именем) не читаются — об этом стоит знать.
  for (const std::string& rel : src.list("data"))
    if (fileIndex(rel) < 0 && utf8::lower(fs::ext(rel)) == ".json")
      r.warnings.push_back({rel, "", "неизвестный файл данных — не читается (имена файлов мира — docs/FORMAT.md)"});
  r.world = assemble(std::move(parts), r.warnings, &r.fixedTables);
  return r;
}

// ---------------------------------------------------------------- текст файлов
std::vector<std::string> fileTexts(const World& w, const std::vector<int>& files) {
  std::vector<std::string> out(files.size());
  jobs::parallelFor(files.size(), [&](size_t k) {
    FileId f = FileId(files[k]);
    out[k] = format(encode(w, f), f);
  }, 1);
  return out;
}

std::vector<int> allFiles() {
  std::vector<int> v;
  for (int i = 0; i < F_COUNT; i++) v.push_back(i);
  return v;
}

// ---------------------------------------------------------------- резервные копии
// .regnum-backup/<папка файла>/<имя>.<NNNNNN><расширение>; номер растёт, хранятся последние keep.
// auto — версия, записанная автосохранением: своя очередь в .regnum-backup/auto/ (ручные копии не вытесняются).
std::string backupFile(const std::string& folder, const std::string& relIn, const std::string& content, int keep, bool autoQueue = false) {
  std::string root = fs::join(folder, kBackupDir);
  std::string rel = autoQueue ? "auto/" + relIn : relIn;
  std::string sub = fs::parent(rel);
  std::string dir = sub.empty() ? root : fs::join(root, sub);
  std::string err;
  std::string ignore = fs::join(root, ".gitignore");
  if (!fs::isFile(ignore) && !fs::writeFileAtomic(ignore, std::string_view("*\n"), &err))
    fail("Не удалось создать папку резервных копий «" + root + "»: " + err);
  std::string stem = fs::stem(rel), ext = fs::ext(rel);
  std::vector<std::pair<u64, std::string>> have;
  for (auto& e : fs::list(dir)) {
    if (e.dir) continue;
    const std::string& n = e.name;
    if (n.size() <= stem.size() + 1 + ext.size() || n.compare(0, stem.size() + 1, stem + ".") != 0 ||
        n.compare(n.size() - ext.size(), ext.size(), ext) != 0)
      continue;
    std::string_view num(n.data() + stem.size() + 1, n.size() - stem.size() - 1 - ext.size());
    u64 k = 0;
    auto res = std::from_chars(num.data(), num.data() + num.size(), k);
    if (num.empty() || res.ec != std::errc() || res.ptr != num.data() + num.size()) continue;
    have.push_back({k, e.path});
  }
  std::sort(have.begin(), have.end());
  u64 next = have.empty() ? 1 : have.back().first + 1;
  char buf[32];
  std::snprintf(buf, sizeof buf, ".%06llu", static_cast<unsigned long long>(next));
  std::string name = stem + buf + ext;
  std::string full = fs::join(dir, name);
  if (!fs::writeFileAtomic(full, content, &err)) fail("Не удалось создать резервную копию «" + full + "»: " + err);
  have.push_back({next, full});
  size_t excess = have.size() > size_t(keep) ? have.size() - size_t(keep) : 0;
  for (size_t i = 0; i < excess; i++) fs::remove(have[i].second);
  return std::string(kBackupDir) + "/" + (sub.empty() ? "" : sub + "/") + name;
}

// Файл изменён извне относительно отпечатка? (обновляет время, если содержимое прежнее)
bool changedOnDisk(const std::string& full, FileStamp& st) {
  bool ex = fs::isFile(full);
  if (!ex) return st.exists;
  if (!st.exists) return true;
  auto sz = fs::fileSize(full);
  auto mt = fs::mtime(full);
  const bool racy = st.checked - st.mtime < kRacyMs;
  if (sz && mt && *sz == st.size && *mt == st.mtime && !racy) return false;
  auto data = fs::readFile(full);
  if (!data) return true;
  if (data->size() == st.size && codec::crc32(*data) == st.crc) {
    if (mt) st.mtime = *mt;
    st.checked = wallMs();
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- история ходов
std::string turnFileName(int turn) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "turn-%04d.json.gz", turn);
  return buf;
}

// turn-NNNN.json.gz (снимок без номера) или turn-NNNN-SSSSSS.json.gz (ход и номер снимка).
std::optional<std::pair<int, u64>> parseSnapName(std::string_view name) {
  constexpr std::string_view pre = "turn-", post = ".json.gz";
  if (name.size() <= pre.size() + post.size() || name.substr(0, pre.size()) != pre || name.substr(name.size() - post.size()) != post)
    return std::nullopt;
  std::string_view num = name.substr(pre.size(), name.size() - pre.size() - post.size());
  std::string_view seqPart;
  if (size_t dash = num.find('-'); dash != std::string_view::npos) {
    seqPart = num.substr(dash + 1);
    num = num.substr(0, dash);
    if (seqPart.empty()) return std::nullopt;
  }
  int t = 0;
  auto r = std::from_chars(num.data(), num.data() + num.size(), t);
  if (r.ec != std::errc() || r.ptr != num.data() + num.size() || t < 1) return std::nullopt;
  u64 seq = 0;
  if (!seqPart.empty()) {
    auto rs = std::from_chars(seqPart.data(), seqPart.data() + seqPart.size(), seq);
    if (rs.ec != std::errc() || rs.ptr != seqPart.data() + seqPart.size() || seq == 0) return std::nullopt;
  }
  return std::make_pair(t, seq);
}

std::string snapFileName(int turn, u64 seq) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "turn-%04d-%06llu.json.gz", turn, static_cast<unsigned long long>(seq));
  return buf;
}

void sortSnaps(std::vector<SnapshotInfo>& v) {
  std::sort(v.begin(), v.end(), [](const SnapshotInfo& a, const SnapshotInfo& b) {
    if (a.turn != b.turn) return a.turn < b.turn;
    if (a.seq != b.seq) return a.seq < b.seq;
    return a.file < b.file;
  });
}

std::vector<SnapshotInfo> listFrom(const Source& src, u64* head = nullptr) {
  std::map<std::string, SnapshotInfo> m;   // файл -> сведения
  std::vector<std::string> files = src.list(kHistoryDir);
  auto exists = [&](const std::string& rel) { return std::find(files.begin(), files.end(), rel) != files.end(); };
  if (head) *head = 0;
  if (auto text = src.read(kHistoryIndex, nullptr)) {
    if (auto v = json::tryParse(*text)) {
      if (head) *head = u64(std::max<i64>(0, v->integer("head")));
      for (const Value& e : v->arr("snapshots")) {
        auto p = parseSnapName(e.str("file"));
        if (!p) continue;
        SnapshotInfo s;
        s.turn = p->first;
        s.seq = p->second;
        s.file = std::string(kHistoryDir) + "/" + e.str("file");
        if (!exists(s.file)) continue;
        s.at = e.str("at");
        s.label = e.str("label");
        s.name = e.str("name");
        s.kind = e.str("kind");
        s.parent = u64(std::max<i64>(0, e.integer("parent")));
        m[s.file] = std::move(s);
      }
    }
  }
  for (const std::string& rel : files) {
    auto p = parseSnapName(fs::filename(rel));
    if (!p || m.count(rel)) continue;
    SnapshotInfo s;
    s.turn = p->first;
    s.seq = p->second;
    s.file = rel;
    m[rel] = std::move(s);
  }
  std::vector<SnapshotInfo> out;
  for (auto& [f, s] : m) {
    s.size = src.size(s.file);
    out.push_back(std::move(s));
  }
  sortSnaps(out);
  return out;
}

std::string indexText(const std::vector<SnapshotInfo>& list, u64 head = 0) {
  json::Array a;
  for (auto& s : list) {
    Value e = Value::object();
    e.set("turn", s.turn);
    e.set("file", fs::filename(s.file));
    e.set("at", s.at);
    e.set("label", s.label);
    e.set("name", s.name);
    e.set("size", s.size);
    if (s.seq) e.set("seq", s.seq);
    if (!s.kind.empty()) e.set("kind", s.kind);
    if (s.parent) e.set("parent", s.parent);
    a.push_back(std::move(e));
  }
  Value o = Value::object();
  o.set("snapshots", Value(std::move(a)));
  if (head) o.set("head", head);
  std::string out = json::write(o, json::WriteOptions{2, true, true});
  out.push_back('\n');
  return out;
}

// Файлы истории источника: относительный путь -> байты (index.json пересобирается).
std::vector<std::pair<std::string, std::string>> readHistory(const Source& src, std::vector<SnapshotInfo>* infos, u64* head = nullptr) {
  std::vector<std::pair<std::string, std::string>> out;
  auto list = listFrom(src, head);
  for (auto& s : list) {
    std::string err;
    auto data = src.read(s.file, &err);
    if (!data) fail("Не удалось прочитать " + src.origin(s.file) + ": " + err);
    out.push_back({s.file, std::move(*data)});
  }
  if (infos) *infos = std::move(list);
  return out;
}

using FileList = std::vector<std::pair<std::string, std::string>>;  // относительный путь -> байты

// Файлы мира (world.json — первым), записанные заново.
FileList worldFiles(const World& w) {
  auto files = allFiles();
  std::rotate(files.begin(), files.begin() + F_WORLD, files.end());
  auto texts = fileTexts(w, files);
  FileList out;
  for (size_t k = 0; k < files.size(); k++) out.push_back({kFiles[files[k]].path, std::move(texts[k])});
  return out;
}

// Файлы мира источника как есть (для пересборки архива без изменения мира).
FileList rawWorldFiles(const Source& src) {
  FileList out;
  for (int k = 0; k < F_COUNT; k++) {
    int i = (k + F_WORLD) % F_COUNT;
    std::string rel = kFiles[i].path, err;
    if (!src.has(rel)) continue;
    auto data = src.read(rel, &err);
    if (!data) fail("Не удалось прочитать " + src.origin(rel) + ": " + err);
    out.push_back({rel, std::move(*data)});
  }
  return out;
}

// Собрать архив: файлы мира + история.
std::string buildBundle(const FileList& world, const FileList& history, const std::vector<SnapshotInfo>& infos, u64 head = 0) {
  i64 now = i64(std::time(nullptr));
  codec::ZipWriter zw(6);
  std::string err;
  for (auto& [rel, data] : world)
    if (!zw.add(rel, data, now, &err)) fail("Не удалось собрать архив: " + err);
  if (!history.empty()) {
    if (!zw.add(kHistoryIndex, indexText(infos, head), now, &err)) fail("Не удалось собрать архив: " + err);
    zw.setLevel(0);   // снимки уже сжаты gzip: повторное сжатие только тратит время записи архива
    for (auto& [rel, data] : history)
      if (!zw.add(rel, data, now, &err)) fail("Не удалось собрать архив: " + err);
  }
  return zw.finish("Regnum world");
}

void writeBundleFile(const std::string& path, const std::string& bytes) {
  std::string err;
  if (!fs::writeFileAtomic(path, bytes, &err)) fail("Не удалось записать архив «" + path + "»: " + err);
}

// ---------------------------------------------------------------- данные пользователя
std::string dataDirOr(const std::string& d) {
  std::string dir = d.empty() ? fs::userDataDir() : d;
  fs::makeDirs(dir);
  return dir;
}

std::string hex16(u64 v) {
  char buf[20];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

std::string autosaveSlot(const std::string& project) {
  return project.empty() ? std::string("new") : hex16(hash64(fs::absolute(project)));
}

}  // namespace

bool isProject(const std::string& path) {
  if (fs::isDir(path)) return fs::isFile(fs::join(path, kWorldFile));
  if (!fs::isFile(path)) return false;
  if (fs::filename(path) == kWorldFile) return true;
  return isBundlePath(path);  // содержимое архива проверяется при чтении
}

// ================================================================ папка и архив
LoadResult load(const std::string& path) {
  auto src = openSource(path);
  return loadFrom(*src);
}

LoadResult loadBundle(const std::string& path) {
  if (!fs::isFile(path)) fail("Архив не найден: «" + path + "»");
  ZipSource src(path);
  return loadFrom(src);
}

SaveResult save(const std::string& folder, const World& w, u32 dirtyTables, FileState* state, const SaveOptions& opt) {
  if (folder.empty()) fail("Не указана папка для сохранения мира");
  std::string err;
  if (fs::exists(folder) && !fs::isDir(folder)) fail("«" + folder + "» — файл, а не папка");
  if (!fs::makeDirs(folder, &err)) fail("Не удалось создать папку «" + folder + "»: " + err);
  if (state && (state->folder.empty() || fs::absolute(state->folder) != fs::absolute(folder))) {
    state->folder = fs::absolute(folder);
    state->files.clear();
  }

  std::vector<int> todo;
  for (int i = 0; i < F_COUNT; i++)
    if ((kFiles[i].tables & dirtyTables) || !fs::isFile(fs::join(folder, kFiles[i].path))) todo.push_back(i);

  if (state && !opt.overwriteExternal) {
    std::vector<std::string> conflicts;
    for (int i : todo) {
      auto it = state->files.find(kFiles[i].path);
      if (it == state->files.end()) continue;
      std::string full = fs::join(folder, kFiles[i].path);
      if (!fs::isFile(full)) continue;  // удалённый файл просто создаётся заново
      if (changedOnDisk(full, it->second)) conflicts.push_back(kFiles[i].path);
    }
    if (!conflicts.empty())
      fail("Файлы мира изменены другой программой: " + join(conflicts, ", ") +
           ". Перечитайте мир или сохраните с заменой чужих изменений.");
  }

  SaveResult res;
  auto texts = fileTexts(w, todo);
  for (size_t k = 0; k < todo.size(); k++) {
    const std::string rel = kFiles[todo[k]].path;
    const std::string full = fs::join(folder, rel);
    const std::string& text = texts[k];
    std::optional<std::string> old = fs::isFile(full) ? fs::readFile(full) : std::nullopt;
    if (old && *old == text) {
      res.unchanged.push_back(rel);
      if (state) state->files[rel] = stampOf(text, fs::mtime(full));
      continue;
    }
    if (old && opt.backups && opt.keepBackups > 0) {
      bool autoQueue = std::find(opt.autoBackups.begin(), opt.autoBackups.end(), rel) != opt.autoBackups.end();
      res.backups.push_back(backupFile(folder, rel, *old, opt.keepBackups, autoQueue));
    }
    if (!fs::writeFileAtomic(full, text, &err)) fail("Не удалось записать «" + full + "»: " + err);
    res.written.push_back(rel);
    if (state) state->files[rel] = stampOf(text, fs::mtime(full));
  }
  if (state) {
    // Отпечатки файлов, не входивших в запись, остаются прежними; недостающие — снимаются с диска.
    for (const FileDef& f : kFiles) {
      if (state->files.count(f.path)) continue;
      std::string full = fs::join(folder, f.path);
      if (auto data = fs::readFile(full)) state->files[f.path] = stampOf(*data, fs::mtime(full));
      else state->files[f.path] = FileStamp{};
    }
  }
  return res;
}

std::vector<ExternalChange> externalChanges(FileState& state) {
  std::vector<ExternalChange> out;
  if (state.folder.empty()) return out;
  for (auto& [rel, st] : state.files) {
    std::string full = fs::join(state.folder, rel);
    bool ex = fs::isFile(full);
    ExternalChange c;
    c.file = rel;
    c.tables = tablesOfFile(rel);
    if (!ex) {
      if (!st.exists) continue;
      c.kind = ChangeKind::Removed;
    } else if (!st.exists) {
      c.kind = ChangeKind::Added;
    } else if (!changedOnDisk(full, st)) {
      continue;
    } else {
      c.kind = ChangeKind::Modified;
    }
    out.push_back(std::move(c));
  }
  return out;
}

FileStamp stampFile(const std::string& path) {
  auto mt = fs::mtime(path);
  auto data = fs::isFile(path) ? fs::readFile(path) : std::nullopt;
  if (!data) return FileStamp{};
  return stampOf(*data, mt);
}

bool fileChanged(const std::string& path, FileStamp& st) { return changedOnDisk(path, st); }

std::string savedTime(const std::string& path) {
  try {
    auto src = openSource(path);
    auto text = src->read(kWorldFile, nullptr);
    if (!text) return {};
    auto v = json::tryParse(*text);
    return v ? v->obj("meta").str("updatedAt") : std::string();
  } catch (const std::exception&) {
    return {};
  }
}

void saveBundle(const std::string& path, const World& w, const std::string& historyFrom) {
  if (path.empty()) fail("Не указан путь архива");
  if (fs::isDir(path)) fail("«" + path + "» — папка, а не файл архива");
  std::vector<std::pair<std::string, std::string>> history;
  std::vector<SnapshotInfo> infos;
  u64 head = 0;
  std::string from = historyFrom;
  if (from.empty() && fs::isFile(path)) {
    // История прежней версии архива (повреждённый архив истории не даёт — перезаписывается).
    try {
      ZipSource old(path);
      history = readHistory(old, &infos, &head);
    } catch (const UserError& e) {
      logWarn("история архива не перенесена: %s", e.what());
    }
  } else if (!from.empty()) {
    auto src = openSource(from);
    history = readHistory(*src, &infos, &head);
  }
  writeBundleFile(path, buildBundle(worldFiles(w), history, infos, head));
}

void convert(const std::string& src, const std::string& dst, Warnings* warnings, bool overwrite) {
  if (src.empty() || dst.empty()) fail("Не указан исходный или целевой путь");
  if (fs::absolute(src) == fs::absolute(dst)) fail("Исходный и целевой пути совпадают");
  auto source = openSource(src);
  LoadResult r = loadFrom(*source);
  if (warnings) warnings->insert(warnings->end(), r.warnings.begin(), r.warnings.end());
  std::vector<SnapshotInfo> infos;
  u64 head = 0;
  auto history = readHistory(*source, &infos, &head);
  if (isBundlePath(dst)) {
    if (fs::exists(dst) && !overwrite) fail("Файл «" + dst + "» уже существует");
    writeBundleFile(dst, buildBundle(worldFiles(r.world), history, infos, head));
    return;
  }
  if (fs::exists(dst) && !fs::isDir(dst)) fail("«" + dst + "» — файл, а не папка");
  if (fs::isDir(dst) && !fs::list(dst).empty() && !overwrite) fail("Папка «" + dst + "» не пуста");
  SaveOptions so;
  so.backups = false;
  save(dst, r.world, TB_ALL, nullptr, so);
  std::string err;
  for (auto& [rel, data] : history)
    if (!fs::writeFileAtomic(fs::join(dst, rel), data, &err)) fail("Не удалось записать «" + rel + "»: " + err);
  if (!history.empty() && !fs::writeFileAtomic(fs::join(dst, kHistoryIndex), indexText(infos, head), &err))
    fail("Не удалось записать историю ходов: " + err);
}

// ================================================================ снимки ходов
std::vector<SnapshotInfo> listSnapshots(const std::string& path) {
  auto src = openSource(path);
  return listFrom(*src);
}

SnapshotInfo writeSnapshot(const std::string& path, const World& w, const std::string& label) {
  auto src = openSource(path);
  SnapshotInfo info;
  info.turn = std::max(1, w.turn());
  info.file = std::string(kHistoryDir) + "/" + turnFileName(info.turn);
  info.at = nowIso();
  info.label = label;
  info.name = w.meta->name;
  std::vector<u8> gz = pack(w, 6);
  info.size = gz.size();
  std::string err;
  if (src->bundle()) {
    std::vector<SnapshotInfo> infos;
    u64 head = 0;
    auto history = readHistory(*src, &infos, &head);
    history.erase(std::remove_if(history.begin(), history.end(), [&](auto& h) { return h.first == info.file; }), history.end());
    infos.erase(std::remove_if(infos.begin(), infos.end(), [&](auto& s) { return s.file == info.file; }), infos.end());
    history.push_back({info.file, bytesStr(gz)});
    infos.push_back(info);
    std::sort(history.begin(), history.end());
    sortSnaps(infos);
    std::string bytes = buildBundle(rawWorldFiles(*src), history, infos, head);
    src.reset();
    writeBundleFile(path, bytes);
    return info;
  }
  u64 head = 0;
  listFrom(*src, &head);
  if (!fs::writeFileAtomic(fs::join(src->path(), info.file), gz, &err)) fail("Не удалось записать снимок хода: " + err);
  auto list = listFrom(*src);
  for (auto& s : list)
    if (s.file == info.file) s = info;
  if (!fs::writeFileAtomic(fs::join(src->path(), kHistoryIndex), indexText(list, head), &err)) fail("Не удалось записать историю ходов: " + err);
  return info;
}

LoadResult loadSnapshot(const std::string& path, int turn) {
  auto src = openSource(path);
  std::string file;
  for (auto& s : listFrom(*src))
    if (s.turn == turn) file = s.file;   // последний снимок хода
  if (file.empty()) fail("Нет снимка хода " + std::to_string(turn));
  std::string err;
  auto data = src->read(file, &err);
  if (!data) fail("Не удалось прочитать снимок хода " + std::to_string(turn) + ": " + err);
  return unpack(std::span<const u8>(reinterpret_cast<const u8*>(data->data()), data->size()), src->origin(file));
}

bool removeSnapshot(const std::string& path, int turn) {
  auto src = openSource(path);
  auto list = listFrom(*src);
  auto it = std::find_if(list.begin(), list.end(), [&](auto& s) { return s.turn == turn; });
  if (it == list.end()) return false;
  std::string file = it->file;
  list.erase(it);
  std::string err;
  u64 head = 0;
  if (src->bundle()) {
    std::vector<SnapshotInfo> infos;
    auto history = readHistory(*src, &infos, &head);
    history.erase(std::remove_if(history.begin(), history.end(), [&](auto& h) { return h.first == file; }), history.end());
    infos.erase(std::remove_if(infos.begin(), infos.end(), [&](auto& s) { return s.file == file; }), infos.end());
    std::string bytes = buildBundle(rawWorldFiles(*src), history, infos, head);
    src.reset();
    writeBundleFile(path, bytes);
    return true;
  }
  listFrom(*src, &head);
  if (!fs::remove(fs::join(src->path(), file), &err)) fail("Не удалось удалить снимок хода: " + err);
  if (!fs::writeFileAtomic(fs::join(src->path(), kHistoryIndex), indexText(list, head), &err)) fail("Не удалось записать историю ходов: " + err);
  return true;
}

// ---------------------------------------------------------------- история с ветвями (снимки с номерами)
History listHistory(const std::string& path) {
  auto src = openSource(path);
  History h;
  h.list = listFrom(*src, &h.head);
  return h;
}

namespace {

// Сведения нового снимка: файл с номером, размер — по данным.
SnapshotInfo newInfo(const NewSnapshot& n) {
  if (n.info.seq == 0) fail("Снимок хода без номера");
  SnapshotInfo s = n.info;
  s.turn = std::max(1, s.turn);
  s.file = std::string(kHistoryDir) + "/" + snapFileName(s.turn, s.seq);
  s.size = n.gz.size();
  return s;
}

// Добавить новые снимки к истории в памяти (файл с тем же именем заменяется).
void mergeNew(FileList& history, std::vector<SnapshotInfo>& infos, const std::vector<NewSnapshot>& add) {
  for (const NewSnapshot& n : add) {
    SnapshotInfo s = newInfo(n);
    history.erase(std::remove_if(history.begin(), history.end(), [&](auto& h) { return h.first == s.file; }), history.end());
    infos.erase(std::remove_if(infos.begin(), infos.end(), [&](auto& x) { return x.file == s.file; }), infos.end());
    history.push_back({s.file, bytesStr(n.gz)});
    infos.push_back(std::move(s));
  }
  std::sort(history.begin(), history.end());
  sortSnaps(infos);
}

void writeFolderHistory(const std::string& folder, const FileList& files, const std::vector<SnapshotInfo>& infos, u64 head) {
  std::string err;
  for (auto& [rel, data] : files)
    if (!fs::writeFileAtomic(fs::join(folder, rel), data, &err)) fail("Не удалось записать «" + rel + "»: " + err);
  if (!fs::writeFileAtomic(fs::join(folder, kHistoryIndex), indexText(infos, head), &err)) fail("Не удалось записать историю ходов: " + err);
}

}  // namespace

void addSnapshots(const std::string& path, const std::vector<NewSnapshot>& snaps, u64 head) {
  auto src = openSource(path);
  u64 oldHead = 0;
  if (src->bundle()) {
    std::vector<SnapshotInfo> infos;
    auto history = readHistory(*src, &infos, &oldHead);
    mergeNew(history, infos, snaps);
    std::string bytes = buildBundle(rawWorldFiles(*src), history, infos, head ? head : oldHead);
    src.reset();
    writeBundleFile(path, bytes);
    return;
  }
  std::vector<SnapshotInfo> infos = listFrom(*src, &oldHead);
  FileList files;
  mergeNew(files, infos, snaps);
  writeFolderHistory(src->path(), files, infos, head ? head : oldHead);
}

LoadResult loadSnapshotFile(const std::string& path, const std::string& file) {
  auto src = openSource(path);
  if (!src->has(file)) fail("Нет снимка хода «" + file + "»");
  std::string err;
  auto data = src->read(file, &err);
  if (!data) fail("Не удалось прочитать снимок хода «" + file + "»: " + err);
  return unpack(std::span<const u8>(reinterpret_cast<const u8*>(data->data()), data->size()), src->origin(file));
}

void saveBundleWith(const std::string& path, const World& w, const std::string& historyFrom, const std::vector<NewSnapshot>& add, u64 head) {
  if (path.empty()) fail("Не указан путь архива");
  if (fs::isDir(path)) fail("«" + path + "» — папка, а не файл архива");
  FileList history;
  std::vector<SnapshotInfo> infos;
  u64 oldHead = 0;
  if (!historyFrom.empty() && isProject(historyFrom)) {
    try {
      auto src = openSource(historyFrom);
      history = readHistory(*src, &infos, &oldHead);
    } catch (const UserError& e) {
      // Повреждённая прежняя версия самого архива истории не даёт — он перезаписывается.
      if (fs::absolute(historyFrom) != fs::absolute(path)) throw;
      logWarn("история архива не перенесена: %s", e.what());
      history.clear();
      infos.clear();
    }
  }
  mergeNew(history, infos, add);
  writeBundleFile(path, buildBundle(worldFiles(w), history, infos, head ? head : oldHead));
}

void replaceHistory(const std::string& to, const std::string& from) {
  if (to.empty() || !fs::isDir(to)) fail("История ходов: «" + to + "» — не папка мира");
  if (!from.empty() && fs::absolute(from) == fs::absolute(to)) return;
  FileList history;
  std::vector<SnapshotInfo> infos;
  u64 head = 0;
  if (!from.empty() && isProject(from)) {
    auto src = openSource(from);
    history = readHistory(*src, &infos, &head);
  }
  std::string err;
  std::string dir = fs::join(to, kHistoryDir);
  if (fs::exists(dir) && !fs::removeAll(dir, &err)) fail("Не удалось очистить историю ходов «" + dir + "»: " + err);
  if (history.empty()) return;
  writeFolderHistory(to, history, infos, head);
}

// ================================================================ мир целиком
std::string toJson(const World& w, int indent) {
  std::vector<Value> vals(F_COUNT);
  jobs::parallelFor(size_t(F_COUNT), [&](size_t i) { vals[i] = encode(w, FileId(i)); }, 1);
  Value files = Value::object();
  for (int i = 0; i < F_COUNT; i++) files.set(kFiles[i].path, std::move(vals[size_t(i)]));
  Value doc = Value::object();
  doc.set("format", kSnapshotFormat);
  doc.set("version", kVersion);
  doc.set("files", std::move(files));
  std::string out = json::write(doc, json::WriteOptions{std::max(0, indent), true, true});
  if (indent > 0) out.push_back('\n');
  return out;
}

LoadResult fromJson(std::string_view text, const std::string& origin) {
  Value doc = parseFile(text, origin);
  if (!doc.isObj() || doc.str("format") != kSnapshotFormat) fail(origin + ": это не снимок мира Regnum (format должен быть «regnum-snapshot»)");
  double ver = doc.num("version", 1);
  if (!std::isfinite(ver) || ver < 1) fail(origin + ": неверная версия формата снимка");
  if (std::floor(ver) > kVersion)
    fail("Снимок сохранён более новой версией Regnum (формат " + std::to_string(i64(ver)) + "). Обновите редактор.");
  const Value& files = doc.obj("files");
  const Value* world = files.find(kWorldFile);
  if (!world) fail(origin + ": в снимке нет world.json");
  LoadResult r;
  Warnings warnFiles[F_COUNT];
  checkVersion(*world, origin + ": world.json", warnFiles[F_WORLD]);
  Parts parts;
  jobs::parallelFor(size_t(F_COUNT), [&](size_t i) {
    const Value* v = files.find(kFiles[i].path);
    if (v) decode(*v, FileId(i), parts, warnFiles[i]);
    else warnFiles[i].push_back({kFiles[i].path, "", "файла нет в снимке — таблица пуста"});
  }, 1);
  r.warnings = std::move(warnFiles[F_WORLD]);
  for (size_t i = 0; i < F_WORLD; i++) r.warnings.insert(r.warnings.end(), warnFiles[i].begin(), warnFiles[i].end());
  for (auto& [k, v] : files.members())
    if (fileIndex(k) < 0) r.warnings.push_back({k, "", "неизвестный файл в снимке — отброшен"});
  r.world = assemble(std::move(parts), r.warnings, &r.fixedTables);
  return r;
}

std::vector<u8> pack(const World& w, int level) { return codec::deflate(toJson(w, 0), level, codec::ZFormat::Gzip); }

LoadResult unpack(std::span<const u8> gz, const std::string& origin) {
  std::string err;
  codec::InflateOptions o;
  o.maxOutput = kMaxUnpacked;
  auto raw = codec::inflate(gz, codec::ZFormat::Gzip, &err, o);
  if (!raw) fail(origin + ": повреждённые сжатые данные (" + err + ")");
  return fromJson(std::string_view(reinterpret_cast<const char*>(raw->data()), raw->size()), origin);
}

std::string fileText(const World& w, std::string_view rel) {
  int i = fileIndex(rel);
  if (i < 0) fail("Нет такого файла мира: «" + std::string(rel) + "»");
  return format(encode(w, FileId(i)), FileId(i));
}

// ================================================================ недавние проекты
namespace {

std::vector<RecentProject> readRecent(const std::string& dir) {
  std::vector<RecentProject> out;
  auto text = fs::readFile(fs::join(dir, "recent.json"));
  if (!text) return out;
  auto v = json::tryParse(*text);
  if (!v) return out;
  for (const Value& e : v->arr("recent")) {
    RecentProject r;
    r.path = e.str("path");
    if (r.path.empty()) continue;
    r.name = e.str("name");
    r.at = e.str("at");
    out.push_back(std::move(r));
  }
  return out;
}

void writeRecent(const std::string& dir, const std::vector<RecentProject>& list) {
  json::Array a;
  for (auto& r : list) {
    Value e = Value::object();
    e.set("path", r.path);
    e.set("name", r.name);
    e.set("at", r.at);
    a.push_back(std::move(e));
  }
  Value o = Value::object();
  o.set("recent", Value(std::move(a)));
  std::string text = json::write(o, json::WriteOptions{2, true, true});
  text.push_back('\n');
  std::string err;
  if (!fs::writeFileAtomic(fs::join(dir, "recent.json"), text, &err)) logWarn("недавние проекты не сохранены: %s", err.c_str());
}

}  // namespace

std::vector<RecentProject> recentProjects(const std::string& dataDir) {
  auto list = readRecent(dataDirOr(dataDir));
  for (auto& r : list) {
    r.exists = isProject(r.path);
    r.bundle = isBundlePath(r.path);
  }
  return list;
}

void addRecent(const std::string& path, const std::string& name, const std::string& dataDir) {
  std::string dir = dataDirOr(dataDir);
  std::string abs = fs::absolute(path);
  auto list = readRecent(dir);
  list.erase(std::remove_if(list.begin(), list.end(), [&](auto& r) { return r.path == abs; }), list.end());
  RecentProject r;
  r.path = abs;
  r.name = name;
  r.at = nowIso();
  list.insert(list.begin(), std::move(r));
  if (list.size() > size_t(kMaxRecent)) list.resize(size_t(kMaxRecent));
  writeRecent(dir, list);
}

void removeRecent(const std::string& path, const std::string& dataDir) {
  std::string dir = dataDirOr(dataDir);
  std::string abs = fs::absolute(path);
  auto list = readRecent(dir);
  size_t n = list.size();
  list.erase(std::remove_if(list.begin(), list.end(), [&](auto& r) { return r.path == abs || r.path == path; }), list.end());
  if (list.size() != n) writeRecent(dir, list);
}

// ================================================================ автосохранение
// <dataDir>/autosave/autosave-<слот>.json.gz — мир; autosave-<слот>.json — сведения (проект, время, ход).
AutosaveInfo writeAutosave(const World& w, const std::string& project, const std::string& dataDir) {
  std::string dir = fs::join(dataDirOr(dataDir), "autosave");
  std::string slot = autosaveSlot(project);
  AutosaveInfo info;
  info.file = fs::join(dir, "autosave-" + slot + ".json.gz");
  info.project = project.empty() ? std::string() : fs::absolute(project);
  info.name = w.meta->name;
  info.at = nowIso();
  info.turn = w.turn();
  std::vector<u8> gz = pack(w, 1);
  info.size = gz.size();
  std::string err;
  if (!fs::writeFileAtomic(info.file, gz, &err)) fail("Не удалось записать автосохранение: " + err);
  Value m = Value::object();
  m.set("project", info.project);
  m.set("name", info.name);
  m.set("at", info.at);
  m.set("turn", info.turn);
  std::string text = json::write(m, json::WriteOptions{2, true, true});
  text.push_back('\n');
  if (!fs::writeFileAtomic(fs::join(dir, "autosave-" + slot + ".json"), text, &err)) fail("Не удалось записать автосохранение: " + err);
  return info;
}

std::vector<AutosaveInfo> listAutosaves(const std::string& dataDir) {
  std::string dir = fs::join(dataDirOr(dataDir), "autosave");
  std::vector<std::pair<i64, AutosaveInfo>> found;
  for (auto& e : fs::list(dir)) {
    const std::string& n = e.name;
    if (e.dir || n.size() < 15 || n.compare(0, 9, "autosave-") != 0 || n.compare(n.size() - 5, 5, ".json") != 0) continue;
    std::string gz = fs::join(dir, n + ".gz");
    if (!fs::isFile(gz)) continue;
    auto text = fs::readFile(e.path);
    if (!text) continue;
    auto v = json::tryParse(*text);
    if (!v || !v->isObj()) continue;
    AutosaveInfo info;
    info.file = gz;
    info.project = v->str("project");
    info.name = v->str("name");
    info.at = v->str("at");
    info.turn = int(v->integer("turn", 0));
    info.size = fs::fileSize(gz).value_or(0);
    found.push_back({fs::mtime(gz).value_or(0), std::move(info)});
  }
  std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second.file < b.second.file; });
  std::vector<AutosaveInfo> out;
  for (auto& f : found) out.push_back(std::move(f.second));
  return out;
}

namespace {

std::optional<Autosave> loadAutosaveInfo(AutosaveInfo info, std::string* error) {
  std::string err;
  auto data = fs::readFile(info.file, &err);
  if (!data) {
    if (error) *error = err;
    return std::nullopt;
  }
  try {
    LoadResult r = unpack(std::span<const u8>(reinterpret_cast<const u8*>(data->data()), data->size()), "автосохранение");
    Autosave a;
    a.info = std::move(info);
    a.world = std::move(r.world);
    a.warnings = std::move(r.warnings);
    return a;
  } catch (const UserError& e) {
    if (error) *error = e.what();
    return std::nullopt;
  }
}

}  // namespace

std::optional<Autosave> loadAutosave(const std::string& dataDir, std::string* error) {
  auto list = listAutosaves(dataDir);
  if (list.empty()) {
    if (error) *error = "автосохранений нет";
    return std::nullopt;
  }
  return loadAutosaveInfo(std::move(list.front()), error);
}

std::optional<Autosave> loadAutosaveFor(const std::string& project, const std::string& dataDir, std::string* error) {
  std::string file = fs::join(fs::join(dataDirOr(dataDir), "autosave"), "autosave-" + autosaveSlot(project) + ".json.gz");
  for (auto& info : listAutosaves(dataDir))
    if (info.file == file) return loadAutosaveInfo(info, error);
  if (error) *error = "автосохранения этого мира нет";
  return std::nullopt;
}

void clearAutosave(const std::string& project, const std::string& dataDir) {
  std::string dir = fs::join(dataDirOr(dataDir), "autosave");
  std::string base = fs::join(dir, "autosave-" + autosaveSlot(project));
  fs::remove(base + ".json");
  fs::remove(base + ".json.gz");
}

}  // namespace rg::io
