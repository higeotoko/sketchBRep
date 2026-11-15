// IFC export (docs/90 G-13, docs/101). The file is read back here by a small reader of its
// own: every reference must lead somewhere, the attribute counts of each entity must be those
// of the IFC4 schema, ids must be valid and stay the same, and the solids written must have
// the volume of the model's solids (which also proves the faces look outwards and close).
#include <cmath>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <nlohmann/json.hpp>
#include <set>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/IfcExport.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"

using json = nlohmann::json;
using sbr::api::CommandApi;

namespace {
struct Entity {
  std::string type;
  std::vector<std::string> args;  // top-level attributes, as written
};

// Splits "a,(b,c),'d,e'" at its top-level commas.
std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::string current;
  int depth = 0;
  bool quoted = false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '\'') quoted = !quoted;
    if (!quoted && c == '(') ++depth;
    if (!quoted && c == ')') --depth;
    if (!quoted && depth == 0 && c == ',') {
      out.push_back(current);
      current.clear();
      continue;
    }
    current += c;
  }
  if (!current.empty() || !s.empty()) out.push_back(current);
  return out;
}

std::vector<int> refsIn(const std::string& s) {
  std::vector<int> out;
  bool quoted = false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\'') quoted = !quoted;
    if (quoted || s[i] != '#') continue;
    int v = 0;
    std::size_t j = i + 1;
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) v = v * 10 + (s[j++] - '0');
    out.push_back(v);
  }
  return out;
}

std::string inner(const std::string& list) {  // "(a,b)" -> "a,b"
  return list.size() >= 2 && list.front() == '(' ? list.substr(1, list.size() - 2) : list;
}

struct Ifc {
  std::map<int, Entity> entities;
  std::string header;
  explicit Ifc(const std::string& text) {
    const auto data = text.find("DATA;");
    header = text.substr(0, data);
    std::size_t at = data;
    while ((at = text.find("\n#", at)) != std::string::npos) {
      const std::size_t eq = text.find('=', at), open = text.find('(', eq), end = text.find(";\n", open);
      const int id = std::stoi(text.substr(at + 2, eq - at - 2));
      entities[id] = {text.substr(eq + 1, open - eq - 1), split(text.substr(open + 1, end - open - 2))};
      at = end;
    }
  }
  std::vector<int> of(const std::string& type) const {
    std::vector<int> out;
    for (const auto& [id, e] : entities)
      if (e.type == type) out.push_back(id);
    return out;
  }
  const Entity& at(int id) const { return entities.at(id); }
  std::vector<double> point(int id) const {
    std::vector<double> out;
    for (const std::string& v : split(inner(at(id).args[0]))) out.push_back(std::stod(v));
    return out;
  }
  // The element whose Name is `name` (type given).
  int named(const std::string& type, const std::string& name) const {
    for (int id : of(type))
      if (at(id).args[2] == "'" + name + "'") return id;
    return 0;
  }
  // Volume enclosed by the element's faceted solids (mm3): outwards-looking closed shells give
  // a positive volume; holes in faces run the other way and take their part away.
  double volume(int element) const {
    double six = 0.0;
    for (int rep : refsIn(at(refsIn(at(element).args[6])[0]).args[2]))
      for (int item : refsIn(at(rep).args[3])) {
        if (at(item).type != "IFCFACETEDBREP") continue;
        for (int face : refsIn(at(refsIn(at(item).args[0])[0]).args[0]))
          for (int bound : refsIn(at(face).args[0])) {
            std::vector<std::vector<double>> p;
            for (int point : refsIn(at(refsIn(at(bound).args[0])[0]).args[0])) p.push_back(this->point(point));
            for (std::size_t i = 1; i + 1 < p.size(); ++i) {
              const auto &a = p[0], &b = p[i], &c = p[i + 1];
              six += a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
            }
          }
      }
    return six / 6;
  }
};

