// Regnum — шапка инспектора провинции (ТЗ 1.a.iii, 1.a.iv, 1.a.ix): название с переименованием на месте, владелец
// с флагом, метки «море» и «оккупирована», меню действий, галочка «Морская провинция»; команды провинции.
#include "app/widgets.h"
#include "gfx/text.h"

namespace rg::app::prov {
// province_common.cpp
std::string provinceTitle(const Province& p);
bool flagPill(std::string_view key, const Faction& f, std::string_view tip, float maxW);
bool hatchPill(std::string_view key, const Faction& occ, std::string_view tip, float maxW);
void showProvince(App& a, Id pid);
void askDelete(App& a, Id pid);
extern const char* const kDeleteNeedsEdit;
void startMerge(App& a, Id pid);

// Переименование на месте: общее состояние шапки (команда F2 и пункт меню его включают).
struct Rename {
  Id pid = 0;
  bool on = false;
  int frames = 0;
  std::string buf;
  const App* app = nullptr;
  u64 frame = 0;
};
Rename& renameState(App& a) {
  static Rename r;
  // Другое приложение (тесты) — без правки.
  if (r.app != &a || a.frameCount() < r.frame) r = Rename{};
  r.app = &a;
  r.frame = a.frameCount();
  return r;
}
void startRename(App& a, Id pid) {
  const Province* p = a.world().province(pid);
  if (!p || a.readOnly()) return;
  Rename& r = renameState(a);
  r = Rename{pid, true, 0, p->name, &a, a.frameCount()};
  a.requestRedraw();
}
}  // namespace rg::app::prov

