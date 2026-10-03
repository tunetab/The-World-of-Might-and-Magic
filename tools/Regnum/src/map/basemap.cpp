// Regnum — базовая карта во время работы (см. basemap.h).
#include "map/basemap.h"

#include <unordered_set>

#include "base/fs.h"
#include "base/json.h"

namespace rg::map {

namespace {

bool setErr(std::string* error, const std::string& msg) {
  if (error) *error = msg;
  return false;
}

std::string substitute(std::string s, std::string_view key, const std::string& value) { return replaceAll(std::move(s), key, value); }

bool positiveInt(const json::Value& v, int& out, int maxV = 1 << 24) {
  if (!v.isNum()) return false;
  const double d = v.asNum();
  if (!(d >= 1 && d <= maxV) || d != std::floor(d)) return false;
  out = int(d);
  return true;
}

}  // namespace

std::string Basemap::file(const std::string& name) const { return name.empty() ? std::string() : fs::join(dir_, name); }

bool Basemap::load(const std::string& dirIn, std::string* error) {
  *this = Basemap();
  const std::string dir = fs::absolute(dirIn);
  std::string err;
  auto text = fs::readFile(fs::join(dir, "manifest.json"), &err);
  if (!text) return setErr(error, "Базовая карта не найдена: нет manifest.json в «" + dir + "».");
  auto mv = json::tryParse(*text, &err);
  if (!mv || !mv->isObj()) return setErr(error, "Манифест базовой карты повреждён: " + err);
  const json::Value& m = *mv;

  Basemap b;
  b.dir_ = dir;
  b.id_ = m.str("id");
  b.sha_ = m.str("sourceSha256");
  b.pattern_ = m.str("tiles", "L{z}/{layer}_{x}_{y}.png");
  if (b.id_.empty()) return setErr(error, "В манифесте базовой карты нет идентификатора.");
  if (b.pattern_.find("{layer}") == std::string::npos || b.pattern_.find("{x}") == std::string::npos ||
      b.pattern_.find("{y}") == std::string::npos || b.pattern_.find("{z}") == std::string::npos || b.pattern_.find("..") != std::string::npos)
    return setErr(error, "Неверный шаблон имён тайлов в манифесте.");
  if (!positiveInt(m.get("width"), b.width_) || !positiveInt(m.get("height"), b.height_) || !positiveInt(m.get("tile"), b.tile_, 1 << 16))
    return setErr(error, "В манифесте базовой карты неверные размеры.");

  // Уровни: размеры должны соответствовать уменьшению в 2^z раз и сетке тайлов.
  for (const json::Value& lv : m.arr("levels")) {
    BasemapLevel l;
    const double z = lv.num("z", -1);
    if (!(z >= 0 && z < 16) || z != std::floor(z)) return setErr(error, "Неверный уровень в манифесте базовой карты.");
    l.z = int(z);
    if (!positiveInt(lv.get("w"), l.w) || !positiveInt(lv.get("h"), l.h) || !positiveInt(lv.get("cols"), l.cols) ||
        !positiveInt(lv.get("rows"), l.rows))
      return setErr(error, "Неверный уровень в манифесте базовой карты.");
    const int f = 1 << l.z;
    if (l.z != int(b.levels_.size()) || l.w != (b.width_ + f - 1) / f || l.h != (b.height_ + f - 1) / f ||
        l.cols != (l.w + b.tile_ - 1) / b.tile_ || l.rows != (l.h + b.tile_ - 1) / b.tile_)
      return setErr(error, strf("Уровень %d базовой карты не согласован с размерами карты.", l.z));
    b.levels_.push_back(l);
  }
  if (b.levels_.empty()) return setErr(error, "В манифесте базовой карты нет уровней.");

  for (const json::Value& s : m.arr("layers")) {
    if (!s.isStr() || s.asStr().empty() || s.asStr().find_first_of("/\\.") != std::string::npos)
      return setErr(error, "Неверное имя слоя в манифесте базовой карты.");
    b.layers_.push_back(s.asStr());
  }
  if (b.layers_.empty()) return setErr(error, "В манифесте базовой карты нет слоёв.");

  for (const auto& [name, list] : m.obj("empty").members()) {
    const int li = b.layerIndex(name);
    if (li < 0) return setErr(error, "Список пустых тайлов ссылается на неизвестный слой «" + name + "».");
    for (const json::Value& t : list.items()) {
      const int z = int(t[0].asInt(-1)), x = int(t[1].asInt(-1)), y = int(t[2].asInt(-1));
      if (t.size() != 3 || !b.inGrid(z, x, y)) return setErr(error, "Неверный пустой тайл в манифесте базовой карты.");
      b.empty_.insert(key(li, z, x, y));
    }
  }

  const json::Value& colors = m.obj("colors");
  if (auto c = Color::parse(colors.str("ocean", "#0026ff"))) b.ocean_ = *c;
  if (auto c = Color::parse(colors.str("land", "#ffffff"))) b.land_ = *c;

  b.coastFile_ = m.obj("coast").str("file", "coast.json");
  b.preview_ = m.obj("preview").str("file");
  b.thumb_ = m.obj("thumb").str("file");
  for (const std::string* f : {&b.coastFile_, &b.preview_, &b.thumb_})
    if (f->find("..") != std::string::npos) return setErr(error, "Неверное имя файла в манифесте базовой карты.");

  // Маска моря.
  const json::Value& mk = m.obj("mask");
  b.maskFile_ = mk.str("file", "mask.png");
  if (b.maskFile_.find("..") != std::string::npos) return setErr(error, "Неверное имя файла маски.");
  if (!positiveInt(mk.get("scale"), b.maskScale_, 4096)) return setErr(error, "Неверный масштаб маски моря.");
  auto mbytes = fs::readFile(b.file(b.maskFile_), &err);
  if (!mbytes) return setErr(error, "Нет маски моря базовой карты: " + err);
  auto mimg = codec::decodePng(*mbytes, &err);
  if (!mimg) return setErr(error, "Маска моря повреждена: " + err);
  if (mimg->w != (b.width_ + b.maskScale_ - 1) / b.maskScale_ || mimg->h != (b.height_ + b.maskScale_ - 1) / b.maskScale_)
    return setErr(error, "Размер маски моря не соответствует карте.");
  b.maskW_ = mimg->w;
  b.maskH_ = mimg->h;
  b.mask_.resize(size_t(b.maskW_) * size_t(b.maskH_));
  for (size_t i = 0; i < b.mask_.size(); i++) b.mask_[i] = mimg->rgba[i * 4] >= 128 ? 1 : 0;

  // Все непустые тайлы должны быть на месте.
  std::vector<std::string> files = fs::listTree(dir);
  std::unordered_set<std::string> present(files.begin(), files.end());
  for (const BasemapLevel& l : b.levels_)
    for (size_t li = 0; li < b.layers_.size(); li++)
      for (int y = 0; y < l.rows; y++)
        for (int x = 0; x < l.cols; x++) {
          if (b.empty_.count(key(int(li), l.z, x, y))) continue;
          std::string rel = b.pattern_;
          rel = substitute(rel, "{z}", std::to_string(l.z));
          rel = substitute(rel, "{layer}", b.layers_[li]);
          rel = substitute(rel, "{x}", std::to_string(x));
          rel = substitute(rel, "{y}", std::to_string(y));
          if (!present.count(rel)) return setErr(error, "Базовая карта неполная: нет тайла " + rel + ".");
        }
  if (!present.count(b.coastFile_)) return setErr(error, "Базовая карта неполная: нет " + b.coastFile_ + ".");

  *this = std::move(b);
  return true;
}

int Basemap::levelFor(double screenPerMapPixel) const {
  if (levels_.empty()) return 0;
  if (!(screenPerMapPixel > 0)) return int(levels_.size()) - 1;
  int z = 0;
  while (z + 1 < int(levels_.size()) && double(1 << (z + 1)) * screenPerMapPixel <= 1.0) z++;
  return z;
}

int Basemap::layerIndex(std::string_view name) const {
  for (size_t i = 0; i < layers_.size(); i++)
    if (layers_[i] == name) return int(i);
  return -1;
}

bool Basemap::inGrid(int z, int x, int y) const {
  if (z < 0 || z >= int(levels_.size())) return false;
  const BasemapLevel& l = levels_[size_t(z)];
  return x >= 0 && y >= 0 && x < l.cols && y < l.rows;
}

TileRect Basemap::tileRect(int z, int x, int y) const {
  if (!inGrid(z, x, y)) return {};
  const BasemapLevel& l = levels_[size_t(z)];
  TileRect r;
  r.x = x * tile_;
  r.y = y * tile_;
  r.w = std::min(tile_, l.w - r.x);
  r.h = std::min(tile_, l.h - r.y);
  return r;
}

std::string Basemap::tilePath(std::string_view layer, int z, int x, int y) const {
  if (layerIndex(layer) < 0 || !inGrid(z, x, y)) return {};
  std::string rel = pattern_;
  rel = substitute(rel, "{z}", std::to_string(z));
  rel = substitute(rel, "{layer}", std::string(layer));
  rel = substitute(rel, "{x}", std::to_string(x));
  rel = substitute(rel, "{y}", std::to_string(y));
  return fs::join(dir_, rel);
}

bool Basemap::tileExists(std::string_view layer, int z, int x, int y) const {
  const int li = layerIndex(layer);
  return li >= 0 && inGrid(z, x, y) && !empty_.count(key(li, z, x, y));
}

std::optional<codec::RgbaImage> Basemap::readTile(std::string_view layer, int z, int x, int y, std::string* error) const {
  if (!tileExists(layer, z, x, y)) {
    setErr(error, "Тайл базовой карты пуст или вне сетки.");
    return std::nullopt;
  }
  const std::string path = tilePath(layer, z, x, y);
  std::string err;
  auto bytes = fs::readFile(path, &err);
  if (!bytes) {
    setErr(error, err);
    return std::nullopt;
  }
  auto img = codec::decodePng(*bytes, &err);
  if (!img) {
    setErr(error, "Тайл " + path + " повреждён: " + err);
    return std::nullopt;
  }
  const TileRect r = tileRect(z, x, y);
  if (img->w != r.w || img->h != r.h) {
    setErr(error, "Размер тайла " + path + " не соответствует сетке.");
    return std::nullopt;
  }
  return img;
}

geo::Coast Basemap::coast() const {
  if (!loaded()) fail("Базовая карта не загружена.");
  std::string err;
  auto text = fs::readFile(coastPath(), &err);
  if (!text) fail("Не удалось прочитать береговую линию: " + err);
  auto v = json::tryParse(*text, &err);
  if (!v || !v->isObj()) fail("Файл береговой линии повреждён: " + err);
  geo::Coast c;
  c.width = v->num("width", 0);
  c.height = v->num("height", 0);
  if (c.width != width_ || c.height != height_) fail("Размеры береговой линии не совпадают с базовой картой.");
  const json::Array& rings = v->arr("rings");
  c.landRings.reserve(rings.size());
  for (const json::Value& r : rings) {
    const json::Array& a = r.items();
    if (!r.isArr() || a.size() < 6 || a.size() % 2) fail("Кольцо береговой линии повреждено.");
    std::vector<Vec2> ring;
    ring.reserve(a.size() / 2);
    for (size_t k = 0; k < a.size(); k += 2) {
      if (!a[k].isNum() || !a[k + 1].isNum()) fail("Кольцо береговой линии повреждено.");
      const Vec2 p(a[k].asNum(), a[k + 1].asNum());
      if (!(p.x >= 0 && p.x <= c.width && p.y >= 0 && p.y <= c.height)) fail("Точка береговой линии вне карты.");
      ring.push_back(p);
    }
    c.landRings.push_back(std::move(ring));
  }
  return c;
}

bool Basemap::isOcean(Vec2 p) const {
  if (mask_.empty() || !(p.x >= 0 && p.y >= 0 && p.x < width_ && p.y < height_)) return false;
  const int mx = std::min(maskW_ - 1, int(p.x / maskScale_)), my = std::min(maskH_ - 1, int(p.y / maskScale_));
  return mask_[size_t(my) * size_t(maskW_) + size_t(mx)] != 0;
}

}  // namespace rg::map
