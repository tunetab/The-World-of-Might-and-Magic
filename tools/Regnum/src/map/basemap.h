// Regnum — базовая карта во время работы: манифест, тайлы слоёв, береговая линия, маска моря.
//
// Папка (assets/basemap, собирается regnum-cli build-basemap):
//   manifest.json            размеры, уровни, слои, список пустых тайлов, цвета
//   L{z}/{layer}_{x}_{y}.png тайлы RGBA (не premultiplied); крайние тайлы обрезаны по размеру уровня
//   coast.json               кольца суши {width, height, rings: [[x0, y0, x1, y1, ...], ...]}
//   mask.png                 маска моря (255 — море), пиксель маски = scale × scale пикселей карты
//   preview.png, thumb.png   уменьшенная карта и миниатюра для экрана запуска
// Отрисовки здесь нет: тайлы читает и кеширует отрисовщик карты. После load() объект не меняется, поэтому
// readTile, coast и isOcean можно вызывать из нескольких потоков (например, из задач jobs::submit).
#pragma once
#include <unordered_set>

#include "codec/png.h"
#include "geo/ops.h"

namespace rg::map {

struct BasemapLevel {
  int z = 0;         // уровень: 0 — полный размер, z — уменьшение в 2^z раз
  int w = 0, h = 0;  // размер уровня в пикселях
  int cols = 0, rows = 0;
};

struct TileRect { int x = 0, y = 0, w = 0, h = 0; };  // в пикселях уровня

class Basemap {
 public:
  // Прочитать манифест и маску; проверить наличие всех непустых тайлов. false и сообщение по-русски при ошибке.
  bool load(const std::string& dir, std::string* error = nullptr);
  bool loaded() const { return width_ > 0; }

  const std::string& dir() const { return dir_; }
  const std::string& id() const { return id_; }
  const std::string& sourceSha256() const { return sha_; }
  int width() const { return width_; }
  int height() const { return height_; }
  int tileSize() const { return tile_; }
  int levels() const { return int(levels_.size()); }
  const BasemapLevel& level(int z) const { return levels_.at(size_t(z)); }
  // Уровень для масштаба (пикселей экрана на пиксель карты): самый мелкий, ещё не уступающий в чёткости.
  int levelFor(double screenPerMapPixel) const;
  const std::vector<std::string>& layers() const { return layers_; }
  int layerIndex(std::string_view name) const;  // −1 — нет такого слоя
  Color oceanColor() const { return ocean_; }
  Color landColor() const { return land_; }

  // ---------------------------------------------------------------- тайлы
  bool inGrid(int z, int x, int y) const;
  TileRect tileRect(int z, int x, int y) const;                                   // пустой вне сетки
  std::string tilePath(std::string_view layer, int z, int x, int y) const;       // пусто для неизвестного слоя
  bool tileExists(std::string_view layer, int z, int x, int y) const;            // в сетке и не пустой
  // Декодировать тайл. Пустой тайл, вне сетки или ошибка чтения — nullopt и сообщение.
  std::optional<codec::RgbaImage> readTile(std::string_view layer, int z, int x, int y, std::string* error = nullptr) const;

  // ---------------------------------------------------------------- берег и маска
  geo::Coast coast() const;            // разбор coast.json; ошибка — UserError
  bool isOcean(Vec2 p) const;          // по маске; вне карты — false
  int maskWidth() const { return maskW_; }
  int maskHeight() const { return maskH_; }
  int maskScale() const { return maskScale_; }
  std::string previewPath() const { return file(preview_); }
  std::string thumbPath() const { return file(thumb_); }
  std::string coastPath() const { return file(coastFile_); }

 private:
  std::string file(const std::string& name) const;
  static u64 key(int layer, int z, int x, int y) {
    return (u64(u32(layer)) << 56) | (u64(u32(z) & 0xff) << 48) | (u64(u32(x) & 0xffffff) << 24) | u64(u32(y) & 0xffffff);
  }

  std::string dir_, id_, sha_, pattern_, coastFile_, maskFile_, preview_, thumb_;
  int width_ = 0, height_ = 0, tile_ = 0;
  std::vector<BasemapLevel> levels_;
  std::vector<std::string> layers_;
  std::unordered_set<u64> empty_;
  Color ocean_ = Color(0, 38, 255), land_ = Color(255, 255, 255);
  int maskW_ = 0, maskH_ = 0, maskScale_ = 1;
  std::vector<u8> mask_;  // 1 — море
};

}  // namespace rg::map
