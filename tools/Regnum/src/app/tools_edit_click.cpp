// Regnum — инструменты одного щелчка: заливка грани (остров — в провинцию), объединение с соседней
// провинцией, удаление провинции (ТЗ 1.a.i «удаление»: земли становятся ничьими).
#include "app/tools_edit.h"

namespace rg::app::tools {

namespace {

// ---------------------------------------------------------------- заливка
class FillTool final : public MapTool {
 public:
  bool pointerDown(App& a, const PointerEvent& e) override {
    if (e.button != 0) return false;
    if (a.readOnly()) return true;
    const World& w = a.world();
    auto fs = geo::faces(w);
    int f = fs->locate(e.map);
    if (f < 0) return true;
    const geo::Face& face = fs->faces[size_t(f)];
    Id sel = selectedProvince(a);
    if ((e.mods & platform::ModShift) && sel) {
      if (face.province == sel) return true;
      // Область чужой провинции: если у той не осталось области, правило удаляет её запись.
      std::vector<std::string> removed;
      if (a.act("Добавить область к провинции", [&](Tx& tx) { removed = rules::fillAt(tx, e.map, sel).removed; }) && !removed.empty())
        a.toast("Удалена провинция без области: «" + join(removed, "», «") + "»", ToastKind::Warning, "trash");
      return true;
    }
    if (face.province) {
      // Занятая область: выбрать её провинцию (Shift+щелчок затем добавит соседние области к ней).
      a.select(SelType::Province, face.province);
      return true;
    }
    Id nid = 0;
    bool sea = face.terrain == Terrain::Sea;
    if (a.act(sea ? "Новая морская провинция" : "Новая провинция", [&](Tx& tx) {
          nid = rules::fillAt(tx, e.map, 0).province;
          std::string name = newProvinceName(tx.w());
          tx.province(nid).name = name;
          rules::addLog(tx, LogKind::Province, "Провинция «" + name + "» добавлена на карту", rules::LogRefs{nid, 0, {}});
        }))
      a.select(SelType::Province, nid);
    return true;
  }

  enum class Mode : u8 { None, New, Add, Pick, Same };
  Mode mode(App& a, const geo::Face** out) const {
    *out = nullptr;
    if (!a.ui.cursorMap) return Mode::None;
    const World& w = a.world();
    auto fs = geo::faces(w);
    int f = fs->locate(*a.ui.cursorMap);
    if (f < 0) return Mode::None;
    fsKeep_ = fs;
    *out = &fs->faces[size_t(f)];
    Id sel = selectedProvince(a);
    bool shift = (ui::mouse().mods & platform::ModShift) != 0;
    if (shift && sel) return (*out)->province == sel ? Mode::Same : Mode::Add;
    return (*out)->province ? Mode::Pick : Mode::New;
  }

  void drawOverlay(App& a, gfx::Canvas& c, const map::View& v) override {
    const Palette P = palette();
    const geo::Face* face = nullptr;
    Mode m = mode(a, &face);
    if (face && (m == Mode::New || m == Mode::Add) && face->box.intersects(v.visibleBox())) {
      Color col = m == Mode::Add ? P.success : P.accent;
      gfx::Path p = facePath(*face, v);
      c.fillPath(p, col.alpha(0.26f), gfx::FillRule::EvenOdd);
      gfx::Stroke s;
      s.join = gfx::Join::Round;
      s.width = 4;
      c.strokePath(p, s, P.ink.alpha(0.35f));
      s.width = 1.8f;
      s.dash = {7, 5};
      c.strokePath(p, s, col);
    }
    if (a.ui.cursorMap && (m == Mode::New || m == Mode::Add))
      cursorBadge(c, v.toScreen(*a.ui.cursorMap), m == Mode::Add ? "plus" : "tool-fill", m == Mode::Add ? P.success : P.accent);
    fsKeep_.reset();
    options(a);
  }

