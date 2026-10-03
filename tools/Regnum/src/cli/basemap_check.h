// Regnum — проверочные изображения сборки базовой карты (regnum-cli build-basemap --check=<папка>).
#pragma once
#include <array>

#include "map/basemap_build.h"

namespace rg::cli::basemap {

struct Region {
  std::string name;
  int x = 0, y = 0, w = 0, h = 0;
};

// Области для проверки: заданные пользователем и подобранные автоматически
// (реки и озёра, сложный берег с островками, горы, замки и башни).
std::vector<Region> pickRegions(const map::bake::Artifacts& a, const std::vector<Region>& user);

// Обзоры (классы пикселей, тепловая карта ошибки, берег) и для каждой области — исходник, каждый слой на шахматке,
// композиция с заливкой провинции, ошибка, классы и берег в масштабе 2:1.
void writeChecks(const std::string& dir, const map::bake::Artifacts& a, const std::vector<Region>& regions);

// Проверка готовой папки: загрузка через map::Basemap, декодирование всех тайлов, сравнение уровня 0 с исходником
// и уменьшенных уровней с уменьшением уровня 0, нейтральность символов, берег. Печатает итог; false — расхождение.
bool verifyOutput(const std::string& dir, const map::bake::Artifacts& a, const map::bake::Report& rep);

}  // namespace rg::cli::basemap