// IFC4: how many attributes each entity written here has.
const std::map<std::string, std::size_t> kAttributes = {
    {"IFCPROJECT", 9}, {"IFCSITE", 14}, {"IFCBUILDING", 12}, {"IFCBUILDINGSTOREY", 10}, {"IFCSPACE", 11},
    {"IFCWALL", 9}, {"IFCSLAB", 9}, {"IFCROOF", 9}, {"IFCSTAIR", 9}, {"IFCCOLUMN", 9}, {"IFCBEAM", 9}, {"IFCMEMBER", 9},
    {"IFCBUILDINGELEMENTPROXY", 9}, {"IFCOPENINGELEMENT", 9}, {"IFCWINDOW", 13}, {"IFCDOOR", 13}, {"IFCGRID", 11},
    {"IFCGRIDAXIS", 3}, {"IFCRELAGGREGATES", 6}, {"IFCRELCONTAINEDINSPATIALSTRUCTURE", 6}, {"IFCRELVOIDSELEMENT", 6},
    {"IFCRELFILLSELEMENT", 6}, {"IFCRELDEFINESBYPROPERTIES", 6}, {"IFCPROPERTYSET", 5}, {"IFCPROPERTYSINGLEVALUE", 4},
    {"IFCELEMENTQUANTITY", 6}, {"IFCQUANTITYLENGTH", 5}, {"IFCQUANTITYAREA", 5}, {"IFCQUANTITYVOLUME", 5},
    {"IFCSIUNIT", 4}, {"IFCUNITASSIGNMENT", 1}, {"IFCGEOMETRICREPRESENTATIONCONTEXT", 6},
    {"IFCGEOMETRICREPRESENTATIONSUBCONTEXT", 10}, {"IFCAXIS2PLACEMENT3D", 3}, {"IFCLOCALPLACEMENT", 2},
    {"IFCCARTESIANPOINT", 1}, {"IFCDIRECTION", 1}, {"IFCSHAPEREPRESENTATION", 4}, {"IFCPRODUCTDEFINITIONSHAPE", 3},
    {"IFCFACETEDBREP", 1}, {"IFCCLOSEDSHELL", 1}, {"IFCFACE", 1}, {"IFCFACEOUTERBOUND", 2}, {"IFCFACEBOUND", 2},
    {"IFCPOLYLOOP", 1}, {"IFCCOVERING", 9}, {"IFCRELCOVERSSPACES", 6}, {"IFCPOLYLINE", 1}, {"IFCARBITRARYCLOSEDPROFILEDEF", 3}, {"IFCEXTRUDEDAREASOLID", 4}};

void expectWellFormed(const Ifc& ifc) {
  std::set<std::string> ids;
  for (const auto& [id, e] : ifc.entities) {
    const auto known = kAttributes.find(e.type);
    ASSERT_NE(known, kAttributes.end()) << "an entity the schema table here does not know: " << e.type;
    EXPECT_EQ(e.args.size(), known->second) << "#" << id << " " << e.type;
    for (const std::string& a : e.args)
      for (int r : refsIn(a)) EXPECT_TRUE(ifc.entities.count(r)) << "#" << id << " refers to #" << r << ", which is not there";
    // Rooted entities: a GlobalId of 22 characters of the IFC alphabet, never twice.
    const bool rooted = e.type.rfind("IFCREL", 0) == 0 || e.args.size() >= 9 || e.type == "IFCPROPERTYSET" || e.type == "IFCELEMENTQUANTITY";
    if (rooted && e.type != "IFCGEOMETRICREPRESENTATIONSUBCONTEXT") {
      const std::string& g = e.args[0];
      ASSERT_EQ(g.size(), 24u) << "#" << id << " " << e.type << " " << g;
      EXPECT_TRUE(g[1] >= '0' && g[1] <= '3') << g;
      for (std::size_t i = 1; i <= 22; ++i)
        EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(g[i])) || g[i] == '_' || g[i] == '$') << g;
      EXPECT_TRUE(ids.insert(g).second) << "GlobalId used twice: " << g;
    }
  }
}

struct IfcExport : testing::Test {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};
  json ok(const std::string& command, json args = json::object()) {
    const json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  }
  double volume(const std::string& id) { return sbr::geom::volume(modeler.document().worldShape(*modeler.document().find(id))); }
  std::string name(const std::string& id) { return modeler.document().find(id)->name; }
};

