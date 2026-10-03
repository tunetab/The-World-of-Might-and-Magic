// Regnum — персонажи: общие части панели и инспектора. Портреты (чтение файла PNG/JPEG, уменьшение больших,
// фоновое декодирование и кеш по персонажу и содержимому), роли персонажа (правитель, лорд провинций, совет,
// войска и флот — ТЗ 1.a.vi, 1.b.iv, 1.c.iii), удаление с подтверждением, поиск карточки кампании (канон).
#include <algorithm>
#include <future>

#include "app/app_internal.h"
#include "app/widgets.h"
#include "base/fs.h"
#include "base/jobs.h"
#include "codec/jpeg.h"
#include "codec/png.h"

namespace rg::app::chars {

// ---- объявления (используются в characters.cpp и character_inspector.cpp; там — точные копии)
struct Roles {
  std::vector<Id> rulerOf;                   // фракции, где персонаж — правитель
  std::vector<Id> lordOf;                    // провинции, где он лорд
  std::vector<std::pair<Id, Id>> seats;      // место в совете: фракция, ID места
  std::vector<Id> armies;                    // войска и флоты, где он герой
  std::vector<Id> commands;                  // из них — главный полководец
  size_t total() const { return rulerOf.size() + lordOf.size() + seats.size() + armies.size(); }
};
Roles rolesOf(const World& w, Id character);
std::string rolesText(const World& w, const Roles& r);
std::string roleLine(const World& w, const Character& c, const Roles& r);
const gfx::Image* portraitImage(const Character& c, bool square);
void avatar(const Character& c, float size, bool ring, std::string_view tip);
void askDelete(App& a, Id character);
void loadPortrait(App& a, Id character);
bool setPortraitFromFile(App& a, Id character, const std::string& path);
void askClearPortrait(App& a, Id character);
std::optional<std::string> findCanonCard(App& a, std::string_view entity, std::string_view name, std::string* foundId);
std::string positionOf(const World& w, Id faction, Id seat);

// ================================================================ роли
Roles rolesOf(const World& w, Id cid) {
  Roles r;
  if (!cid) return r;
  w.factions.each([&](const Faction& f) {
    if (f.ruler == cid) r.rulerOf.push_back(f.id);
    for (const CouncilSeat& s : f.council)
      if (s.character == cid) r.seats.push_back({f.id, s.id});
  });
  w.provinces.each([&](const Province& p) {
    if (p.lord == cid) r.lordOf.push_back(p.id);
  });
  w.armies.each([&](const Army& a) {
    bool hero = false;
    for (const ArmyGroup& g : a.groups) hero = hero || std::find(g.heroes.begin(), g.heroes.end(), cid) != g.heroes.end();
    if (hero || a.commander == cid) r.armies.push_back(a.id);
    if (a.commander == cid) r.commands.push_back(a.id);
  });
  std::sort(r.lordOf.begin(), r.lordOf.end(), [&](Id x, Id y) { return compareRu(w.provinceName(x), w.provinceName(y)) < 0; });
  return r;
}

std::string positionOf(const World& w, Id faction, Id seat) {
  if (const Faction* f = w.faction(faction))
    for (const CouncilSeat& s : f->council)
      if (s.id == seat) return s.position.empty() ? std::string("Советник") : s.position;
  return {};
}

std::string rolesText(const World& w, const Roles& r) {
  std::vector<std::string> parts;
  for (Id f : r.rulerOf) parts.push_back("правитель «" + w.factionName(f) + "»");
  if (!r.lordOf.empty())
    parts.push_back(r.lordOf.size() == 1 ? "лорд провинции «" + w.provinceName(r.lordOf[0]) + "»"
                                         : "лорд " + fmtInt(i64(r.lordOf.size())) + " " + plural(i64(r.lordOf.size()), "провинции", "провинций", "провинций"));
  for (auto [f, s] : r.seats) parts.push_back(utf8::lower(positionOf(w, f, s)) + " в совете «" + w.factionName(f) + "»");
  if (!r.armies.empty())
    parts.push_back(r.commands.empty() ? "герой " + fmtInt(i64(r.armies.size())) + " " + plural(i64(r.armies.size()), "войска", "войск", "войск")
                                       : "полководец");
  return join(parts, ", ");
}

// Подпись строки списка: титул и главная роль.
std::string roleLine(const World& w, const Character& c, const Roles& r) {
  std::vector<std::string> parts;
  if (!c.title.empty()) parts.push_back(c.title);
  if (!r.rulerOf.empty()) {
    if (c.title.empty()) parts.push_back("Правитель");
  } else if (!r.seats.empty()) {
    parts.push_back(positionOf(w, r.seats[0].first, r.seats[0].second));
  }
  if (!r.lordOf.empty()) parts.push_back("лорд: " + w.provinceName(r.lordOf[0]) + (r.lordOf.size() > 1 ? " +" + std::to_string(r.lordOf.size() - 1) : ""));
  if (!r.commands.empty()) parts.push_back("командует войском");
  if (parts.empty()) return c.hero ? "Герой" : "Без ролей";
  return join(parts, " · ");
}

// ================================================================ портреты
namespace {

struct Entry {
  const char* data = nullptr;   // тождество строки портрета (быстрая проверка без хеша)
  size_t size = 0;
  u64 hash = 0;
  std::future<std::shared_ptr<gfx::Image>> job, cardJob;
  std::shared_ptr<gfx::Image> square, card;   // аватар 128 × 128; карточка 3:4 (до 300 × 400) — только у одного
  bool failed = false;
};
std::unordered_map<Id, Entry>& cache() {
  static std::unordered_map<Id, Entry> c;
  return c;
}
Id& cardOwner() {   // чья карточка держится в памяти (инспектор показывает одного персонажа)
  static Id id = 0;
  return id;
}

// Вырезать прямоугольник заданных пропорций (лицо обычно выше центра — сдвиг к верху).
gfx::Image cropAspect(const gfx::Image& img, double aspect) {
  int w = img.w, h = img.h;
  double cur = double(w) / std::max(1, h);
  gfx::RectI r{0, 0, w, h};
  if (cur > aspect) {
    int cw = std::max(1, int(std::lround(h * aspect)));
    r = gfx::RectI{(w - cw) / 2, 0, cw, h};
  } else {
    int ch = std::max(1, int(std::lround(w / aspect)));
    r = gfx::RectI{0, int(std::lround((h - ch) * 0.15)), w, ch};
  }
  return img.cropped(r);
}

// Декодирование и подгонка (фоновая задача): aspect 1 — аватар, 0,75 — карточка.
std::shared_ptr<gfx::Image> decode(const std::string& bytes, double aspect, int maxW) {
  auto rgba = codec::decodeImage(bytes);
  if (!rgba || rgba->empty()) return nullptr;
  gfx::Image full = gfx::Image::fromRgba(rgba->rgba.data(), rgba->w, rgba->h);
  gfx::Image cut = cropAspect(full, aspect);
  int w = std::max(1, std::min(maxW, cut.w));
  return std::make_shared<gfx::Image>(cut.scaled(w, std::max(1, int(std::lround(w / aspect)))));
}

// Готовность фоновой задачи без ожидания.
template <class T>
bool poll(std::future<T>& f, T& out) {
  if (!f.valid()) return false;
  if (f.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    ui::requestRedraw();   // ещё декодируется — следующий кадр покажет результат
    return false;
  }
  out = f.get();
  return true;
}

}  // namespace

const gfx::Image* portraitImage(const Character& c, bool square) {
  auto& m = cache();
  if (c.portrait.empty()) {
    m.erase(c.id);
    return nullptr;
  }
  Entry& e = m[c.id];
  if (e.data != c.portrait.data() || e.size != c.portrait.size()) {
    u64 h = hash64(c.portrait);
    if (h != e.hash || (!e.job.valid() && !e.square && !e.failed)) {
      e = Entry{};
      e.hash = h;
      e.job = jobs::submit([bytes = c.portrait] { return decode(bytes, 1.0, 128); });
    }
    e.data = c.portrait.data();
    e.size = c.portrait.size();
  }
  std::shared_ptr<gfx::Image> got;
  if (poll(e.job, got)) {
    e.square = got;
    e.failed = !got;
  }
  if (e.failed) return nullptr;
  if (square) return e.square ? e.square.get() : nullptr;
  // Карточка: держится только у одного персонажа.
  Id& owner = cardOwner();
  if (owner != c.id) {
    if (auto it = m.find(owner); it != m.end()) it->second.card.reset();
    owner = c.id;
  }
  if (!e.card && !e.cardJob.valid()) e.cardJob = jobs::submit([bytes = c.portrait] { return decode(bytes, 0.75, 300); });
  if (poll(e.cardJob, got)) e.card = got;
  return e.card ? e.card.get() : nullptr;
}

void avatar(const Character& c, float size, bool ring, std::string_view tip) {
  ui::avatar(c.name.empty() ? std::string_view("?") : std::string_view(c.name),
             {.image = portraitImage(c, true), .size = size, .ring = ring, .tooltip = tip});
}

bool setPortraitFromFile(App& a, Id cid, const std::string& path) {
  std::string err;
  auto bytes = fs::readFile(path, &err);
  if (!bytes) {
    a.toast("Не удалось прочитать файл: " + fs::filename(path), ToastKind::Warning, "warning");
    return false;
  }
  auto img = codec::decodeImage(*bytes);
  if (!img || img->empty()) {
    a.toast("Файл не похож на изображение PNG или JPEG", ToastKind::Warning, "image");
    return false;
  }
  std::string data = std::move(*bytes);
  bool shrunk = false;
  // Большие изображения уменьшаются (мир хранит портрет внутри файла персонажей).
  constexpr int kMaxSide = 1024, kTarget = 512;
  constexpr size_t kMaxBytes = size_t(2) << 20;
  if (std::max(img->w, img->h) > kMaxSide || data.size() > kMaxBytes) {
    gfx::Image g = gfx::Image::fromRgba(img->rgba.data(), img->w, img->h);
    double k = double(kTarget) / std::max(img->w, img->h);
    if (k < 1) g = g.scaled(std::max(1, int(std::lround(img->w * k))), std::max(1, int(std::lround(img->h * k))));
    codec::RgbaImage out;
    out.w = g.w;
    out.h = g.h;
    out.rgba = g.toRgba();
    std::vector<u8> png = codec::encodePng(out, 6);
    data.assign(png.begin(), png.end());
    shrunk = true;
  }
  bool ok = a.act("Портрет персонажа", [&](Tx& tx) { tx.character(cid).portrait = data; });
  if (ok && shrunk) a.toast("Портрет уменьшен до " + std::to_string(kTarget) + " точек", ToastKind::Info, "image");
  return ok;
}

void loadPortrait(App& a, Id cid) {
  if (a.readOnly()) {
    a.act("Портрет персонажа", [](Tx&) {});   // покажет отказ с подсказкой
    return;
  }
  detail::later(a, [cid](App& x) {
    static std::string lastDir;
    std::string start = !lastDir.empty() ? lastDir : (x.projectPath().empty() ? fs::documentsDir() : fs::parent(x.projectPath()));
    if (!platform::dialogsSupported()) {
      x.prompt("Портрет персонажа", "Путь к файлу PNG или JPEG", "", [cid](App& y, const std::string& p) {
        if (setPortraitFromFile(y, cid, trim(p))) lastDir = fs::parent(trim(p));
      });
      return;
    }
    auto path = platform::openFileDialog("Портрет персонажа", {{"Изображения PNG и JPEG", {"png", "jpg", "jpeg"}}}, start);
    if (!path) return;
    if (setPortraitFromFile(x, cid, *path)) lastDir = fs::parent(*path);
  });
}

void askClearPortrait(App& a, Id cid) {
  const Character* c = a.world().character(cid);
  if (!c || c->portrait.empty()) return;
  a.confirm("Убрать портрет?", "Портрет «" + c->name + "» будет удалён из мира. Действие можно отменить Ctrl+Z.", "Убрать", true,
            [cid](App& x) { x.act("Убрать портрет", [&](Tx& tx) { tx.character(cid).portrait.clear(); }); });
}

void askDelete(App& a, Id cid) {
  const World& w = a.world();
  const Character* c = w.character(cid);
  if (!c) return;
  Roles r = rolesOf(w, cid);
  std::string name = "«" + (c->name.empty() ? std::string("Без имени") : c->name) + "»";
  std::string text = r.total() ? name + " будет снят со всех ролей: " + rolesText(w, r) + "." : name + " пока без ролей.";
  text += " Действие можно отменить Ctrl+Z.";
  a.confirm("Удалить персонажа?", text, "Удалить", true, [cid](App& x) {
    if (x.act("Удалить персонажа", [&](Tx& tx) { rules::removeCharacter(tx, cid); }) && x.ui.sel == Selection{SelType::Character, cid})
      x.clearSelection();
  });
}

// ================================================================ карточка кампании (канон)
namespace {

// Поля карточки: заголовок «# Имя», «id: …», «aliases: [...]».
struct CardMeta {
  std::string title, id;
  std::vector<std::string> aliases;
};
CardMeta readCardMeta(const std::string& text) {
  CardMeta m;
  size_t pos = 0;
  int lines = 0;
  while (pos < text.size() && lines < 60) {
    size_t e = text.find('\n', pos);
    std::string_view ln(text.data() + pos, (e == std::string::npos ? text.size() : e) - pos);
    if (!ln.empty() && ln.back() == '\r') ln.remove_suffix(1);
    if (m.title.empty() && startsWith(ln, "# ")) m.title = trim(ln.substr(2));
    if (startsWith(ln, "id:")) m.id = trim(ln.substr(3));
    if (startsWith(ln, "aliases:")) {
      std::string v = trim(ln.substr(8));
      v = replaceAll(replaceAll(replaceAll(v, "[", ""), "]", ""), "\"", "");
      for (auto& s : split(v, ',')) {
        std::string t = trim(s);
        if (!t.empty()) m.aliases.push_back(t);
      }
    }
    if (e == std::string::npos) break;
    pos = e + 1;
    lines++;
  }
  return m;
}

// Папки «03_Персонажи» выше папки проекта и рабочей папки.
std::vector<std::string> cardDirs(App& a) {
  std::vector<std::string> roots, out;
  if (!a.projectPath().empty()) roots.push_back(fs::absolute(a.projectPath()));
  roots.push_back(fs::absolute("."));
  for (std::string r : roots) {
    for (int i = 0; i < 8 && !r.empty(); i++) {
      std::string d = fs::join(r, "03_Персонажи");
      if (fs::isDir(d) && std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
      std::string p = fs::parent(r);
      if (p == r) break;
      r = p;
    }
  }
  return out;
}

}  // namespace

// Найти карточку по ID (entity) или, если ID пуст, по имени (заголовок, имя файла, псевдонимы).
std::optional<std::string> findCanonCard(App& a, std::string_view entity, std::string_view name, std::string* foundId) {
  std::string want = utf8::searchKey(trim(entity));
  std::string nameKey = utf8::searchKey(trim(name));
  if (want.empty() && nameKey.empty()) return std::nullopt;
  for (const std::string& dir : cardDirs(a)) {
    for (const fs::DirEntry& e : fs::list(dir)) {
      if (e.dir || !endsWith(e.name, ".md") || startsWith(e.name, "00_")) continue;
      {
        auto text = fs::readFile(e.path);
        if (!text) continue;
        CardMeta m = readCardMeta(text->substr(0, std::min<size_t>(text->size(), 4096)));
        bool hit = false;
        if (!want.empty()) {
          hit = utf8::searchKey(m.id) == want;
        } else {
          hit = utf8::searchKey(m.title) == nameKey || utf8::searchKey(replaceAll(fs::stem(e.name), "_", " ")) == nameKey;
          for (auto& al : m.aliases) hit = hit || utf8::searchKey(al) == nameKey;
        }
        if (hit) {
          if (foundId) *foundId = m.id;
          return e.path;
        }
      }
    }
  }
  return std::nullopt;
}

}  // namespace rg::app::chars
