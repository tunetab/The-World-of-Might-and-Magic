// Regnum — команды regnum-cli для мира: validate, inspect, convert, new.
#include <cstdio>
#include <map>

#include "base/fs.h"
#include "base/json.h"
#include "cli/cli.h"
#include "core/io.h"
#include "core/schema.h"
#include "geo/ops.h"
#include "map/basemap.h"

namespace rg::cli {

namespace {

void out(const std::string& s) { std::fwrite(s.data(), 1, s.size(), stdout); }
void outLine(const std::string& s) { out(s + "\n"); }
void printJson(const json::Value& v) { outLine(json::write(v, json::WriteOptions{2, false, true})); }

int usageError(const char* usage) {
  std::fprintf(stderr, "Использование: regnum-cli %s\n", usage);
  return 2;
}

std::string ref(Seq s, Id id) {
  if (!id) return {};
  return std::string(schema::idPrefix(s)) + std::to_string(id);
}
json::Value refV(Seq s, Id id) { return id ? json::Value(ref(s, id)) : json::Value(); }

std::string money(double v) { return fmtNum(v, 2); }

// Точек геометрии: узлы + промежуточные точки дуг.
size_t geoPoints(const World& w) {
  size_t n = w.nodes.size();
  w.edges.each([&](const Edge& e) { n += e.pts.size(); });
  return n;
}

// Файлы мира по состоянию: rewrite — таблицы с исправлениями (переписываются при первом сохранении);
// reformat — (только папка) данные те же, но текст не канонический: порядок ключей, отступы, округление
// координат; такой файл получит канонический вид при следующей записи своей таблицы.
struct FileReport {
  std::vector<std::string> rewrite, reformat;
};
FileReport fileReport(const io::LoadResult& r) {
  FileReport rep;
  const bool folder = !r.bundle && !r.files.folder.empty();
  for (const io::ProjectFile& f : io::projectFiles()) {
    if (f.tables & r.fixedTables) {
      rep.rewrite.push_back(f.path);
    } else if (folder) {
      auto text = fs::readFile(fs::join(r.files.folder, f.path));
      if (text && *text != io::fileText(r.world, f.path)) rep.reformat.push_back(f.path);
    }
  }
  return rep;
}

// ================================================================ validate
int runValidate(const std::vector<std::string>& args) {
  static const char* usage = "validate <мир> [--json] [--strict]";
  const auto pos = positional(args);
  if (pos.size() != 1) return usageError(usage);
  const bool asJson = hasFlag(args, "--json"), strict = hasFlag(args, "--strict");
  const std::string path = pos[0];

  io::LoadResult r;
  try {
    r = io::load(path);
  } catch (const UserError& e) {
    if (asJson) {
      json::Value v = json::Value::object();
      v.set("ok", false);
      v.set("path", fs::absolute(path));
      json::Value err = json::Value::object({{"code", "load"}, {"msg", e.what()}});
      v.set("errors", json::Value::array({err}));
      v.set("warnings", json::Value::array());
      printJson(v);
    } else {
      outLine("Ошибка чтения: " + std::string(e.what()));
      outLine("Итог: мир не читается.");
    }
    return 1;
  }

  const std::vector<geo::Issue> issues = geo::validate(r.world);
  const FileReport files = fileReport(r);
  const size_t nErrors = issues.size();
  const size_t nWarnings = r.warnings.size();
  const bool ok = nErrors == 0 && (!strict || nWarnings == 0);

  if (asJson) {
    json::Value v = json::Value::object();
    v.set("ok", ok);
    v.set("path", fs::absolute(path));
    v.set("bundle", r.bundle);
    v.set("name", r.world.meta->name);
    v.set("turn", r.world.turn());
    json::Array errs;
    for (const geo::Issue& i : issues) {
      json::Value e = json::Value::object();
      e.set("code", i.code);
      e.set("msg", i.msg);
      e.set("at", json::Value::array({std::round(i.at.x * 100) / 100, std::round(i.at.y * 100) / 100}));
      errs.push_back(std::move(e));
    }
    v.set("errors", json::Value(std::move(errs)));
    json::Array warns;
    for (const io::Warning& w : r.warnings)
      warns.push_back(json::Value::object({{"file", w.file}, {"where", w.where}, {"msg", w.msg}}));
    v.set("warnings", json::Value(std::move(warns)));
    json::Array rw, rf;
    for (auto& f : files.rewrite) rw.emplace_back(f);
    for (auto& f : files.reformat) rf.emplace_back(f);
    v.set("rewrite", json::Value(std::move(rw)));
    v.set("reformat", json::Value(std::move(rf)));
    printJson(v);
    return ok ? 0 : 1;
  }

  outLine("Мир «" + r.world.meta->name + "», ход " + std::to_string(r.world.turn()) + (r.bundle ? " (архив)" : " (папка)"));
  if (r.warnings.empty()) {
    outLine("Данные: исправлений нет.");
  } else {
    outLine("Данные: исправлено при чтении — " + std::to_string(nWarnings) + ":");
    for (const io::Warning& w : r.warnings) outLine("  " + w.text());
  }
  if (issues.empty()) {
    outLine("Геометрия: ошибок нет (узлов " + fmtInt(i64(r.world.nodes.size())) + ", дуг " + fmtInt(i64(r.world.edges.size())) + ").");
  } else {
    outLine("Геометрия: ошибок — " + std::to_string(nErrors) + ":");
    for (const geo::Issue& i : issues)
      outLine("  [" + i.code + "] " + i.msg + strf(" (%.2f, %.2f)", i.at.x, i.at.y));
  }
  if (!files.rewrite.empty()) outLine("Будут переписаны при сохранении (исправления): " + join(files.rewrite, ", "));
  if (!files.reformat.empty())
    outLine("Не в каноническом виде (переформатируются при следующей записи таблицы): " + join(files.reformat, ", "));
  outLine("Итог: ошибок " + std::to_string(nErrors) + ", исправлений " + std::to_string(nWarnings) + (ok ? " — мир в порядке." : " — есть ошибки."));
  return ok ? 0 : 1;
}

// ================================================================ inspect
struct Summary {
  int states = 0, guilds = 0, sea = 0, unowned = 0, occupied = 0, fleets = 0, armies = 0, activeDeals = 0;
  std::map<Id, int> byOwner;      // государство -> провинций
  std::map<Id, int> hqs;          // гильдия -> штабов
  std::map<Id, int> armyCount;    // фракция-лидер -> объектов
};

Summary summarize(const World& w) {
  Summary s;
  w.factions.each([&](const Faction& f) { (f.isState() ? s.states : s.guilds)++; });
  w.provinces.each([&](const Province& p) {
    if (p.sea) s.sea++;
    else if (!p.owner) s.unowned++;
    if (p.owner) s.byOwner[p.owner]++;
    if (p.occupied) s.occupied++;
    for (Id h : p.hqs) s.hqs[h]++;
  });
  w.armies.each([&](const Army& a) {
    (a.isFleet() ? s.fleets : s.armies)++;
    for (const ArmyGroup& g : a.groups) s.armyCount[g.faction]++;
  });
  w.deals.each([&](const Deal& d) { if (d.status == DealStatus::Active) s.activeDeals++; });
  return s;
}

int runInspect(const std::vector<std::string>& args) {
  static const char* usage = "inspect <мир> [--json]";
  const auto pos = positional(args);
  if (pos.size() != 1) return usageError(usage);
  const bool asJson = hasFlag(args, "--json");
  const std::string path = pos[0];

  io::LoadResult r;
  std::vector<io::SnapshotInfo> snaps;
  try {
    r = io::load(path);
    snaps = io::listSnapshots(path);
  } catch (const UserError& e) {
    if (!asJson) throw;
    printJson(json::Value::object({{"ok", false}, {"path", fs::absolute(path)}, {"error", e.what()}}));
    return 1;
  }
  const World& w = r.world;
  const Meta& m = *w.meta;
  const Summary s = summarize(w);
  const size_t points = geoPoints(w);

  if (asJson) {
    json::Value v = json::Value::object();
    v.set("ok", true);
    v.set("path", fs::absolute(path));
    v.set("bundle", r.bundle);
    v.set("name", m.name);
    v.set("turn", m.turn);
    v.set("createdAt", m.createdAt);
    v.set("updatedAt", m.updatedAt);
    v.set("basemap", m.basemap);
    json::Value c = json::Value::object();
    c.set("provinces", w.provinces.size());
    c.set("seaProvinces", s.sea);
    c.set("unownedProvinces", s.unowned);
    c.set("occupiedProvinces", s.occupied);
    c.set("states", s.states);
    c.set("guilds", s.guilds);
    c.set("characters", w.characters.size());
    c.set("modifiers", w.modifiers.size());
    c.set("buildings", w.buildings.size());
    c.set("techs", w.techs.size());
    c.set("armies", s.armies);
    c.set("fleets", s.fleets);
    c.set("routes", w.routes.size());
    c.set("deals", w.deals.size());
    c.set("activeDeals", s.activeDeals);
    c.set("relations", w.relations->size());
    c.set("log", w.log.size());
    c.set("nodes", w.nodes.size());
    c.set("edges", w.edges.size());
    c.set("points", points);
    v.set("counts", std::move(c));
    json::Array facs;
    w.factions.each([&](const Faction& f) {
      json::Value x = json::Value::object();
      x.set("id", ref(Seq::Faction, f.id));
      x.set("kind", schema::kFactionKinds[int(f.kind)].id);
      x.set("name", f.name);
      x.set("treasury", f.treasury());
      if (f.isState()) {
        auto it = s.byOwner.find(f.id);
        x.set("provinces", it == s.byOwner.end() ? 0 : it->second);
        x.set("capital", refV(Seq::Province, f.capital));
        x.set("ruler", refV(Seq::Character, f.ruler));
      } else {
        auto it = s.hqs.find(f.id);
        x.set("hqs", it == s.hqs.end() ? 0 : it->second);
        x.set("homeState", refV(Seq::Faction, f.homeState));
      }
      auto ac = s.armyCount.find(f.id);
      x.set("armies", ac == s.armyCount.end() ? 0 : ac->second);
      facs.push_back(std::move(x));
    });
    v.set("factions", json::Value(std::move(facs)));
    json::Value owners = json::Value::object();
    for (auto& [id, n] : s.byOwner) owners.set(ref(Seq::Faction, id), n);
    v.set("provincesByOwner", std::move(owners));
    json::Array hist;
    for (auto& sn : snaps)
      hist.push_back(json::Value::object({{"turn", sn.turn}, {"file", sn.file}, {"at", sn.at}, {"label", sn.label}}));
    v.set("history", json::Value(std::move(hist)));
    v.set("warnings", r.warnings.size());
    printJson(v);
    return 0;
  }

  outLine("Мир «" + m.name + "» — ход " + std::to_string(m.turn) + (r.bundle ? " (архив)" : " (папка)"));
  outLine("Путь: " + fs::absolute(path));
  if (!m.createdAt.empty() || !m.updatedAt.empty()) outLine("Создан: " + m.createdAt + ", изменён: " + m.updatedAt);
  outLine("Базовая карта: " + m.basemap);
  outLine("Провинции: " + fmtInt(w.provinces.size()) + " (морских " + fmtInt(s.sea) + ", без владельца " + fmtInt(s.unowned) +
          ", оккупировано " + fmtInt(s.occupied) + ")");
  outLine("Геометрия: узлов " + fmtInt(i64(w.nodes.size())) + ", дуг " + fmtInt(i64(w.edges.size())) + ", точек " + fmtInt(i64(points)));
  outLine("Государства: " + fmtInt(s.states) + ", гильдии: " + fmtInt(s.guilds) + ", персонажи: " + fmtInt(w.characters.size()));
  outLine("Модификаторы: " + fmtInt(w.modifiers.size()) + ", постройки: " + fmtInt(w.buildings.size()) + ", технологии: " +
          fmtInt(w.techs.size()));
  outLine("Армии: " + fmtInt(s.armies) + ", флоты: " + fmtInt(s.fleets) + ", маршруты: " + fmtInt(w.routes.size()) + ", сделки: " +
          fmtInt(w.deals.size()) + " (действующих " + fmtInt(s.activeDeals) + "), записей хроники: " + fmtInt(w.log.size()));
  if (s.states) {
    outLine("Государства (казна, провинций):");
    w.factions.each([&](const Faction& f) {
      if (!f.isState()) return;
      auto it = s.byOwner.find(f.id);
      outLine("  " + ref(Seq::Faction, f.id) + "  " + f.name + " — " + money(f.treasury()) + ", провинций " +
              fmtInt(it == s.byOwner.end() ? 0 : it->second));
    });
  }
  if (s.guilds) {
    outLine("Гильдии (казна, штабов):");
    w.factions.each([&](const Faction& f) {
      if (!f.isGuild()) return;
      auto it = s.hqs.find(f.id);
      std::string home = f.homeState ? ", государство " + ref(Seq::Faction, f.homeState) : std::string();
      outLine("  " + ref(Seq::Faction, f.id) + "  " + f.name + " — " + money(f.treasury()) + ", штабов " +
              fmtInt(it == s.hqs.end() ? 0 : it->second) + home);
    });
  }
  if (snaps.empty()) {
    outLine("История ходов: снимков нет.");
  } else {
    std::vector<std::string> t;
    for (auto& sn : snaps) t.push_back(std::to_string(sn.turn));
    outLine("История ходов: " + join(t, ", "));
  }
  if (!r.warnings.empty())
    outLine("Исправлений при чтении: " + std::to_string(r.warnings.size()) + " (подробно — regnum-cli validate).");
  return 0;
}

// ================================================================ convert
int runConvert(const std::vector<std::string>& args) {
  static const char* usage = "convert <откуда> <куда> [--force]";
  const auto pos = positional(args);
  if (pos.size() != 2) return usageError(usage);
  io::Warnings warns;
  const double t0 = nowSeconds();
  const bool force = hasFlag(args, "--force");
  try {
    io::convert(pos[0], pos[1], &warns, force);
  } catch (const UserError& e) {
    const std::string m = e.what();
    if (!force && (m.find("не пуста") != std::string::npos || m.find("уже существует") != std::string::npos))
      fail(m + " (замена — --force)");
    throw;
  }
  const size_t snaps = io::listSnapshots(pos[1]).size();
  for (const io::Warning& w : warns) outLine("  " + w.text());
  outLine(strf("Готово за %.0f мс: %s (%s, снимков ходов: %zu, исправлений при чтении: %zu)", (nowSeconds() - t0) * 1000,
               fs::absolute(pos[1]).c_str(), io::isBundlePath(pos[1]) ? "архив" : "папка", snaps, warns.size()));
  return 0;
}

// ================================================================ new
// Папка базовой карты по умолчанию: рядом с исполняемым файлом (bin/<ОС>/../../assets/basemap) или от текущей папки.
std::string defaultBasemap() {
  const std::string candidates[] = {
    fs::join(fs::exeDir(), "../../assets/basemap"),
    "assets/basemap",
    "tools/Regnum/assets/basemap",
  };
  for (const std::string& c : candidates)
    if (fs::isFile(fs::join(c, "manifest.json"))) return fs::absolute(c);
  return {};
}

int runNew(const std::vector<std::string>& args) {
  static const char* usage = "new <папка|файл.regnum> [--name=Название] [--basemap=<папка>] [--no-geometry] [--force]";
  const auto pos = positional(args);
  if (pos.size() != 1) return usageError(usage);
  const std::string dst = pos[0];
  const bool bundle = io::isBundlePath(dst), force = hasFlag(args, "--force");
  if (!force) {
    if (bundle && fs::exists(dst)) fail("Файл «" + dst + "» уже существует (замена — --force)");
    if (!bundle && fs::isDir(dst) && !fs::list(dst).empty()) fail("Папка «" + dst + "» не пуста (замена — --force)");
  }
  if (!bundle && fs::exists(dst) && !fs::isDir(dst)) fail("«" + dst + "» — файл, а не папка");

  std::string name = option(args, "--name");
  if (name.empty()) name = bundle ? fs::stem(dst) : fs::filename(fs::absolute(dst));
  World w = newWorld(trim(name));

  std::string note;
  if (!hasFlag(args, "--no-geometry")) {
    std::string dir = option(args, "--basemap");
    const bool explicitDir = !dir.empty();
    if (!explicitDir) dir = defaultBasemap();
    if (dir.empty()) {
      note = "Базовая карта не найдена — мир создан без геометрии. Соберите её: regnum-cli build-basemap \"assets/source/Expanded Map.png\" "
             "assets/basemap, затем повторите или укажите --basemap=<папка>.";
    } else {
      map::Basemap bm;
      std::string err;
      if (!bm.load(dir, &err)) {
        if (explicitDir) fail(err);
        note = "Базовая карта не читается (" + err + ") — мир создан без геометрии.";
      } else {
        const geo::Coast coast = bm.coast();
        Tx tx(w);
        geo::initFromCoast(tx, coast);
        tx.meta().basemap = bm.id();
        w = std::move(tx).finish();
        note = "Базовая карта: " + bm.id() + " (" + bm.dir() + "), колец берега: " + std::to_string(coast.landRings.size());
      }
    }
  }

  io::Warnings warns;
  io::normalize(w, warns);
  for (const io::Warning& x : warns) outLine("  " + x.text());
  if (bundle) {
    io::saveBundle(dst, w);
  } else {
    io::SaveOptions so;
    so.backups = false;
    io::save(dst, w, TB_ALL, nullptr, so);
  }
  if (!note.empty()) outLine(note);
  const auto issues = geo::validate(w);
  outLine("Создан мир «" + w.meta->name + "»: " + fs::absolute(dst));
  outLine("Геометрия: узлов " + fmtInt(i64(w.nodes.size())) + ", дуг " + fmtInt(i64(w.edges.size())) + ", точек " +
          fmtInt(i64(geoPoints(w))) + (issues.empty() ? "" : ", ошибок проверки: " + std::to_string(issues.size())));
  return issues.empty() ? 0 : 1;
}

const Command regValidate("validate", "<мир> [--json] [--strict]",
                          "Проверить мир (папку, world.json или .regnum): исправления данных при чтении и геометрию; код 1 — ошибки "
                          "(с --strict — и исправления)",
                          &runValidate);
const Command regInspect("inspect", "<мир> [--json]", "Сводка мира: число сущностей, казна фракций, провинции по владельцам, история ходов",
                         &runInspect);
const Command regConvert("convert", "<откуда> <куда> [--force]", "Папка мира <-> архив .regnum (по расширению назначения), с историей ходов",
                         &runConvert);
const Command regNew("new", "<папка|файл.regnum> [--name=...] [--basemap=<папка>] [--no-geometry] [--force]",
                     "Создать новый мир; геометрия — рамка и берег базовой карты (по умолчанию assets/basemap)", &runNew);

}  // namespace

}  // namespace rg::cli
