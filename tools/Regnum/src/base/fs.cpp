// Regnum — файловая система (std::filesystem + немного системных вызовов для атомарной записи и папок).
#include "base/fs.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <knownfolders.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace rg::fs {

namespace stdfs = std::filesystem;

namespace {

std::string errText(const std::error_code& ec) {
  // Сообщения std на Windows приходят в кодировке консоли; оставляем только код.
  return strf("код %d", ec.value());
}

void setErr(std::string* error, const std::string& msg) {
  if (error) *error = msg;
}

#ifdef _WIN32
// Длинные пути Windows (>= MAX_PATH) — префикс \\?\ (требует абсолютного пути с обратными слешами).
stdfs::path longForm(stdfs::path p) {
  if (p.native().rfind(L"\\\\?\\", 0) == 0) return p;
  std::error_code ec;
  if (!p.is_absolute()) {
    // предел считается по полному пути: относительный путь дополняется текущей папкой
    stdfs::path a = stdfs::absolute(p, ec);
    if (ec || a.native().size() < 240) return p;
    p = a;
  } else if (p.native().size() < 240) {
    return p;
  }
  p = p.lexically_normal();
  p.make_preferred();
  std::wstring w = p.native();
  if (w.rfind(L"\\\\", 0) == 0) return stdfs::path(L"\\\\?\\UNC\\" + w.substr(2));
  return stdfs::path(L"\\\\?\\" + w);
}

std::wstring envW(const wchar_t* name) {
  const wchar_t* v = _wgetenv(name);
  return v ? std::wstring(v) : std::wstring();
}
#else
stdfs::path longForm(stdfs::path p) { return p; }

std::string envU(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}
#endif

FILE* openFile(const stdfs::path& p, bool write) {
#ifdef _WIN32
  return _wfopen(p.c_str(), write ? L"wb" : L"rb");
#else
  return std::fopen(p.c_str(), write ? "wb" : "rb");
#endif
}

bool flushToDisk(FILE* f) {
  if (std::fflush(f) != 0) return false;
#ifdef _WIN32
  HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f)));
  return h != INVALID_HANDLE_VALUE && FlushFileBuffers(h);
#else
  return ::fsync(fileno(f)) == 0;
#endif
}

i64 toUnixMs(stdfs::file_time_type t) {
  auto sys = std::chrono::file_clock::to_sys(t);
  return i64(std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count());
}

std::atomic<u64> gTmpCounter{0};

std::string tmpSuffix() {
  u64 k = gTmpCounter.fetch_add(1) + 1;
  u64 t = u64(std::chrono::steady_clock::now().time_since_epoch().count());
  u64 tid = u64(std::hash<std::thread::id>()(std::this_thread::get_id()));
  return strf(".tmp-%016llx", (unsigned long long)hashMix(hashMix(t, tid), k));
}

bool writeAtomicImpl(const std::string& target, const void* data, size_t size, std::string* error) {
  if (target.empty()) { setErr(error, "Пустой путь файла"); return false; }
  stdfs::path tp = path(target);
  std::string dir = parent(target);
  if (!dir.empty() && !makeDirs(dir, error)) return false;
  std::string tmp = target + tmpSuffix();
  stdfs::path tmpP = path(tmp);
  FILE* f = openFile(tmpP, true);
  if (!f) { setErr(error, "Не удалось создать файл «" + tmp + "»"); return false; }
  bool ok = size == 0 || std::fwrite(data, 1, size, f) == size;
  ok = flushToDisk(f) && ok;
  ok = std::fclose(f) == 0 && ok;
  if (!ok) {
    std::error_code ec;
    stdfs::remove(tmpP, ec);
    setErr(error, "Не удалось записать файл «" + target + "» (нет места или доступа)");
    return false;
  }
#ifdef _WIN32
  // Замену может временно блокировать антивирус или индексатор — несколько попыток.
  bool moved = false;
  DWORD lastErr = 0;
  for (int attempt = 0; attempt < 40 && !moved; attempt++) {
    if (MoveFileExW(tmpP.c_str(), tp.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { moved = true; break; }
    lastErr = GetLastError();
    if (lastErr != ERROR_ACCESS_DENIED && lastErr != ERROR_SHARING_VIOLATION && lastErr != ERROR_LOCK_VIOLATION) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(attempt < 10 ? 5 : 25));
  }
  if (!moved) {
    DeleteFileW(tmpP.c_str());
    setErr(error, strf("Не удалось заменить файл «%s» (код %lu)", target.c_str(), (unsigned long)lastErr));
    return false;
  }
#else
  if (::rename(tmpP.c_str(), tp.c_str()) != 0) {
    ::unlink(tmpP.c_str());
    setErr(error, "Не удалось заменить файл «" + target + "»");
    return false;
  }
  // Сохранить запись каталога.
  std::string d = dir.empty() ? std::string(".") : dir;
  int fd = ::open(path(d).c_str(), O_RDONLY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
#endif
  return true;
}

}  // namespace

// ================================================================ пути
namespace {
// Без преобразований через локаль std: на Windows — свой UTF-8 <-> UTF-16 (некорректные байты и одиночные
// суррогаты -> U+FFFD, без исключений), на других ОС путь — байты UTF-8 как есть.
stdfs::path plain(const std::string& s) {
#ifdef _WIN32
  return stdfs::path(utf8::toWide(s));
#else
  return stdfs::path(s);
#endif
}
}  // namespace

stdfs::path path(const std::string& utf8) { return longForm(plain(utf8)); }

std::string fromPath(const stdfs::path& p) {
#ifdef _WIN32
  std::string s = utf8::fromWide(p.generic_wstring());
  if (s.rfind("//?/UNC/", 0) == 0) s = "//" + s.substr(8);
  else if (s.rfind("//?/", 0) == 0) s = s.substr(4);
  return s;
#else
  return p.generic_string();
#endif
}

std::string join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return fromPath(plain(a) / plain(b));
}

