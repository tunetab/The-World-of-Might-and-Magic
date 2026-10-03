// Аудит (tz-build-mods-turn): безопасная загрузка JSON, исправленного вручную. Сохранённый демонстрационный мир;
// в каждом файле data/*.json и world.json значения в случайных местах заменяются значениями неверного типа или
// крайними числами; мир читается (io::load), считается (rules::calc), проводится пробный ход (rules::previewTurn).
// Допустимы только понятные отказы UserError; иные исключения — дефект. Строгая проверка — при REGNUM_AUDIT=1.
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "base/json.h"
#include "tests/test_app_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

bool auditStrict() {
  const char* v = std::getenv("REGNUM_AUDIT");
  return v && *v == '1';
}

struct Step {
  std::string key;
  size_t index = 0;
  bool isKey = false;
};
using Path = std::vector<Step>;

void collect(const json::Value& v, Path& cur, std::vector<Path>& out, size_t cap) {
  if (out.size() >= cap) return;
  out.push_back(cur);
  if (v.isObj()) {
    for (const auto& [k, m] : v.members()) {
      cur.push_back(Step{k, 0, true});
      collect(m, cur, out, cap);
      cur.pop_back();
      if (out.size() >= cap) return;
    }
  } else if (v.isArr()) {
    // Длинные массивы (координаты границ): только несколько первых элементов.
    size_t n = std::min<size_t>(v.size(), 6);
    for (size_t i = 0; i < n; i++) {
      cur.push_back(Step{{}, i, false});
      collect(v[int(i)], cur, out, cap);
      cur.pop_back();
      if (out.size() >= cap) return;
    }
  }
}

json::Value* walk(json::Value& root, const Path& p) {
  json::Value* v = &root;
  for (const Step& s : p) {
    if (s.isKey) {
      if (!v->isObj()) return nullptr;
      v = &(*v)[std::string_view(s.key)];
    } else {
      if (!v->isArr() || s.index >= v->size()) return nullptr;
      v = &v->items()[s.index];
    }
  }
  return v;
}

std::string pathText(const Path& p) {
  std::string s;
  for (const Step& x : p) s += x.isKey ? "." + x.key : "[" + std::to_string(x.index) + "]";
  return s.empty() ? "(корень)" : s;
}

json::Value replacement(int k) {
  switch (k % 9) {
    case 0: return json::Value(nullptr);
    case 1: return json::Value("строка");
    case 2: return json::Value(-1);
    case 3: return json::Value(1e300);
    case 4: return json::Value(true);
    case 5: return json::Value(json::Array{});
    case 6: return json::Value::object({});
    case 7: return json::Value(-1e300);
    default: return json::Value(2147483648.0);
  }
}

}  // namespace

TEST(audit_tbmt_json_type_fuzz) {
  Harness h("audit_tbmt_json_fuzz");
  h.demo();
  // Немного состояния, чтобы файлы были содержательными: стройка, исследование, сделка-ход.
  CHECK(h->endTurnNow());
  h->toasts().clear();
  std::string dir = fs::join(h.root, "Мир");
  CHECK(h->saveTo(dir));
  const char* files[] = {"world.json",          "data/catalogs.json", "data/provinces.json", "data/factions.json",  "data/characters.json",
                         "data/relations.json", "data/modifiers.json", "data/buildings.json", "data/techs.json",     "data/armies.json",
                         "data/routes.json",    "data/deals.json",     "data/log.json",       "data/geo.json"};
  const char* env = std::getenv("REGNUM_JSON_FUZZ");
  const int perFile = env && *env ? std::atoi(env) : 24;
  Rng rng(0xA0D17);
  int runs = 0, defects = 0, userErrors = 0;
  std::string report;
  auto t0 = std::chrono::steady_clock::now();
  for (const char* rel : files) {
    const std::string full = fs::join(dir, rel);
    const std::string orig = fs::readFile(full).value_or("");
    CHECK(!orig.empty());
    auto parsed = json::tryParse(orig);
    CHECK(parsed.has_value());
    if (!parsed) continue;
    std::vector<Path> paths;
    Path cur;
    collect(*parsed, cur, paths, 20000);
    for (int i = 0; i < perFile && !paths.empty(); i++) {
      const Path& p = paths[size_t(rng.next() % paths.size())];
      if (p.empty()) continue;
      json::Value root = *parsed;
      json::Value* v = walk(root, p);
      if (!v) continue;
      const int k = int(rng.next() % 9);
      *v = replacement(k);
      CHECK(fs::writeFileAtomic(full, json::write(root)));
      runs++;
      std::string what = std::string(rel) + pathText(p) + " = " + json::write(replacement(k));
      try {
        io::LoadResult r = io::load(dir);
        auto c = rules::calc(r.world);
        (void)c;
        rules::TurnReport rep = rules::previewTurn(r.world);
        (void)rep;
        // Повторная нормализация исправленного мира ничего не меняет (иначе исправление нестабильно).
        World w2 = r.world;
        io::Warnings again;
        io::normalize(w2, again);
        if (!again.empty()) {
          defects++;
          report += "  нестабильная нормализация: " + what + " → " + again.front().text() + "\n";
        }
      } catch (const UserError& e) {
        userErrors++;
      } catch (const std::exception& e) {
        defects++;
        report += "  исключение: " + what + " → " + e.what() + "\n";
      }
    }
    CHECK(fs::writeFileAtomic(full, orig));
  }
  double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::fprintf(stderr, "AUDIT INFO: JSON-фазз: %d правок, отказов UserError %d, дефектов %d, %.1f с\n%s", runs, userErrors, defects, sec, report.c_str());
  if (defects && auditStrict()) test::fail(__FILE__, __LINE__, "AUDIT: небезопасная загрузка ручного JSON:\n" + report);
}
