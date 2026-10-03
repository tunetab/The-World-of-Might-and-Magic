// Regnum — утилиты оболочки: сочетания клавиш, время, имена файлов, изображения, сведения о выделении.
#include <ctime>

#include "app/app_internal.h"
#include "base/fs.h"
#include "codec/png.h"
#include "geo/topo.h"

namespace rg::app::detail {

using platform::Key;

// ---------------------------------------------------------------- сочетания
static Key keyOf(std::string_view t) {
  std::string u = utf8::upper(t);
  if (u.size() == 1) {
    char ch = u[0];
    if (ch >= 'A' && ch <= 'Z') return Key(int(Key::A) + (ch - 'A'));
    if (ch >= '0' && ch <= '9') return Key(int(Key::D0) + (ch - '0'));
    switch (ch) {
      case '+':
      case '=': return Key::Equal;
      case '-': return Key::Minus;
      case ',': return Key::Comma;
      case '.': return Key::Period;
      case '/': return Key::Slash;
      case '\\': return Key::Backslash;
      case '[': return Key::LBracket;
      case ']': return Key::RBracket;
      case ';': return Key::Semicolon;
      case '\'': return Key::Quote;
      case '`': return Key::Grave;
      default: break;
    }
  }
  if (u.size() >= 2 && u[0] == 'F') {
    int n = 0;
    for (size_t i = 1; i < u.size(); i++) {
      if (u[i] < '0' || u[i] > '9') { n = -1; break; }
      n = n * 10 + (u[i] - '0');
    }
    if (n >= 1 && n <= 12) return Key(int(Key::F1) + n - 1);
  }
  static const std::pair<const char*, Key> names[] = {
      {"ESC", Key::Escape},       {"ESCAPE", Key::Escape},   {"ENTER", Key::Enter},     {"RETURN", Key::Enter},
      {"TAB", Key::Tab},          {"SPACE", Key::Space},     {"ПРОБЕЛ", Key::Space},    {"DEL", Key::Delete},
      {"DELETE", Key::Delete},    {"BACKSPACE", Key::Backspace}, {"INSERT", Key::Insert}, {"HOME", Key::Home},
      {"END", Key::End},          {"PAGEUP", Key::PageUp},   {"PAGEDOWN", Key::PageDown}, {"LEFT", Key::Left},
      {"RIGHT", Key::Right},      {"UP", Key::Up},           {"DOWN", Key::Down},       {"PLUS", Key::Equal},
      {"MINUS", Key::Minus},
  };
  for (auto& [n, k] : names)
    if (u == n) return k;
  return Key::Unknown;
}

ui::Shortcut parseShortcut(std::string_view s) {
  ui::Shortcut r;
  if (s.empty()) return r;
  std::vector<std::string> parts;
  std::string cur;
  for (size_t i = 0; i < s.size(); i++) {
    char ch = s[i];
    if (ch == '+' && !cur.empty()) {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  if (!cur.empty()) parts.push_back(cur);
  if (parts.empty()) return r;
  for (size_t i = 0; i + 1 < parts.size(); i++) {
    std::string m = utf8::upper(trim(parts[i]));
    if (m == "CTRL" || m == "CMD" || m == "⌘") r.mods |= ui::ModPrimary;
    else if (m == "SHIFT") r.mods |= platform::ModShift;
    else if (m == "ALT" || m == "OPTION") r.mods |= platform::ModAlt;
    else if (m == "SUPER" || m == "WIN") r.mods |= platform::ModSuper;
    else if (m == "CONTROL") r.mods |= platform::ModCtrl;
    else return ui::Shortcut{};
  }
  r.key = keyOf(trim(parts.back()));
  if (r.key == Key::Unknown) r.mods = 0;
  return r;
}

std::string shortcutLabel(std::string_view s) {
  ui::Shortcut sc = parseShortcut(s);
  return sc ? ui::shortcutText(sc) : std::string(s);
}

// ---------------------------------------------------------------- время
static i64 daysFromCivil(i64 y, unsigned m, unsigned d) {
  y -= m <= 2;
  const i64 era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = unsigned(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + i64(doe) - 719468;
}

i64 isoToMs(std::string_view s) {
  auto num = [&](size_t at, size_t n, int& out) {
    if (at + n > s.size()) return false;
    int v = 0;
    for (size_t i = at; i < at + n; i++) {
      if (s[i] < '0' || s[i] > '9') return false;
      v = v * 10 + (s[i] - '0');
    }
    out = v;
    return true;
  };
  int Y, M, D, h = 0, m = 0, sec = 0;
  if (!num(0, 4, Y) || s.size() < 10 || s[4] != '-' || !num(5, 2, M) || s[7] != '-' || !num(8, 2, D)) return 0;
  if (M < 1 || M > 12 || D < 1 || D > 31) return 0;
  if (s.size() >= 19 && (s[10] == 'T' || s[10] == ' ')) {
    if (!num(11, 2, h) || !num(14, 2, m) || !num(17, 2, sec)) return 0;
  }
  i64 days = daysFromCivil(Y, unsigned(M), unsigned(D));
  return ((days * 24 + h) * 60 + m) * 60000 + i64(sec) * 1000;
}

std::string localTime(std::string_view iso) {
  i64 ms = isoToMs(iso);
  if (ms <= 0) return {};
  std::time_t t = std::time_t(ms / 1000);
  std::time_t now = std::time(nullptr);
  std::tm a{}, b{};
  if (const std::tm* p = std::localtime(&t)) a = *p;
  else return {};
  if (const std::tm* p = std::localtime(&now)) b = *p;
  static const char* months[] = {"янв.", "февр.", "мар.", "апр.", "мая", "июн.", "июл.", "авг.", "сент.", "окт.", "нояб.", "дек."};
  std::string hm = strf("%02d:%02d", a.tm_hour, a.tm_min);
  if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) return hm;
  if (a.tm_year == b.tm_year && a.tm_yday + 1 == b.tm_yday) return "вчера, " + hm;
  std::string d = strf("%d %s", a.tm_mday, months[clamp(a.tm_mon, 0, 11)]);
  if (a.tm_year != b.tm_year) d += strf(" %d", a.tm_year + 1900);
  return d + ", " + hm;
}

// ---------------------------------------------------------------- имена и пути
std::string sanitizeName(std::string_view name) {
  std::string out;
  size_t i = 0;
  while (i < name.size()) {
    u32 cp = utf8::decode(name, i);
    if (cp < 32 || cp == '<' || cp == '>' || cp == ':' || cp == '"' || cp == '/' || cp == '\\' || cp == '|' || cp == '?' || cp == '*')
      cp = ' ';
    utf8::append(out, cp);
  }
  // Сжать пробелы, убрать точки и пробелы по краям (Windows их не любит).
  std::string r;
  bool sp = false;
  for (char ch : out) {
    if (ch == ' ') {
      sp = true;
      continue;
    }
    if (sp && !r.empty()) r.push_back(' ');
    sp = false;
    r.push_back(ch);
  }
  while (!r.empty() && (r.back() == '.' || r.back() == ' ')) r.pop_back();
  while (!r.empty() && r.front() == '.') r.erase(r.begin());
  static const char* reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "LPT1", "LPT2", "LPT3"};
  for (const char* rv : reserved)
    if (utf8::upper(r) == rv) r += "_";
  if (utf8::count(r) > 80) {
    size_t p = 0;
    for (int k = 0; k < 80; k++) p = utf8::next(r, p);
    r.resize(p);
  }
  return r.empty() ? std::string("Мир") : r;
}

std::optional<gfx::Image> loadImage(const std::string& path, int maxSide) {
  auto bytes = fs::readFile(path);
  if (!bytes) return std::nullopt;
  auto rgba = codec::decodePng(*bytes);
  if (!rgba) return std::nullopt;
  gfx::Image img = gfx::Image::fromRgba(rgba->rgba.data(), rgba->w, rgba->h);
  if (maxSide > 0 && std::max(img.w, img.h) > maxSide) {
    double k = double(maxSide) / std::max(img.w, img.h);
    img = img.scaled(std::max(1, int(std::lround(img.w * k))), std::max(1, int(std::lround(img.h * k))));
  }
  return img;
}

std::string findBasemapDir() {
  std::vector<std::string> bases;
  std::string exe = fs::exeDir();
  bases.push_back(fs::join(exe, "assets/basemap"));
  bases.push_back(fs::join(exe, "../assets/basemap"));
  bases.push_back(fs::join(exe, "../../assets/basemap"));
  bases.push_back(fs::join(exe, "../share/regnum/basemap"));
  bases.push_back(fs::join(exe, "../Resources/basemap"));
  std::string cwd = fs::absolute(".");
  bases.push_back(fs::join(cwd, "tools/Regnum/assets/basemap"));
  bases.push_back(fs::join(cwd, "assets/basemap"));
  for (auto& b : bases)
    if (fs::isFile(fs::join(b, "manifest.json"))) return fs::absolute(b);
  return {};
}

// ---------------------------------------------------------------- выделение
std::string entityName(const World& w, Selection s) {
  switch (s.type) {
    case SelType::Province: {
      const Province* p = w.province(s.id);
      if (!p) return {};
      return p->name.empty() ? std::string(p->sea ? "Морская провинция" : "Провинция без названия") : p->name;
    }
    case SelType::Faction: {
      const Faction* f = w.faction(s.id);
      if (!f) return {};
      return f->name.empty() ? std::string(f->isGuild() ? "Гильдия без названия" : "Государство без названия") : f->name;
    }
    case SelType::Army: {
      const Army* a = w.army(s.id);
      if (!a) return {};
      if (!a->name.empty()) return a->name;
      std::string who = w.factionName(a->leader());
      return std::string(a->isFleet() ? (a->allied() ? "Союзный флот" : "Флот") : (a->allied() ? "Союзное войско" : "Войско")) +
             (a->leader() ? " · " + who : std::string());
    }
    case SelType::Route: {
      const Route* r = w.route(s.id);
      if (!r) return {};
      return r->name.empty() ? std::string("Торговый маршрут") : r->name;
    }
    case SelType::Character: {
      const Character* c = w.character(s.id);
      if (!c) return {};
      return c->name.empty() ? std::string("Персонаж без имени") : c->name;
    }
    default: return {};
  }
}

const char* selIcon(SelType t) {
  switch (t) {
    case SelType::Province: return "province";
    case SelType::Faction: return "crown";
    case SelType::Army: return "army";
    case SelType::Route: return "route";
    case SelType::Character: return "character";
    default: return "info";
  }
}

const char* selCaption(SelType t) {
  switch (t) {
    case SelType::Province: return "Провинция";
    case SelType::Faction: return "Фракция";
    case SelType::Army: return "Войско";
    case SelType::Route: return "Торговый маршрут";
    case SelType::Character: return "Персонаж";
    default: return "";
  }
}

bool selectionExists(const World& w, Selection s) {
  switch (s.type) {
    case SelType::Province: return w.province(s.id) != nullptr;
    case SelType::Faction: return w.faction(s.id) != nullptr;
    case SelType::Army: return w.army(s.id) != nullptr;
    case SelType::Route: return w.route(s.id) != nullptr;
    case SelType::Character: return w.character(s.id) != nullptr;
    default: return false;
  }
}

Box2 selectionBox(const World& w, Selection s) {
  Box2 b;
  switch (s.type) {
    case SelType::Province: {
      auto fs = geo::faces(w);
      if (const geo::ProvinceShape* sh = fs->shape(s.id)) b = sh->box;
      break;
    }
    case SelType::Faction: {
      const Faction* f = w.faction(s.id);
      if (!f) break;
      auto fs = geo::faces(w);
      w.provinces.each([&](const Province& p) {
        bool mine = f->isState() ? p.owner == f->id : std::find(p.hqs.begin(), p.hqs.end(), f->id) != p.hqs.end();
        if (!mine) return;
        if (const geo::ProvinceShape* sh = fs->shape(p.id)) b.add(sh->box);
      });
      if (b.empty()) {
        w.armies.each([&](const Army& a) {
          for (auto& g : a.groups)
            if (g.faction == f->id) b.add(a.pos);
        });
      }
      break;
    }
    case SelType::Army: {
      if (const Army* a = w.army(s.id)) b = Box2{a->pos.x - 160, a->pos.y - 110, a->pos.x + 160, a->pos.y + 110};
      break;
    }
    case SelType::Route: {
      if (const Route* r = w.route(s.id))
        for (Vec2 p : r->pts) b.add(p);
      break;
    }
    case SelType::Character: {
      const Character* c = w.character(s.id);
      if (!c) break;
      Id army = 0;
      w.armies.each([&](const Army& a) {
        if (army) return;
        if (a.commander == c->id) army = a.id;
        for (auto& g : a.groups)
          if (std::find(g.heroes.begin(), g.heroes.end(), c->id) != g.heroes.end()) army = a.id;
      });
      if (army) return selectionBox(w, {SelType::Army, army});
      Id prov = 0;
      w.provinces.each([&](const Province& p) {
        if (!prov && p.lord == c->id) prov = p.id;
      });
      if (prov) return selectionBox(w, {SelType::Province, prov});
      if (c->faction) return selectionBox(w, {SelType::Faction, c->faction});
      break;
    }
    default: break;
  }
  return b;
}

}  // namespace rg::app::detail