std::string parent(const std::string& p) { return fromPath(plain(p).parent_path()); }
std::string filename(const std::string& p) { return fromPath(plain(p).filename()); }
std::string stem(const std::string& p) { return fromPath(plain(p).stem()); }
std::string ext(const std::string& p) { return fromPath(plain(p).extension()); }
bool isAbsolute(const std::string& p) { return plain(p).is_absolute(); }

std::string absolute(const std::string& p) {
  std::error_code ec;
  stdfs::path a = stdfs::absolute(plain(p), ec);
  if (ec) return p;
  return fromPath(a.lexically_normal());
}

// ================================================================ чтение и запись
std::optional<std::string> readFile(const std::string& p, std::string* error) {
  stdfs::path fp = path(p);
  FILE* f = openFile(fp, false);
  if (!f) { setErr(error, "Не удалось открыть файл «" + p + "»"); return std::nullopt; }
  std::string out;
  std::error_code ec;
  u64 sz = stdfs::file_size(fp, ec);
  if (!ec && sz > 0) {
    if (sz > u64(std::numeric_limits<size_t>::max() / 2)) {
      std::fclose(f);
      setErr(error, "Файл «" + p + "» слишком большой");
      return std::nullopt;
    }
    out.resize(size_t(sz));
    size_t got = std::fread(out.data(), 1, out.size(), f);
    out.resize(got);
  }
  // Дочитать то, что не учтено размером (растущие и специальные файлы).
  char buf[65536];
  for (;;) {
    size_t n = std::fread(buf, 1, sizeof buf, f);
    if (n == 0) break;
    out.append(buf, n);
  }
  bool bad = std::ferror(f) != 0;
  std::fclose(f);
  if (bad) { setErr(error, "Ошибка чтения файла «" + p + "»"); return std::nullopt; }
  return out;
}

bool writeFileAtomic(const std::string& p, std::string_view data, std::string* error) {
  return writeAtomicImpl(p, data.data(), data.size(), error);
}

bool writeFileAtomic(const std::string& p, std::span<const u8> data, std::string* error) {
  return writeAtomicImpl(p, data.data(), data.size(), error);
}

// ================================================================ сведения
bool exists(const std::string& p) {
  std::error_code ec;
  return !p.empty() && stdfs::exists(path(p), ec);
}

bool isDir(const std::string& p) {
  std::error_code ec;
  return !p.empty() && stdfs::is_directory(path(p), ec);
}

bool isFile(const std::string& p) {
  std::error_code ec;
  return !p.empty() && stdfs::is_regular_file(path(p), ec);
}

std::optional<u64> fileSize(const std::string& p) {
  std::error_code ec;
  stdfs::path fp = path(p);
  if (!stdfs::is_regular_file(fp, ec)) return std::nullopt;
  u64 s = stdfs::file_size(fp, ec);
  if (ec) return std::nullopt;
  return s;
}

std::optional<i64> mtime(const std::string& p) {
  std::error_code ec;
  auto t = stdfs::last_write_time(path(p), ec);
  if (ec) return std::nullopt;
  return toUnixMs(t);
}