  void options(App& a) {
    const World& w = a.world();
    Id sel = selectedProvince(a);
    const char* tip = sel ? "Shift+щелчок — добавить к выбранной" : "Щелчок по ничьей земле — новая провинция";
    float width = (sel ? provinceTagW(w, sel) + 6 : 0) + textW(tip, ui::Font::Small);
    OptionsBar bar(a, "tool-fill", "Заливка", width);
    if (!bar) return;
    if (sel) provinceTag(w, sel);
    ui::label(tip, {.font = ui::Font::Small, .ink = ui::Ink::Muted});
  }

  platform::Cursor cursor(App& a) override {
    const geo::Face* face = nullptr;
    Mode m = mode(a, &face);
    fsKeep_.reset();
    return m == Mode::New || m == Mode::Add ? platform::Cursor::Crosshair : platform::Cursor::Arrow;
  }
  const char* hint(App& a) override {
    if (selectedProvince(a)) return "Щелчок по ничьей земле — провинция · Shift — к выбранной";
    return "Щелчок по ничьей области (острову) — новая провинция";
  }

 private:
  mutable std::shared_ptr<const geo::FaceSet> fsKeep_;
};

// ---------------------------------------------------------------- объединение
class MergeTool final : public MapTool {
 public:
  bool pointerDown(App& a, const PointerEvent& e) override {
    if (e.button != 0) return false;
    if (a.readOnly()) return true;
    const World& w = a.world();
    Id prov = provinceUnder(a, e), sel = selectedProvince(a);
    if (!prov) return true;
    if (!sel || prov == sel || !w.province(sel)) {
      a.select(SelType::Province, prov);
      return true;
    }
    const Province* t = w.province(sel);
    const Province* s = w.province(prov);
    if (t->sea != s->sea) {
      a.toast("Нельзя объединить сухопутную провинцию с морской", ToastKind::Warning, "warning");
      return true;
    }
    std::string tn = provinceTitle(w, sel), sn = provinceTitle(w, prov);
    const auto& nb = neighbours(a);
    bool adjacent = std::find(nb.begin(), nb.end(), prov) != nb.end();
    std::string text = "«" + sn + "» войдёт в «" + tn + "»: земли, население, гарнизон и постройки. Запись «" + sn + "» будет удалена.";
    if (!adjacent) text += " Провинции не граничат: земли «" + sn + "» станут отдельной частью «" + tn + "».";
    text += " Ctrl+Z вернёт всё как было.";
    a.confirm("Объединить провинции?", text, "Объединить", false, [sel, prov](App& x) {
                if (!x.world().province(sel) || !x.world().province(prov)) return;
                if (x.act("Объединить провинции", [&](Tx& tx) { rules::mergeProvinces(tx, sel, prov); })) x.select(SelType::Province, sel);
              });
    return true;
  }

  // Соседи выбранной провинции (кеш по граням).
  const std::vector<Id>& neighbours(App& a) {
    auto fs = geo::faces(a.world());
    Id sel = selectedProvince(a);
    if (fs != fs_ || sel != nbFor_) {
      fs_ = fs;
      nbFor_ = sel;
      nb_.clear();
      if (sel)
        for (auto [x, y] : fs->neighbors()) {
          if (x == sel) nb_.push_back(y);
          else if (y == sel) nb_.push_back(x);
        }
    }
    return nb_;
  }

  void drawOverlay(App& a, gfx::Canvas& c, const map::View& v) override {
    const Palette P = palette();
    const World& w = a.world();
    Id sel = selectedProvince(a);
    Id hov = a.ui.hover.type == SelType::Province ? a.ui.hover.id : 0;
    if (sel) {
      const Province* sp = w.province(sel);
      for (Id n : neighbours(a)) {
        const Province* np = w.province(n);
        if (!np || !sp || np->sea != sp->sea || n == hov) continue;
        highlightProvince(c, w, v, n, Color(0, 0, 0, 0), P.success.alpha(0.75f), 1.4f, true);
      }
      if (hov && hov != sel && sp) {
        const Province* hp = w.province(hov);
        bool ok = hp && hp->sea == sp->sea;
        Color col = ok ? P.success : P.danger;
        highlightProvince(c, w, v, hov, col.alpha(0.24f), col, 2.2f);
        if (a.ui.cursorMap) cursorBadge(c, v.toScreen(*a.ui.cursorMap), ok ? "merge" : "close", col);
      }
    }
    options(a);
  }