namespace rg::app {
namespace {

using platform::Key;

void moreMenu(App& a, const Province& p) {
  if (!ui::beginMenu("more")) return;
  const World& wd = a.world();
  bool ro = a.readOnly();
  Id pid = p.id;
  ui::menuHeader(prov::provinceTitle(p));
  if (ui::menuItem("Показать на карте", {.icon = "target", .shortcut = {Key::F, 0}})) prov::showProvince(a, pid);
  if (ui::menuItem("Переименовать", {.icon = "edit", .shortcut = {Key::F2, 0}, .disabled = ro})) prov::startRename(a, pid);
  a.markUi("province.menu.rename");
  if (ui::menuItem("Копировать название", {.icon = "copy"})) {
    platform::setClipboardText(prov::provinceTitle(p));
    a.toast("Название скопировано", ToastKind::Success, "copy");
  }
  a.markUi("province.menu.copy");
  if (!p.sea && p.owner && wd.faction(p.owner))
    if (ui::menuItem("Открыть государство", {.icon = "crown"})) a.select(SelType::Faction, p.owner);
  ui::menuSeparator();
  if (ui::menuItem("Объединить с соседней…", {.icon = "tool-merge", .disabled = ro})) prov::startMerge(a, pid);
  a.markUi("province.menu.merge");
  // ТЗ 1.a.ii: вне режима правки границы и области закреплены — удалить провинцию можно только в нём.
  if (ui::menuItem("Удалить провинцию", {.icon = "trash", .shortcut = {Key::Delete, 0}, .danger = true, .disabled = ro || !a.ui.editBorders}))
    prov::askDelete(a, pid);
  if (!ro && !a.ui.editBorders) ui::tooltip(prov::kDeleteNeedsEdit);
  a.markUi("province.menu.delete");
  ui::endMenu();
}

void drawName(App& a, const Province& p) {
  bool ro = a.readOnly();
  prov::Rename& rs = prov::renameState(a);
  if (rs.on && (rs.pid != p.id || ro)) rs = prov::Rename{};
  if (rs.on) {
    ui::WidgetId fid = ui::id("##rename");
    if (rs.frames == 0) ui::setKeyboardFocus(fid);
    bool changed = ui::textField("##rename", rs.buf, {.placeholder = "Название провинции", .icon = "edit", .maxLength = 80, .selectAllOnFocus = true});
    a.markUi("province.rename");
    bool focused = ui::lastItem().focused;
    if (changed) {
      std::string nm = trim(rs.buf);
      Id pid = p.id;
      rs = prov::Rename{};
      if (!nm.empty() && nm != p.name) a.act("Переименовать провинцию", [&](Tx& tx) { tx.province(pid).name = nm; });
    } else if (!focused && rs.frames > 0) {
      rs = prov::Rename{};
    } else {
      rs.frames++;
    }
    return;
  }
  std::string title = prov::provinceTitle(p);
  ui::HStack hs(38, ui::Align::Left, 4);
  float maxW = std::max(60.f, ui::avail().w - (ro ? 0 : 30));
  std::string shown = gfx::ellipsize(title, ui::textStyle(ui::Font::Display), maxW);
  ui::label(shown, {.font = ui::Font::Display, .ink = p.name.empty() ? ui::Ink::Muted : ui::Ink::Normal});
  RectF nr = ui::lastItem().rect;
  a.markUi("province.name");
  ui::Interaction ni = ui::interact(ui::id("##name"), nr);
  if (!ro && ni.doubleClicked) prov::startRename(a, p.id);
  if (!ro) {
    if (ui::iconButton("edit", "Переименовать", {.size = ui::Size::Small})) prov::startRename(a, p.id);
    ui::tooltip("Переименовать", {Key::F2, 0});
    a.markUi("province.renameBtn");
  }
}

void drawHeader(App& a, Id pid) {
  const World& wd = a.world();
  const Province* p = wd.province(pid);
  if (!p) return;
  bool ro = a.readOnly();
  const Faction* own = p->sea ? nullptr : wd.faction(p->owner);
  ui::IdScope ps{i64(pid)};
  // Подпись и меню действий
  {
    ui::Row r({ui::fr(1), ui::px(30)}, 30, 4);
    std::string cap = p->sea ? std::string("Морская провинция") : own ? "Провинция · " + own->name : std::string("Провинция · без владельца");
    ui::caption(cap);
    bool open = ui::iconButton("more-v", "Действия с провинцией");
    if (open) ui::openPopup("more");
    a.markUi("province.more");
    moreMenu(a, *p);
  }
  drawName(a, *p);
  // Владелец, оккупация, море
  {
    ui::HStack hs(26, ui::Align::Left, 6);
    float W = ui::avail().w;
    if (p->sea) {
      ui::tag("Море", ui::Tone::Info, "sea");
    } else {
      const Faction* occ = p->occupied ? wd.faction(p->occupier) : nullptr;
      float ownMax = occ ? W * 0.56f : W;
      if (own) {
        if (prov::flagPill("owner", *own, "Владелец — открыть государство", ownMax)) a.select(SelType::Faction, own->id);
        a.markUi("province.ownerPill");
      } else {
        ui::chip("Без владельца", {.icon = "flag", .tooltip = "Владелец не назначен — выберите во вкладке «Обзор»"});
      }
      if (occ) {
        if (prov::hatchPill("occupier", *occ, "Оккупирована: " + occ->name + " — открыть", W - ownMax - 6)) a.select(SelType::Faction, occ->id);
        a.markUi("province.occupierPill");
      }
    }
  }
  // ТЗ 1.a.iii: галочка «Морская провинция» — вся информация скрывается (данные сохраняются).
  {
    bool sea = p->sea;
    if (ui::checkbox("Морская провинция", sea, ro))
      // Правило: гарнизон → резерв, столица и оккупация снимаются, стройка приостанавливается (данные суши сохраняются).
      a.act(sea ? "Сделать провинцию морской" : "Сделать провинцию сухопутной", [&](Tx& tx) { rules::setProvinceSea(tx, pid, sea); });
    ui::tooltip("Морская провинция не содержит сведений и не закрашивается; данные суши сохраняются");
    a.markUi("province.sea");
  }
}

HeaderReg header({"province.header", SelType::Province, 0, drawHeader});

// ---------------------------------------------------------------- команды
bool provinceSelected(App& a) {
  return a.ui.screen == Screen::Editor && a.ui.editor.empty() && a.ui.sel.type == SelType::Province && a.world().province(a.ui.sel.id) != nullptr;
}
bool provinceEditable(App& a) { return provinceSelected(a) && !a.readOnly(); }
// Удаление меняет карту — только в режиме правки границ (ТЗ 1.a.ii).
bool provinceDeletable(App& a) { return provinceEditable(a) && a.ui.editBorders; }

CommandReg cDelete({"province.delete", "Удалить провинцию", "trash", "Delete", [](App& a) { prov::askDelete(a, a.ui.sel.id); }, provinceDeletable,
                    false, "Провинции"});
CommandReg cShow({"province.show", "Показать провинцию на карте", "target", nullptr, [](App& a) { prov::showProvince(a, a.ui.sel.id); },
                  provinceSelected, false, "Провинции"});
CommandReg cRename({"province.rename", "Переименовать провинцию", "edit", "F2", [](App& a) { prov::startRename(a, a.ui.sel.id); },
                    provinceEditable, false, "Провинции"});

}  // namespace
}  // namespace rg::app