TEST_F(IfcExport, AHouseIsWrittenWithItsStoreysPartsAndRelations) {
  ASSERT_TRUE(modeler.setLevels({{"", "1FL", 0}, {"", "2FL", 3000}}));
  const json walls = ok("create_wall", {{"points", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}, {0, 0}}},
                                        {"thickness", 200}, {"height", 3000}})["ids"];
  // A timber wall of a build-up, on its own: its type goes into the file.
  ok("create_wall", {{"points", {{0, -5000}, {4000, -5000}}}, {"type", "exterior_siding"}, {"height", 3000}});
  ok("place_openings", {{"host", walls[0]}, {"face_normal", {0, -1, 0}}, {"count", 2}, {"width", 1600}, {"height", 1200}, {"sill", 900}});
  ok("place_openings", {{"host", walls[1]}, {"face_normal", {1, 0, 0}}, {"count", 1}, {"width", 900}, {"height", 2100}, {"sill", 0}});
  json listed = ok("list_openings", {{"ids", {walls[0], walls[1]}}})["hosts"];
  ok("set_opening_fittings", {{"host", walls[0]}, {"openings", {listed[0]["openings"][0]["id"]}}, {"fitting", {{"type", "sliding_double"}}}});
  ok("set_opening_fittings", {{"host", walls[1]}, {"fitting", {{"type", "swing_single"}, {"hinge", "right"}, {"swing", "in"}}}});
  const std::string slab = ok("create_slab", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"top", 3000}, {"thickness", 200}})["id"];
  const std::string roof = ok("create_roof", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"kind", "hip"}, {"height", 3000}})["id"];
  const std::string stair = ok("create_stair", {{"path", {{1000, 1000}, {3000, 1000}, {3000, 2500}}}, {"rise", 3000}})["id"];
  const std::string column = ok("create_member", {{"start", {4000, 3000, 0}}, {"end", {4000, 3000, 2800}}, {"role", "column"},
                                                  {"section", {{"kind", "rectangle"}, {"width", 105}, {"depth", 105}}}})["id"];
  const std::string beam = ok("create_member", {{"start", {0, 3000, 2800}}, {"end", {8000, 3000, 2800}}, {"role", "beam"},
                                                {"section", {{"kind", "rectangle"}, {"width", 105}, {"depth", 240}}}})["id"];
  const std::string block = ok("create_box", {{"origin", {9000, 0, 0}}, {"width", 500}, {"depth", 500}, {"height", 500}})["id"];
  ok("create_line", {{"points", {{0, -2000, 0}, {8000, -2000, 0}}}});  // not a solid: left out
  const std::string roomId = ok("create_room", {{"at", {2000, 2000}}, {"name", "居間 LDK"}})["id"];
  // docs/117: a mark and a product on the door, finishes on the room.
  ok("set_opening_mark", {{"host", walls[1]}, {"mark", "SD-1"}, {"product", "P-900"}});
  ok("set_room_finish", {{"id", roomId}, {"use", "Living"}, {"floor", "Flooring t15"}, {"ceiling", "PB t9.5"}, {"ceiling_height", 2400}});
  ok("make_ceilings", {{"rooms", {roomId}}});  // docs/127: a ceiling of the room
  ok("create_grid", {{"x_spacings", {8000}}, {"y_spacings", {6000}}});

  sbr::doc::IfcSummary summary;
  const auto text = sbr::doc::ifcText(modeler, "house.ifc", &summary);
  ASSERT_TRUE(text) << text.error();
  EXPECT_EQ(text->rfind("ISO-10303-21;", 0), 0u);
  EXPECT_NE(text->find("FILE_SCHEMA(('IFC4'));"), std::string::npos);
  EXPECT_NE(text->find("END-ISO-10303-21;"), std::string::npos);
  const Ifc ifc(*text);
  expectWellFormed(ifc);

  EXPECT_EQ(summary.storeys, 2);
  EXPECT_EQ(summary.walls, 5);
  EXPECT_EQ(summary.openings, 3);
  EXPECT_EQ(summary.windows, 1);
  EXPECT_EQ(summary.doors, 1);
  EXPECT_EQ(summary.slabs, 1);
  EXPECT_EQ(summary.roofs, 1);
  EXPECT_EQ(summary.columns, 1);
  EXPECT_EQ(summary.beams, 1);
  EXPECT_EQ(summary.proxies, 1);
  EXPECT_EQ(summary.spaces, 1);
  EXPECT_EQ(summary.grids, 1);
  EXPECT_EQ(summary.coverings, 1);
  EXPECT_EQ(ifc.of("IFCWALL").size(), 5u);
  EXPECT_EQ(ifc.of("IFCOPENINGELEMENT").size(), 3u);
  EXPECT_EQ(ifc.of("IFCRELVOIDSELEMENT").size(), 3u);
  EXPECT_EQ(ifc.of("IFCRELFILLSELEMENT").size(), 2u);
  EXPECT_EQ(ifc.of("IFCBUILDINGSTOREY").size(), 2u);
  EXPECT_EQ(ifc.of("IFCPROJECT").size(), 1u);

  // The solids written are the model's solids: the same volume (faces outwards, shells closed,
  // holes taken out), measured from each storey's floor.
  for (const json& w : walls) {
    const int e = ifc.named("IFCWALL", name(w));
    ASSERT_NE(e, 0);
    EXPECT_NEAR(ifc.volume(e), volume(w), 1e-6 * volume(w)) << name(w);
  }
  EXPECT_NEAR(ifc.volume(ifc.of("IFCSLAB")[0]), volume(slab), 1.0);
  EXPECT_NEAR(ifc.volume(ifc.of("IFCROOF")[0]), volume(roof), 1e-6 * volume(roof));
  EXPECT_NEAR(ifc.volume(ifc.of("IFCCOLUMN")[0]), 105.0 * 105 * 2800, 1.0);
  EXPECT_NEAR(ifc.volume(ifc.of("IFCBEAM")[0]), 105.0 * 240 * 8000, 1.0);
  EXPECT_NEAR(ifc.volume(ifc.of("IFCBUILDINGELEMENTPROXY")[0]), 500.0 * 500 * 500, 1.0);
  for (int opening : ifc.of("IFCOPENINGELEMENT")) EXPECT_GT(ifc.volume(opening), 1e6);
  EXPECT_EQ(ifc.at(ifc.of("IFCROOF")[0]).args[8], ".HIP_ROOF.");
  // The stair (docs/103): one turn with winders.
  EXPECT_EQ(summary.stairs, 1);
  ASSERT_EQ(ifc.of("IFCSTAIR").size(), 1u);
  EXPECT_EQ(ifc.at(ifc.of("IFCSTAIR")[0]).args[8], ".QUARTER_WINDING_STAIR.");
  EXPECT_NEAR(ifc.volume(ifc.of("IFCSTAIR")[0]), volume(stair), 1e-6 * volume(stair));

  // Each opening voids its own wall; the window and the door fill their openings.
  const int south = ifc.named("IFCWALL", name(walls[0])), east = ifc.named("IFCWALL", name(walls[1]));
  int inSouth = 0, inEast = 0;
  for (int rel : ifc.of("IFCRELVOIDSELEMENT")) {
    inSouth += refsIn(ifc.at(rel).args[4])[0] == south;
    inEast += refsIn(ifc.at(rel).args[4])[0] == east;
  }
  EXPECT_EQ(inSouth, 2);
  EXPECT_EQ(inEast, 1);
  const Entity& door = ifc.at(ifc.of("IFCDOOR")[0]);
  EXPECT_EQ(door.args[2], "'SD-1'");  // its mark as its name (docs/117)
  EXPECT_EQ(door.args[3], "'P-900'");  // its product number
  EXPECT_EQ(door.args[8], "2100.");  // OverallHeight
  EXPECT_EQ(door.args[9], "900.");   // OverallWidth
  EXPECT_EQ(door.args[11], ".SINGLE_SWING_RIGHT.");
  const Entity& window = ifc.at(ifc.of("IFCWINDOW")[0]);
  EXPECT_EQ(window.args[2], "'W1'");  // the worked-out mark
  EXPECT_EQ(window.args[3], "$");
  EXPECT_EQ(window.args[8], "1200.");
  EXPECT_EQ(window.args[9], "1600.");
  EXPECT_EQ(window.args[11], ".DOUBLE_PANEL_VERTICAL.");

  // Storeys: the walls and the column stand on 1FL; the slab (its top at 2FL) and the roof are on 2FL.
  std::map<int, std::set<int>> contained;
  for (int rel : ifc.of("IFCRELCONTAINEDINSPATIALSTRUCTURE"))
    for (int e : refsIn(ifc.at(rel).args[4])) contained[refsIn(ifc.at(rel).args[5])[0]].insert(e);
  const int first = ifc.named("IFCBUILDINGSTOREY", "1FL"), second = ifc.named("IFCBUILDINGSTOREY", "2FL");
  ASSERT_TRUE(first && second);
  EXPECT_EQ(ifc.at(second).args[9], "3000.");
  EXPECT_TRUE(contained[first].count(south));
  EXPECT_TRUE(contained[first].count(ifc.of("IFCCOLUMN")[0]));
  EXPECT_TRUE(contained[second].count(ifc.of("IFCSLAB")[0]));
  EXPECT_TRUE(contained[second].count(ifc.of("IFCROOF")[0]));
  // Every element is in exactly one storey.
  std::map<int, int> times;
  for (const auto& [storey, elements] : contained)
    for (int e : elements) ++times[e];
  for (const char* type : {"IFCWALL", "IFCSLAB", "IFCROOF", "IFCSTAIR", "IFCCOLUMN", "IFCBEAM", "IFCBUILDINGELEMENTPROXY", "IFCWINDOW", "IFCDOOR", "IFCGRID"})
    for (int e : ifc.of(type)) EXPECT_EQ(times[e], 1) << type;

  // The room: a space of 1FL with its areas (m2) and a Japanese name written the STEP way.
  const Entity& space = ifc.at(ifc.of("IFCSPACE")[0]);
  EXPECT_EQ(space.args[2], "'\\X2\\5C459593\\X0\\ LDK'");  // 居間 LDK
  EXPECT_EQ(space.args[4], "'Living'");  // its use (docs/117)
  {
    bool floor = false, ceiling = false, height = false, set = false;
    for (int p : ifc.of("IFCPROPERTYSINGLEVALUE")) {
      floor |= ifc.at(p).args[0] == "'FloorCovering'" && ifc.at(p).args[2] == "IFCLABEL('Flooring t15')";
      ceiling |= ifc.at(p).args[0] == "'CeilingCovering'" && ifc.at(p).args[2] == "IFCLABEL('PB t9.5')";
    }
    for (int q : ifc.of("IFCQUANTITYLENGTH"))
      height |= ifc.at(q).args[0] == "'FinishCeilingHeight'" && std::abs(std::stod(ifc.at(q).args[3]) - 2400) < 1e-6;
    for (int s : ifc.of("IFCPROPERTYSET")) set |= ifc.at(s).args[2] == "'Pset_SpaceCoveringRequirements'";
    EXPECT_TRUE(floor && ceiling && height && set);
  }
  const json room = ok("list_rooms")["rooms"][0];
  bool gross = false, net = false;
  for (int q : ifc.of("IFCQUANTITYAREA")) {
    const Entity& e = ifc.at(q);
    if (e.args[0] == "'GrossFloorArea'") gross = std::abs(std::stod(e.args[3]) - room["area_centre"].get<double>()) < 1e-5;
    if (e.args[0] == "'NetFloorArea'") net = std::abs(std::stod(e.args[3]) - room["area_inner"].get<double>()) < 1e-5;
  }
  EXPECT_TRUE(gross);
  EXPECT_TRUE(net);
  // docs/127: the ceiling board is the room's ceiling (a covering of the space), on 1FL, 10 thick.
  ASSERT_EQ(ifc.of("IFCCOVERING").size(), 1u);
  const int covering = ifc.of("IFCCOVERING")[0];
  EXPECT_EQ(ifc.at(covering).args[8], ".CEILING.");
  EXPECT_NEAR(ifc.volume(covering), room["area_inner"].get<double>() * 1e6 * 10, 1e-3 * room["area_inner"].get<double>() * 1e7);
  ASSERT_EQ(ifc.of("IFCRELCOVERSSPACES").size(), 1u);
  const Entity& covers = ifc.at(ifc.of("IFCRELCOVERSSPACES")[0]);
  EXPECT_EQ(refsIn(covers.args[4])[0], ifc.of("IFCSPACE")[0]);
  EXPECT_EQ(refsIn(covers.args[5]), std::vector<int>{covering});
  EXPECT_TRUE(contained[first].count(covering));
  EXPECT_EQ(times[covering], 1);
  const Entity& solid = ifc.at(ifc.of("IFCEXTRUDEDAREASOLID")[0]);
  EXPECT_EQ(solid.args[3], "3000.");  // up to 2FL

  // The wall type and its quantities.
  bool reference = false, external = false;
  for (int p : ifc.of("IFCPROPERTYSINGLEVALUE")) {
    reference |= ifc.at(p).args[0] == "'Reference'";
    external |= ifc.at(p).args[0] == "'IsExternal'" && ifc.at(p).args[2] == "IFCBOOLEAN(.T.)";
  }
  EXPECT_TRUE(reference && external);

  // The grid: X1, X2 (lines along Y at x = 0 and 8000) and Y1, Y2.
  const Entity& grid = ifc.at(ifc.of("IFCGRID")[0]);
  const std::vector<int> us = refsIn(grid.args[7]), vs = refsIn(grid.args[8]);
  ASSERT_EQ(us.size(), 2u);
  ASSERT_EQ(vs.size(), 2u);
  EXPECT_EQ(ifc.at(us[1]).args[0], "'X2'");
  for (int p : refsIn(ifc.at(refsIn(ifc.at(us[1]).args[1])[0]).args[0])) EXPECT_NEAR(ifc.point(p)[0], 8000, 1e-6);
  for (int p : refsIn(ifc.at(refsIn(ifc.at(vs[1]).args[1])[0]).args[0])) EXPECT_NEAR(ifc.point(p)[1], 6000, 1e-6);

  // Written again, every element has the id it had (other software recognises it).
  const auto again = sbr::doc::ifcText(modeler, "house.ifc");
  ASSERT_TRUE(again);
  const Ifc second2(*again);
  auto ids = [](const Ifc& file) {
    std::set<std::string> out;
    for (const auto& [id, e] : file.entities)
      if (e.args.size() >= 9 && e.type != "IFCGEOMETRICREPRESENTATIONSUBCONTEXT") out.insert(e.type + e.args[0]);
    return out;
  };
  EXPECT_EQ(ids(ifc), ids(second2));
  EXPECT_EQ(summary.skipped, 1);  // the line
}

