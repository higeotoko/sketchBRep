#pragma once
#include <array>
namespace sbr::app {
struct BuiltinMaterialData {
  const char* name;
  const char* resource;
  std::array<double, 3> color;
  double widthMm, heightMm;
  double opacity, metallic, roughness, reflectance;
  bool useTexture;
};
// Data only: connect to existing material creation and document embedding.
inline constexpr std::array<BuiltinMaterialData, 12> kBuiltinMaterialData{{
  {"木（明るい）", ":/materials/wood_light_albedo.jpg", {0.8, 0.64, 0.44}, 1000.0, 1000.0, 1, 0, 0.55, 0.5, true},
  {"木（濃い）", ":/materials/wood_dark_albedo.jpg", {0.48, 0.32, 0.2}, 1000.0, 1000.0, 1, 0, 0.48, 0.5, true},
  {"漆喰（白）", ":/materials/plaster_white_albedo.jpg", {0.95, 0.94, 0.9}, 1000.0, 1000.0, 1, 0, 0.9, 0.45, true},
  {"コンクリート", ":/materials/concrete_albedo.jpg", {0.66, 0.66, 0.64}, 2000.0, 2000.0, 1, 0, 0.88, 0.45, true},
  {"モルタル", ":/materials/mortar_albedo.jpg", {0.74, 0.72, 0.68}, 1000.0, 1000.0, 1, 0, 0.9, 0.45, true},
  {"レンガ", ":/materials/brick_albedo.jpg", {0.63, 0.32, 0.22}, 840.0, 650.0, 1, 0, 0.88, 0.45, true},
  {"タイル（グレー）", ":/materials/tile_gray_albedo.jpg", {0.52, 0.54, 0.56}, 1200.0, 1200.0, 1, 0, 0.65, 0.5, true},
  {"畳", ":/materials/tatami_albedo.jpg", {0.73, 0.73, 0.48}, 900.0, 900.0, 1, 0, 0.85, 0.45, true},
  {"ガラス", ":/materials/glass_albedo.jpg", {0.62, 0.8, 0.88}, 1000.0, 1000.0, 0.22, 0, 0.08, 0.5, false},
  {"金属（ステンレス）", ":/materials/stainless_albedo.jpg", {0.76, 0.77, 0.78}, 500.0, 500.0, 1, 1, 0.27, 0.5, true},
  {"黒（鉄）", ":/materials/black_iron_albedo.jpg", {0.18, 0.18, 0.19}, 500.0, 500.0, 1, 1, 0.7, 0.5, true},
  {"芝", ":/materials/grass_albedo.jpg", {0.4, 0.6, 0.28}, 1000.0, 1000.0, 1, 0, 0.92, 0.4, true},
}};
}

