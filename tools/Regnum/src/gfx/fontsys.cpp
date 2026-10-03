// Regnum — поиск системных шрифтов с кириллицей (только std::filesystem и переменные окружения, без API ОС).
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "gfx/font.h"

namespace rg::gfx {

namespace stdfs = std::filesystem;

namespace {

std::string lowerAscii(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}

std::string toUtf8(const stdfs::path& p) {
  auto u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::string envStr(const char* name) {
  const char* v = std::getenv(name);
  return v && *v ? std::string(v) : std::string();
}

// Набор файлов одного семейства: обычное, полужирное, жирное начертание (nullptr — нет файла).
struct Candidate {
  const char* family;      // имя семейства для выбора начертания в TTC
  const char* files[3];
};

// Порядок — приоритет. Имена файлов сравниваются без учёта регистра.
const Candidate kUi[] = {
    {"Segoe UI", {"segoeui.ttf", "seguisb.ttf", "segoeuib.ttf"}},
    {"System Font", {"SFNS.ttf", nullptr, nullptr}},
    {"Helvetica Neue", {"HelveticaNeue.ttc", "HelveticaNeue.ttc", "HelveticaNeue.ttc"}},
    {"Noto Sans", {"NotoSans-Regular.ttf", "NotoSans-SemiBold.ttf", "NotoSans-Bold.ttf"}},
    {"Ubuntu", {"Ubuntu-R.ttf", "Ubuntu-M.ttf", "Ubuntu-B.ttf"}},
    {"Ubuntu", {"Ubuntu-Regular.ttf", "Ubuntu-Medium.ttf", "Ubuntu-Bold.ttf"}},
    {"Open Sans", {"OpenSans-Regular.ttf", "OpenSans-SemiBold.ttf", "OpenSans-Bold.ttf"}},
    {"DejaVu Sans", {"DejaVuSans.ttf", nullptr, "DejaVuSans-Bold.ttf"}},
    {"Liberation Sans", {"LiberationSans-Regular.ttf", nullptr, "LiberationSans-Bold.ttf"}},
    {"Arial", {"arial.ttf", nullptr, "arialbd.ttf"}},
    {"Arial", {"Arial.ttf", nullptr, "Arial Bold.ttf"}},
    {"Tahoma", {"tahoma.ttf", nullptr, "tahomabd.ttf"}},
    {"Verdana", {"verdana.ttf", nullptr, "verdanab.ttf"}},
    {"Calibri", {"calibri.ttf", nullptr, "calibrib.ttf"}},
};
const Candidate kDisplay[] = {
    {"Cambria", {"cambria.ttc", nullptr, "cambriab.ttf"}},
    {"Georgia", {"georgia.ttf", nullptr, "georgiab.ttf"}},
    {"Georgia", {"Georgia.ttf", nullptr, "Georgia Bold.ttf"}},
    {"Constantia", {"constan.ttf", nullptr, "constanb.ttf"}},
    {"Noto Serif", {"NotoSerif-Regular.ttf", "NotoSerif-SemiBold.ttf", "NotoSerif-Bold.ttf"}},
    {"PT Serif", {"PTSerif.ttc", nullptr, "PTSerif.ttc"}},
    {"DejaVu Serif", {"DejaVuSerif.ttf", nullptr, "DejaVuSerif-Bold.ttf"}},
    {"Liberation Serif", {"LiberationSerif-Regular.ttf", nullptr, "LiberationSerif-Bold.ttf"}},
    {"Times New Roman", {"times.ttf", nullptr, "timesbd.ttf"}},
    {"Times New Roman", {"Times New Roman.ttf", nullptr, "Times New Roman Bold.ttf"}},
};
const Candidate kMono[] = {
    {"Consolas", {"consola.ttf", nullptr, "consolab.ttf"}},
    {"Menlo", {"Menlo.ttc", nullptr, "Menlo.ttc"}},
    {"DejaVu Sans Mono", {"DejaVuSansMono.ttf", nullptr, "DejaVuSansMono-Bold.ttf"}},
    {"Noto Sans Mono", {"NotoSansMono-Regular.ttf", "NotoSansMono-SemiBold.ttf", "NotoSansMono-Bold.ttf"}},
    {"Ubuntu Mono", {"UbuntuMono-R.ttf", nullptr, "UbuntuMono-B.ttf"}},
    {"Ubuntu Mono", {"UbuntuMono-Regular.ttf", nullptr, "UbuntuMono-Bold.ttf"}},
    {"Liberation Mono", {"LiberationMono-Regular.ttf", nullptr, "LiberationMono-Bold.ttf"}},
    {"Courier New", {"cour.ttf", nullptr, "courbd.ttf"}},
    {"Courier New", {"Courier New.ttf", nullptr, "Courier New Bold.ttf"}},
    {"Lucida Console", {"lucon.ttf", nullptr, nullptr}},
};
// Запасные шрифты для символов, отсутствующих в основных: значки, стрелки, математика, прочие письменности.
const Candidate kFallback[] = {
    {"Segoe UI Symbol", {"seguisym.ttf"}},
    {"Segoe UI", {"segoeui.ttf"}},
    {"Arial", {"arial.ttf"}},
    {"Cambria Math", {"cambria.ttc"}},
    {"Segoe UI Emoji", {"seguiemj.ttf"}},
    {"Segoe UI Historic", {"seguihis.ttf"}},
    {"Apple Symbols", {"Apple Symbols.ttf"}},
    {"Arial Unicode MS", {"Arial Unicode.ttf"}},
    {"Helvetica Neue", {"HelveticaNeue.ttc"}},
    {"DejaVu Sans", {"DejaVuSans.ttf"}},
    {"Noto Sans Symbols", {"NotoSansSymbols-Regular.ttf"}},
    {"Noto Sans Symbols2", {"NotoSansSymbols2-Regular.ttf"}},
    {"Noto Sans Math", {"NotoSansMath-Regular.ttf"}},
    {"FreeSerif", {"FreeSerif.ttf"}},
};

const int kWeightValue[3] = {400, 600, 700};

struct Index {
  std::map<std::string, stdfs::path> byName;  // имя файла в нижнем регистре -> путь (первый найденный)
  std::vector<stdfs::path> all;
  std::vector<std::string> dirs;
};

void scanDir(Index& idx, const stdfs::path& dir) {
  std::error_code ec;
  if (!stdfs::is_directory(dir, ec)) return;
  idx.dirs.push_back(toUtf8(dir));
  constexpr int kMaxDepth = 5;
  constexpr size_t kMaxFiles = 40000;
  stdfs::recursive_directory_iterator it(dir, stdfs::directory_options::skip_permission_denied, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (it.depth() >= kMaxDepth) { it.disable_recursion_pending(); continue; }
    std::error_code e2;
    if (!it->is_regular_file(e2)) continue;
    const stdfs::path& p = it->path();
    std::string ext = lowerAscii(toUtf8(p.extension()));
    if (ext != ".ttf" && ext != ".ttc" && ext != ".otf" && ext != ".otc") continue;
    std::string name = lowerAscii(toUtf8(p.filename()));
    idx.byName.emplace(name, p);
    idx.all.push_back(p);
    if (idx.all.size() >= kMaxFiles) break;
  }
}

Index buildIndex(const std::vector<std::string>& utf8Dirs) {
  Index idx;
  // Без повторов (вложенные каталоги тоже пропускаются — они уже входят в рекурсивный обход).
  std::vector<std::string> seen;
  for (const std::string& ds : utf8Dirs) {
    stdfs::path d(reinterpret_cast<const char8_t*>(ds.c_str()));
    std::error_code ec;
    stdfs::path canon = stdfs::weakly_canonical(d, ec);
    std::string key = lowerAscii(toUtf8(ec ? d : canon));
    auto isSep = [](char ch) { return ch == '/' || ch == '\\'; };
    while (!key.empty() && isSep(key.back())) key.pop_back();
    bool dup = false;
    for (auto& s : seen) {
      if (key == s || (startsWith(key, s) && key.size() > s.size() && isSep(key[s.size()]))) { dup = true; break; }
    }
    if (dup) continue;
    seen.push_back(key);
    scanDir(idx, d);
  }
  return idx;
}

// Кеш загруженных файлов (одни данные для всех начертаний TTC).
struct Loader {
  std::map<std::string, std::shared_ptr<const std::string>> data;
  std::map<std::pair<std::string, int>, std::shared_ptr<const Font>> fonts;

  std::shared_ptr<const std::string> read(const stdfs::path& p) {
    std::string key = toUtf8(p);
    auto it = data.find(key);
    if (it != data.end()) return it->second;
    std::shared_ptr<const std::string> d;
    std::error_code ec;
    auto sz = stdfs::file_size(p, ec);
    if (!ec && sz > 12 && sz < (u64(256) << 20)) {
      // Открытие через std::filesystem::path переносимо по кодировкам.
      std::string bytes;
      bytes.resize(size_t(sz));
      std::ifstream in(p, std::ios::binary);
      if (in && in.read(bytes.data(), std::streamsize(sz))) d = std::make_shared<const std::string>(std::move(bytes));
    }
    data[key] = d;
    return d;
  }

  std::shared_ptr<const Font> font(const stdfs::path& p, int index) {
    auto key = std::make_pair(toUtf8(p), index);
    auto it = fonts.find(key);
    if (it != fonts.end()) return it->second;
    std::shared_ptr<const Font> f;
    if (auto d = read(p)) f = Font::load(d, index);
    fonts[key] = f;
    return f;
  }

  int faces(const stdfs::path& p) {
    auto d = read(p);
    return d ? Font::faceCount(*d) : 0;
  }
};

bool familyMatches(const Font& f, const char* family) {
  if (!family) return true;
  std::string a = lowerAscii(f.family()), b = lowerAscii(family);
  return a == b;
}

// Выбор начертания: в TTC — по семейству, ширине и насыщенности; в одиночном файле — само начертание.
std::shared_ptr<const Font> pickFace(Loader& ld, const stdfs::path& p, const char* family, int weight, int* faceIndex, bool strictWeight) {
  int n = ld.faces(p);
  if (n <= 0) return nullptr;
  if (n == 1) {
    auto f = ld.font(p, 0);
    if (faceIndex) *faceIndex = 0;
    return f;
  }
  std::shared_ptr<const Font> best;
  int bestScore = 1 << 30;
  for (int i = 0; i < n && i < 64; i++) {
    auto f = ld.font(p, i);
    if (!f || f->italic()) continue;
    int score = std::abs(f->weightClass() - weight);
    if (f->widthClass() != 5) score += 1000;
    if (!familyMatches(*f, family)) score += 2000;
    if (score < bestScore) { bestScore = score; best = f; if (faceIndex) *faceIndex = i; }
  }
  if (!best) return nullptr;
  if (strictWeight && std::abs(best->weightClass() - weight) > 150) return nullptr;
  if (bestScore >= 2000) {
    // Семейство с другим именем допустимо, только если в коллекции нет точного.
    if (family && !familyMatches(*best, family)) {
      std::string a = lowerAscii(best->family()), b = lowerAscii(family);
      if (a.find(b) == std::string::npos) return nullptr;
    }
  }
  return best;
}

bool chooseFamily(Loader& ld, const Index& idx, const Candidate* cands, size_t count, FontFace out[3]) {
  for (size_t c = 0; c < count; c++) {
    const Candidate& cand = cands[c];
    auto it = idx.byName.find(lowerAscii(cand.files[0]));
    if (it == idx.byName.end()) continue;
    int fi = 0;
    auto reg = pickFace(ld, it->second, cand.family, 400, &fi, true);
    if (!reg || !hasCyrillic(*reg)) continue;
    out[0] = {toUtf8(it->second), fi, reg, Synth::None};
    // Сначала жирное, затем полужирное: без файла полужирного берётся настоящее жирное (как в CSS для 600),
    // синтетическое утолщение — только когда нет ни того, ни другого.
    for (int w : {2, 1}) {
      out[w] = {out[0].path, out[0].index, reg, w == 1 ? Synth::Semibold : Synth::Bold};
      if (w == 1 && out[2].synth == Synth::None) out[1] = out[2];
      if (!cand.files[w]) continue;
      auto wit = idx.byName.find(lowerAscii(cand.files[w]));
      if (wit == idx.byName.end()) continue;
      int wi = 0;
      auto wf = pickFace(ld, wit->second, cand.family, kWeightValue[w], &wi, true);
      if (!wf || !hasCyrillic(*wf) || wf.get() == reg.get()) continue;
      out[w] = {toUtf8(wit->second), wi, wf, Synth::None};
    }
    return true;
  }
  return false;
}

// Последняя надежда: любой шрифт с кириллицей (без засечек — по имени, моноширинный — по флагу post).
bool anyFamily(Loader& ld, const Index& idx, bool wantMono, FontFace out[3]) {
  std::shared_ptr<const Font> best;
  std::string bestPath;
  int bestIdx = 0, bestScore = 1 << 30;
  size_t tried = 0;
  for (const auto& p : idx.all) {
    if (++tried > 400) break;
    int n = ld.faces(p);
    for (int i = 0; i < n && i < 8; i++) {
      auto f = ld.font(p, i);
      if (!f || !hasCyrillic(*f)) continue;
      int score = std::abs(f->weightClass() - 400) + (f->italic() ? 500 : 0) + (f->widthClass() != 5 ? 300 : 0);
      std::string name = lowerAscii(f->family());
      if (wantMono) score += f->monospace() ? 0 : 5000;
      else score += name.find("sans") != std::string::npos ? 0 : 100;
      if (score < bestScore) { bestScore = score; best = f; bestPath = toUtf8(p); bestIdx = i; }
    }
  }
  if (!best || (wantMono && !best->monospace())) return false;
  out[0] = {bestPath, bestIdx, best, Synth::None};
  out[1] = {bestPath, bestIdx, best, Synth::Semibold};
  out[2] = {bestPath, bestIdx, best, Synth::Bold};
  return true;
}

}  // namespace

bool hasCyrillic(const Font& f) {
  static const u32 probe[] = {0x0410, 0x0416, 0x042F, 0x0430, 0x0436, 0x044F, 0x0451, 'A', 'z', '0'};
  for (u32 cp : probe) if (!f.hasGlyph(cp)) return false;
  return true;
}

std::vector<std::string> systemFontDirs() {
  std::vector<std::string> dirs;
  auto add = [&](const stdfs::path& p) { dirs.push_back(toUtf8(p)); };
  // Переменные окружения — в кодировке ОС: путь строится из «родной» строки.
  std::string extra = envStr("REGNUM_FONT_DIRS");
  for (auto& part : split(extra, ';')) {
    std::string t = trim(part);
    if (!t.empty()) add(stdfs::path(t));
  }
  std::string win = envStr("WINDIR");
  if (win.empty()) win = envStr("SystemRoot");
  if (!win.empty()) add(stdfs::path(win) / "Fonts");
  std::string local = envStr("LOCALAPPDATA");
  if (!local.empty()) add(stdfs::path(local) / "Microsoft" / "Windows" / "Fonts");
  std::string home = envStr("HOME");
  add("/System/Library/Fonts");
  add("/System/Library/Fonts/Supplemental");
  add("/Library/Fonts");
  if (!home.empty()) add(stdfs::path(home) / "Library" / "Fonts");
  add("/usr/share/fonts");
  add("/usr/local/share/fonts");
  std::string xdgHome = envStr("XDG_DATA_HOME");
  if (!xdgHome.empty()) add(stdfs::path(xdgHome) / "fonts");
  else if (!home.empty()) add(stdfs::path(home) / ".local" / "share" / "fonts");
  if (!home.empty()) add(stdfs::path(home) / ".fonts");
  for (auto& part : split(envStr("XDG_DATA_DIRS"), ':')) {
    std::string t = trim(part);
    if (!t.empty() && t[0] == '/') add(stdfs::path(t) / "fonts");
  }
  return dirs;
}

bool findSystemFonts(SystemFonts& out, std::string* error) { return findFontsIn(systemFontDirs(), out, error); }

bool findFontsIn(const std::vector<std::string>& dirs, SystemFonts& out, std::string* error) {
  out = SystemFonts{};
  Index idx = buildIndex(dirs);
  out.dirs = idx.dirs;
  Loader ld;
  bool ui = chooseFamily(ld, idx, kUi, std::size(kUi), out.faces[0]);
  if (!ui) ui = anyFamily(ld, idx, false, out.faces[0]);
  if (!ui) {
    if (error) *error = "Не найден системный шрифт с кириллицей. Установите шрифт DejaVu Sans или Noto Sans и перезапустите редактор.";
    return false;
  }
  if (!chooseFamily(ld, idx, kDisplay, std::size(kDisplay), out.faces[1]))
    for (int w = 0; w < 3; w++) out.faces[1][w] = out.faces[0][w];
  if (!chooseFamily(ld, idx, kMono, std::size(kMono), out.faces[2]) && !anyFamily(ld, idx, true, out.faces[2]))
    for (int w = 0; w < 3; w++) out.faces[2][w] = out.faces[0][w];

  // Запасные: только существующие файлы, без повторов основных; загрузка — по требованию.
  std::set<std::pair<std::string, int>> used;
  for (auto& f : out.faces) for (auto& w : f) used.insert({w.path, w.index});
  for (const Candidate& c : kFallback) {
    auto it = idx.byName.find(lowerAscii(c.files[0]));
    if (it == idx.byName.end()) continue;
    FontFace face;
    face.path = toUtf8(it->second);
    face.index = 0;
    if (ld.data.count(face.path)) {
      // Файл уже прочитан при выборе семейств: выбрать начертание сразу.
      int fi = 0;
      if (auto f = pickFace(ld, it->second, c.family, 400, &fi, false)) { face.font = f; face.index = fi; }
      else continue;
    } else if (lowerAscii(toUtf8(it->second.extension())) == ".ttc") {
      int fi = 0;
      auto f = pickFace(ld, it->second, c.family, 400, &fi, false);
      if (!f) continue;
      face.font = f;
      face.index = fi;
    }
    if (used.count({face.path, face.index})) continue;
    used.insert({face.path, face.index});
    out.fallbacks.push_back(std::move(face));
  }
  return true;
}

}  // namespace rg::gfx