TEST_F(IfcExport, CurvedSolidsRoomsLeftOpenAndEmptyModels) {
  EXPECT_FALSE(sbr::doc::ifcText(modeler, "empty.ifc"));  // nothing to export
  // No levels: one storey, said in the notes. A cylinder goes as triangles.
  const std::string circle = ok("create_circle", {{"center", {0, 0, 0}}, {"radius", 500}})["id"];
  ok("extrude", {{"id", circle}, {"distance", 2000}});
  const json walls = ok("create_wall", {{"points", {{3000, 0}, {6000, 0}, {6000, 3000}, {3000, 3000}, {3000, 0}}}, {"thickness", 150}, {"height", 2400}})["ids"];
  ok("create_room", {{"at", {4500, 1500}}, {"name", "A"}});
  ok("delete", {{"ids", {walls[2]}}});  // the room is open now
  sbr::doc::IfcSummary summary;
  const auto text = sbr::doc::ifcText(modeler, "it's.ifc", &summary);
  ASSERT_TRUE(text) << text.error();
  const Ifc ifc(*text);
  expectWellFormed(ifc);
  EXPECT_EQ(summary.storeys, 1);
  EXPECT_EQ(summary.spaces, 0);  // not exported, and said
  EXPECT_EQ(summary.proxies, 1);
  bool saidRoom = false, saidLevels = false, saidTriangles = false;
  for (const std::string& n : summary.notes) {
    saidRoom |= n.find("Room 'A'") != std::string::npos;
    saidLevels |= n.find("No levels") != std::string::npos;
    saidTriangles |= n.find("triangles") != std::string::npos;
  }
  EXPECT_TRUE(saidRoom && saidLevels && saidTriangles);
  // The cylinder as triangles: its volume within the chord error of the mesh.
  const double exact = 3.14159265358979 * 500 * 500 * 2000;
  EXPECT_NEAR(ifc.volume(ifc.of("IFCBUILDINGELEMENTPROXY")[0]), exact, 0.02 * exact);
  EXPECT_NE(ifc.header.find("'it''s.ifc'"), std::string::npos);  // apostrophes doubled

  // To a file, through the command.
  const std::string path = (std::filesystem::temp_directory_path() / "sbr_ifc_export_test.ifc").string();
  const json done = ok("export_ifc", {{"path", path}});
  EXPECT_EQ(done["walls"], 3);
  std::ifstream file(path, std::ios::binary);
  const std::string written((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  EXPECT_EQ(written.rfind("ISO-10303-21;", 0), 0u);
  file.close();
  std::filesystem::remove(path);
}
}  // namespace