std::vector<DirEntry> list(const std::string& dir) {
  std::vector<DirEntry> out;
  std::error_code ec;
  stdfs::path dp = path(dir);
  for (stdfs::directory_iterator it(dp, ec), end; !ec && it != end; it.increment(ec)) {
    const stdfs::directory_entry& e = *it;
    DirEntry d;
    d.name = fromPath(e.path().filename());
    d.path = join(dir, d.name);
    std::error_code ec2;
    d.dir = e.is_directory(ec2);
    std::error_code ec4;
    d.link = e.is_symlink(ec4);
#ifdef _WIN32
    DWORD attr = GetFileAttributesW(e.path().c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) d.link = true;
#endif
    if (!d.dir) {
      u64 s = e.file_size(ec2);
      d.size = ec2 ? 0 : s;
    }
    std::error_code ec3;
    auto t = e.last_write_time(ec3);
    d.mtime = ec3 ? 0 : toUnixMs(t);
    out.push_back(std::move(d));
  }
  std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
  return out;
}

std::vector<std::string> listTree(const std::string& dir) {
  std::vector<std::string> out;
  // Обход вручную: так относительные пути строятся без зависимости от префикса длинных путей.
  std::vector<std::string> stack{std::string()};
  while (!stack.empty()) {
    std::string rel = std::move(stack.back());
    stack.pop_back();
    for (auto& e : list(rel.empty() ? dir : join(dir, rel))) {
      std::string r = rel.empty() ? e.name : rel + "/" + e.name;
      if (e.dir && e.link) continue;  // не заходить по ссылкам: возможны циклы
      if (e.dir) stack.push_back(std::move(r));
      else out.push_back(std::move(r));
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

// ================================================================ изменения
bool makeDirs(const std::string& p, std::string* error) {
  if (p.empty()) return true;
  std::error_code ec;
  if (stdfs::is_directory(path(p), ec)) return true;
  // Цепочка от корня: create_directories ненадёжен с префиксом \\?\, поэтому по одному уровню.
  stdfs::path abs = stdfs::absolute(plain(p), ec);
  if (ec) { setErr(error, "Неверный путь «" + p + "»"); return false; }
  abs = abs.lexically_normal();
  std::vector<stdfs::path> chain;
  for (stdfs::path q = abs; !q.empty(); q = q.parent_path()) {
    if (q == q.root_path() || q.parent_path() == q) break;
    chain.push_back(q);
  }
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    stdfs::path np = longForm(*it);
    std::error_code e1;
    if (stdfs::is_directory(np, e1)) continue;
    std::error_code e2;
    stdfs::create_directory(np, e2);
    std::error_code e3;
    if (!stdfs::is_directory(np, e3)) {
      setErr(error, "Не удалось создать папку «" + fromPath(*it) + "» (" + errText(e2) + ")");
      return false;
    }
  }
  return true;
}

bool remove(const std::string& p, std::string* error) {
  std::error_code ec;
  stdfs::remove(path(p), ec);
  if (ec) { setErr(error, "Не удалось удалить «" + p + "» (" + errText(ec) + ")"); return false; }
  return true;
}

namespace {
// Рекурсивное удаление без перехода по ссылкам (символические ссылки и точки соединения удаляются сами).
// Своя реализация: remove_all не поддерживает длинные пути Windows.
bool removeTree(const stdfs::path& p, std::error_code& ec) {
  std::error_code e;
  auto st = stdfs::symlink_status(p, e);
  if (e || !stdfs::exists(st)) return true;
  bool link = stdfs::is_symlink(st);
#ifdef _WIN32
  DWORD attr = GetFileAttributesW(p.c_str());
  if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) link = true;
#endif
  if (stdfs::is_directory(st) && !link) {
    std::vector<stdfs::path> children;
    for (stdfs::directory_iterator it(p, e), end; !e && it != end; it.increment(e)) children.push_back(longForm(it->path()));
    if (e) {
      ec = e;
      return false;
    }
    bool ok = true;
    for (auto& c : children) ok = removeTree(c, ec) && ok;
    if (!ok) return false;
  }
  stdfs::remove(p, e);
  if (e) {
    ec = e;
    return false;
  }
  return true;
}
}  // namespace

bool removeAll(const std::string& p, std::string* error) {
  std::error_code ec;
  if (p.empty()) return true;
  if (!removeTree(path(p), ec)) {
    setErr(error, "Не удалось удалить «" + p + "» (" + errText(ec) + ")");
    return false;
  }
  return true;
}

bool rename(const std::string& from, const std::string& to, std::string* error) {
  std::string d = parent(to);
  if (!d.empty() && !makeDirs(d, error)) return false;
#ifdef _WIN32
  stdfs::path a = path(from), b = path(to);
  std::error_code ec;
  if (!stdfs::is_directory(a, ec)) {
    if (MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) return true;
    setErr(error, strf("Не удалось переименовать «%s» (код %lu)", from.c_str(), (unsigned long)GetLastError()));
    return false;
  }
#endif
  std::error_code ec2;
  stdfs::rename(path(from), path(to), ec2);
  if (ec2) { setErr(error, "Не удалось переименовать «" + from + "» (" + errText(ec2) + ")"); return false; }
  return true;
}

bool copyFile(const std::string& from, const std::string& to, std::string* error) {
  std::string d = parent(to);
  if (!d.empty() && !makeDirs(d, error)) return false;
  std::error_code ec;
  stdfs::copy_file(path(from), path(to), stdfs::copy_options::overwrite_existing, ec);
  if (ec) { setErr(error, "Не удалось скопировать «" + from + "» (" + errText(ec) + ")"); return false; }
  return true;
}

// ================================================================ системные папки
std::string exeDir() {
#ifdef _WIN32
  std::wstring buf(512, L'\0');
  for (;;) {
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    if (n == 0) return fromPath(stdfs::current_path());
    if (n < buf.size()) { buf.resize(n); break; }
    buf.resize(buf.size() * 2);
  }
  return fromPath(stdfs::path(buf).parent_path());
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buf(size + 1, '\0');
  if (_NSGetExecutablePath(buf.data(), &size) != 0) return fromPath(stdfs::current_path());
  buf.resize(std::strlen(buf.c_str()));
  std::error_code ec;
  stdfs::path p = stdfs::canonical(stdfs::path(buf), ec);
  if (ec) p = stdfs::path(buf);
  return fromPath(p.parent_path());
#else
  std::error_code ec;
  stdfs::path p = stdfs::read_symlink("/proc/self/exe", ec);
  if (ec) return fromPath(stdfs::current_path(ec));
  return fromPath(p.parent_path());
#endif
}

std::string homeDir() {
#ifdef _WIN32
  std::wstring h = envW(L"USERPROFILE");
  if (h.empty()) h = envW(L"HOMEDRIVE") + envW(L"HOMEPATH");
  if (h.empty()) return "C:/";
  return fromPath(stdfs::path(h));
#else
  std::string h = envU("HOME");
  if (h.empty()) {
    if (passwd* pw = getpwuid(getuid())) if (pw->pw_dir) h = pw->pw_dir;
  }
  return h.empty() ? std::string("/") : fromPath(path(h));
#endif
}

std::string documentsDir() {
#ifdef _WIN32
  PWSTR p = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)) && p) {
    std::string r = fromPath(stdfs::path(p));
    CoTaskMemFree(p);
    return r;
  }
  if (p) CoTaskMemFree(p);
  std::string d = join(homeDir(), "Documents");
  return isDir(d) ? d : homeDir();