  void options(App& a) {
    const World& w = a.world();
    Id sel = selectedProvince(a);
    const char* tip = sel ? "Щелчок по соседней — объединить" : "Выберите провинцию";
    float width = (sel ? provinceTagW(w, sel) + 6 + 22 : 0) + textW(tip, ui::Font::Small);
    OptionsBar bar(a, "tool-merge", "Объединение", width);
    if (!bar) return;
    if (sel) {
      provinceTag(w, sel);
      ui::icon("plus", ui::Ink::Muted, 16);
    }
    ui::label(tip, {.font = ui::Font::Small, .ink = ui::Ink::Muted});
  }

  platform::Cursor cursor(App& a) override {
    return a.ui.hover.type == SelType::Province ? platform::Cursor::Hand : platform::Cursor::Arrow;
  }
  const char* hint(App& a) override {
    return selectedProvince(a) ? "Щелчок по соседней — присоединить к выбранной" : "Щелчок — выбрать провинцию, к которой присоединять";
  }

 private:
  std::shared_ptr<const geo::FaceSet> fs_;
  Id nbFor_ = 0;
  std::vector<Id> nb_;
};

// ---------------------------------------------------------------- удаление
class DeleteTool final : public MapTool {
 public:
  bool pointerDown(App& a, const PointerEvent& e) override {
    if (e.button != 0) return false;
    if (a.readOnly()) return true;
    Id prov = provinceUnder(a, e);
    if (!prov) return true;
    std::string name = provinceTitle(a.world(), prov);
    a.confirm("Удалить провинцию?", "«" + name + "» исчезнет с карты, её земли станут ничьими, сведения о ней будут удалены. Ctrl+Z вернёт.",
              "Удалить", true, [prov](App& x) {
                if (!x.world().province(prov)) return;
                bool wasSel = x.ui.sel == Selection{SelType::Province, prov};
                if (x.act("Удалить провинцию", [&](Tx& tx) { rules::deleteProvince(tx, prov); }) && wasSel) x.clearSelection();
              });
    return true;
  }
  void drawOverlay(App& a, gfx::Canvas& c, const map::View& v) override {
    const Palette P = palette();
    Id hov = a.ui.hover.type == SelType::Province ? a.ui.hover.id : 0;
    if (hov) {
      highlightProvince(c, a.world(), v, hov, P.danger.alpha(0.26f), P.danger, 2.2f);
      if (a.ui.cursorMap) cursorBadge(c, v.toScreen(*a.ui.cursorMap), "trash", P.danger);
    }
    const char* tip = "Щелчок по провинции — удалить";
    OptionsBar bar(a, "tool-delete-province", "Удаление", textW(tip, ui::Font::Small));
    if (bar) ui::label(tip, {.font = ui::Font::Small, .ink = ui::Ink::Muted});
  }
  platform::Cursor cursor(App& a) override {
    return a.ui.hover.type == SelType::Province ? platform::Cursor::Hand : platform::Cursor::Arrow;
  }
  const char* hint(App&) override { return "Щелчок по провинции — удалить её (земли станут ничьими)"; }
};

ToolDef fillDef() {
  return {ToolId::Fill, "tool-fill", "Заливка области", "U", true, [] { return std::make_unique<FillTool>(); }, 50};
}
ToolDef mergeDef() {
  return {ToolId::Merge, "tool-merge", "Объединить провинции", "J", true, [] { return std::make_unique<MergeTool>(); }, 70};
}
ToolDef deleteDef() {
  return {ToolId::DeleteProvince, "tool-delete-province", "Удалить провинцию", "D", true, [] { return std::make_unique<DeleteTool>(); }, 80};
}

ToolReg regFill(fillDef());
ToolReg regMerge(mergeDef());
ToolReg regDelete(deleteDef());

}  // namespace

void registerClickTools() {
  ToolReg a(fillDef());
  ToolReg b(mergeDef());
  ToolReg c(deleteDef());
}

}  // namespace rg::app::tools
