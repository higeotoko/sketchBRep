// Headless scaling benchmark (performance plan step 0).
//
// Builds fixtures of growing size and times the core operations that a
// large model stresses: lookups, world placement, the snap index, a small
// edit (commit + undo), the AI's list_objects and saving. Doubling N shows
// how each cost grows (linear vs quadratic) independent of the machine.
//
//   sbr_bench [--sizes 250,500,1000] [--json out.json]
//
// Fixtures:
//   instances: one component definition placed N times (shared geometry)
//   unique:    N independent extruded boxes (no sharing)
// The numbers are measurements of this build on this machine, not targets.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Profile.h"
#include "sbr/snap/Inference.h"

using namespace sbr;
using nlohmann::json;

namespace {

double millis(const std::function<void()>& f) {
  const auto t0 = std::chrono::steady_clock::now();
  f();
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

doc::ObjectId box(doc::Modeler& m, double x, double y, double w, double d, double h) {
  geom::SketchPlane plane = geom::SketchPlane::ground();
  plane.origin = gp_Pnt(x, y, 0);
  auto id = m.createRectangle(plane, w, d);
  if (!id || !m.extrude(*id, h)) {
    std::fprintf(stderr, "fixture: could not build a box\n");
    std::exit(2);
  }
  return *id;
}

// N placements of one component on a grid (copied row by row).
void buildInstances(doc::Modeler& m, int n) {
  const doc::ObjectId window = box(m, 0, 0, 1200, 150, 1500);
  auto instance = m.makeComponent({window}, "Window");
  if (!instance) std::exit(2);
  const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
  const int rows = (n + columns - 1) / columns;
  gp_Trsf dx, dy;
  dx.SetTranslation(gp_Vec(1800, 0, 0));
  dy.SetTranslation(gp_Vec(0, 3000, 0));
  auto row = m.copy({*instance}, dx, columns - 1);
  if (!row) std::exit(2);
  std::vector<doc::ObjectId> firstRow{*instance};
  firstRow.insert(firstRow.end(), row->begin(), row->end());
  if (rows > 1 && !m.copy(firstRow, dy, rows - 1)) std::exit(2);
}

void buildUnique(doc::Modeler& m, int n) {
  const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
  for (int i = 0; i < n; ++i) {
    // Slightly different sizes: no two shapes are alike.
    box(m, (i % columns) * 1800.0, (i / columns) * 3000.0, 1000 + i % 7, 200 + i % 5, 2500 + i % 11);
  }
}

// Orthographic top view over the fixture (1 px = 20 mm).
snap::SnapQuery topQuery(double x, double y) {
  constexpr double s = 20.0;
  snap::SnapQuery q;
  q.ray = snap::Ray{gp_Pnt(x, y, 1e6), gp_Dir(0, 0, -1)};
  q.cursor = snap::ScreenPoint{x / s, -y / s};
  q.project = [](const gp_Pnt& p) { return snap::ScreenPoint{p.X() / s, -p.Y() / s}; };
  q.pixelTolerance = 10;
  return q;
}

json measure(const std::string& fixture, int n) {
  json r = {{"fixture", fixture}, {"n", n}};
  doc::Modeler m;
  r["build_ms"] = millis([&] { fixture == "instances" ? buildInstances(m, n) : buildUnique(m, n); });
  const doc::Document& d = m.document();
  r["objects"] = d.objects().size();

  std::vector<doc::ObjectId> ids;
  for (const doc::Object& o : d.objects()) ids.push_back(o.id);
  r["find_all_ms"] = millis([&] {
    std::size_t hits = 0;
    for (const doc::ObjectId& id : ids) hits += d.find(id) != nullptr;
    if (hits != ids.size()) std::exit(3);
  });

  std::vector<TopoDS_Shape> world;
  r["world_shapes_ms"] = millis([&] {
    for (const doc::Object& o : d.objects()) {
      if (!o.shape.IsNull() && d.isVisible(o.id)) world.push_back(d.worldShape(o));
    }
  });

  snap::InferenceEngine engine;
  r["snap_index_ms"] = millis([&] { engine.setShapes(world); });
  constexpr int kQueries = 50;
  r["snap_query_ms"] = millis([&] {
                         for (int i = 0; i < kQueries; ++i) engine.snap(topQuery(37.0 * i, 61.0 * i));
                       }) /
                       kQueries;

  // The viewport's path: shapes indexed by key, one changed, then a query
  // (includes the lazy rebuild of the bounding volume hierarchy).
  snap::InferenceEngine keyed;
  for (std::size_t i = 0; i < world.size(); ++i) keyed.setShape(std::to_string(i), world[i]);
  keyed.snap(topQuery(0, 0));
  r["edit_reindex_query_ms"] = millis([&] {
    keyed.setShape("0", world.back());
    keyed.snap(topQuery(100, 100));
  });

  // A small edit on one object: commit, then undo.
  const doc::ObjectId target = ids.back();
  r["move_one_ms"] = millis([&] { m.move({target}, gp_Vec(10, 0, 0)); });
  r["undo_ms"] = millis([&] { m.undo(); });

  api::CommandApi api(m);
  std::size_t bytes = 0;
  r["list_objects_ms"] = millis([&] { bytes = api.execute(R"({"command": "list_objects"})").size(); });
  r["list_objects_bytes"] = bytes;

  r["query_ids_ms"] = millis([&] {
    api.execute(R"({"command": "query_objects", "args": {"fields": "id", "limit": 5000}})");
  });
  r["aggregate_ms"] = millis([&] { api.execute(R"({"command": "aggregate", "args": {"group_by": "kind"}})"); });

  const std::string path = (std::filesystem::temp_directory_path() / "sbr_bench.sbr").string();
  r["save_ms"] = millis([&] {
    if (!m.save(path)) std::exit(4);
  });
  r["file_bytes"] = std::filesystem::file_size(path);
  std::filesystem::remove(path);
  return r;
}

std::vector<int> parseSizes(const std::string& text) {
  std::vector<int> sizes;
  std::stringstream in(text);
  std::string item;
  while (std::getline(in, item, ',')) sizes.push_back(std::stoi(item));
  return sizes;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<int> sizes = {250, 500, 1000};
  std::string jsonPath;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--sizes" && i + 1 < argc) {
      sizes = parseSizes(argv[++i]);
    } else if (a == "--json" && i + 1 < argc) {
      jsonPath = argv[++i];
    } else {
      std::fprintf(stderr, "usage: sbr_bench [--sizes 250,500,1000] [--json out.json]\n");
      return 1;
    }
  }
  const char* columns[] = {"build_ms",     "find_all_ms", "world_shapes_ms",  "snap_index_ms",
                           "snap_query_ms", "edit_reindex_query_ms", "move_one_ms", "undo_ms",          "list_objects_ms", "query_ids_ms", "aggregate_ms",
                           "save_ms"};
  json all = json::array();
  std::printf("%-10s %6s", "fixture", "n");
  for (const char* c : columns) std::printf(" %15s", c);
  std::printf(" %12s\n", "file_bytes");
  for (const char* fixture : {"instances", "unique"}) {
    for (int n : sizes) {
      const json r = measure(fixture, n);
      std::printf("%-10s %6d", fixture, n);
      for (const char* c : columns) std::printf(" %15.2f", r[c].get<double>());
      std::printf(" %12zu\n", r["file_bytes"].get<std::size_t>());
      std::fflush(stdout);
      all.push_back(r);
    }
  }
  if (!jsonPath.empty()) std::ofstream(jsonPath) << all.dump(2) << "\n";
  return 0;
}