#elif defined(__APPLE__)
  std::string d = join(homeDir(), "Documents");
  return isDir(d) ? d : homeDir();
#else
  // XDG: ~/.config/user-dirs.dirs, строка XDG_DOCUMENTS_DIR="$HOME/Документы"
  std::string home = homeDir();
  std::string cfg = envU("XDG_CONFIG_HOME");
  if (cfg.empty()) cfg = join(home, ".config");
  if (auto text = readFile(join(cfg, "user-dirs.dirs"))) {
    for (auto& line : split(*text, '\n')) {
      std::string l = trim(line);
      if (!startsWith(l, "XDG_DOCUMENTS_DIR=")) continue;
      std::string v = l.substr(18);
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
      if (startsWith(v, "$HOME")) v = home + v.substr(5);
      if (!v.empty() && v != home && isDir(v)) return fromPath(path(v));
    }
  }
  std::string d = join(home, "Documents");
  return isDir(d) ? d : home;
#endif
}

std::string userDataDir() {
  std::string d;
#ifdef _WIN32
  std::wstring app = envW(L"APPDATA");
  d = app.empty() ? join(homeDir(), "AppData/Roaming/Regnum") : join(fromPath(stdfs::path(app)), "Regnum");
#elif defined(__APPLE__)
  d = join(homeDir(), "Library/Application Support/Regnum");
#else
  std::string x = envU("XDG_CONFIG_HOME");
  d = (!x.empty() && x[0] == '/') ? join(x, "regnum") : join(homeDir(), ".config/regnum");
#endif
  makeDirs(d);
  return d;
}

std::string tempDir() {
  std::error_code ec;
  stdfs::path t = stdfs::temp_directory_path(ec);
  if (ec) return fromPath(stdfs::current_path(ec));
  std::string s = fromPath(t);
  while (s.size() > 1 && s.back() == '/') s.pop_back();
  return s;
}

}  // namespace rg::fs
