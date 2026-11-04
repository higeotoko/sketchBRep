#include "sbr/ConstructionGuide.h"

#include <cmath>
#include <sstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace sbr::embedded {
extern const char* const kTimber_sections[];
extern const unsigned kTimber_sections_parts;
extern const char* const kTimber_roles[];
extern const unsigned kTimber_roles_parts;
extern const char* const kTimber_presets[];
extern const unsigned kTimber_presets_parts;
extern const char* const kTimber_recipes[];
extern const unsigned kTimber_recipes_parts;
extern const char* const kTimber_hardware[];
extern const unsigned kTimber_hardware_parts;
}

namespace sbr::construction {
namespace {
using json = nlohmann::json;
constexpr const char* kNotice =
    "意匠・概略の検討用。構造の安全、法令への適合、施工できることを判定・保証しません。"
    "標準寸法は市場の流通寸法の目安で、法令の値ではありません。"
    "梁せいは形を作るための仮の値です。スパンから自動で決めません。"
    "樹種・等級は属性のみ。生成は概略形です。診断・壁量計算は未実装です。";

json read(const char* const* parts, unsigned count) {
  std::string text;
  for (unsigned i = 0; i < count; ++i) text += parts[i];
  return json::parse(text);
}

const json& builtin(const std::string& system) {
  if (system != "timber") throw std::runtime_error("Construction guide not implemented for system '" + system + "'");
  static const json data = {
      {"schema_version", 1}, {"system", "timber"}, {"title", "木造の手引き"}, {"notice", kNotice},
      {"sections", read(embedded::kTimber_sections, embedded::kTimber_sections_parts)},
      {"roles", read(embedded::kTimber_roles, embedded::kTimber_roles_parts)},
      {"presets", read(embedded::kTimber_presets, embedded::kTimber_presets_parts)},
      {"recipes", read(embedded::kTimber_recipes, embedded::kTimber_recipes_parts)},
      {"hardware", read(embedded::kTimber_hardware, embedded::kTimber_hardware_parts)}};
  return data;
}

void require(bool condition, const std::string& why) {
  if (!condition) throw std::runtime_error("Construction guide: " + why);
}
void metadata(const json& table) {
  require(table.is_object() && table.contains("source") && table.at("source").is_object(), "source is required");
  const auto& source = table.at("source");
  for (const char* key : {"name", "version", "date", "url"}) {
    require(source.contains(key) && source.at(key).is_string() && !source.at(key).get<std::string>().empty(),
            std::string("source.") + key + " is required");
  }
  const std::string normative = table.value("normative", std::string());
  require(normative == "market_practice" || normative == "specification" || normative == "law" || normative == "user",
          "normative must be market_practice, specification, law or user");
}
void positive(const json& value, const std::string& key) {
  require(value.is_number() && std::isfinite(value.get<double>()) && value.get<double>() >= tolerance::kMinDimension,
          key + " must be a finite positive number");
}

geom::Section asSection(const std::string& id, const json& entry) {
  require(entry.at("kind") == "rectangle", "'" + id + "' has no linear section (sheet or product unselected)");
  geom::Section s;
  s.width = entry.at("width").get<double>();
  s.depth = entry.at("depth").get<double>();
  s.catalog = id;
  s.label = entry.at("name").get<std::string>();
  return s;
}

void validate(const json& data) {
  for (const char* key : {"sections", "roles", "presets"}) {
    const auto& table = data.at(key);
    metadata(table);
    require(table.at("entries").is_object() && !table.at("entries").empty(), std::string(key) + ": entries required");
    for (const auto& [id, entry] : table.at("entries").items()) {
      require(!id.empty() && entry.is_object() && entry.at("name").is_string() && !entry.at("name").get<std::string>().empty(),
              "entries need ids and names");
    }
  }
  const auto& sections = data.at("sections").at("entries");
  for (const auto& [id, entry] : sections.items()) {
    const std::string kind = entry.at("kind").get<std::string>();
    require(kind == "rectangle" || kind == "sheet" || kind == "unselected", "unknown section kind");
    const std::string material = entry.at("material_class").get<std::string>();
    require(material == "timber" || material == "steel" || material == "rc", "unknown material_class");
    if (kind == "rectangle") {
      require(id.rfind("TIMBER-", 0) == 0 && material == "timber", "timber rectangles need a TIMBER- id and timber material");
      positive(entry.at("width"), id + ".width");
      positive(entry.at("depth"), id + ".depth");
      require(geom::validateSection(asSection(id, entry)).ok(), "invalid section '" + id + "'");
    }
    if (kind == "sheet") positive(entry.at("thickness"), id + ".thickness");
  }
  for (const auto& [id, role] : data.at("roles").at("entries").items()) {
    const std::string def = role.at("default_section").get<std::string>();
    require(role.at("sections").is_array() && !role.at("sections").empty(), id + ": section choices required");
    bool found = false;
    for (const auto& s : role.at("sections")) {
      require(s.is_string() && sections.contains(s.get<std::string>()), id + ": unknown section");
      if (s == def) found = true;
    }
    require(found, id + ": default_section must be in section choices");
    require(role.at("note").is_string(), id + ": note must be text");
  }
  for (const auto& [id, p] : data.at("presets").at("entries").items()) {
    const auto& value = p.at("value");
    require(value.is_number() || value.is_string(), id + ": value must be a number or text");
    const auto& original = builtin("timber").at("presets").at("entries");
    if (original.contains(id)) {
      require(value.is_number() == original.at(id).at("value").is_number(), id + ": cannot change preset value type");
    }
    if (value.is_number()) {
      require(std::isfinite(value.get<double>()), id + ": value must be finite");
      if (p.contains("minimum")) {
        require(p.at("minimum").is_number() && value.get<double>() >= p.at("minimum").get<double>(), id + ": value is below minimum");
      }
    }
    require(p.at("unit").is_string() && p.at("note").is_string(), id + ": unit and note must be text");
    if (p.contains("choices")) {
      require(p.at("choices").is_array(), id + ": choices must be an array");
      for (const auto& choice : p.at("choices")) {
        require(choice.is_number() || choice.is_string(), id + ": invalid choice");
        if (choice.is_number()) require(std::isfinite(choice.get<double>()), id + ": choice must be finite");
      }
    }
  }
}

json effective(const std::string& system, const std::string& overrides) {
  json data = builtin(system);
  const json patch = json::parse(overrides);
  require(patch.is_object(), "overrides must be an object");
  if (!patch.empty()) {
    metadata(patch);
    for (const auto& [key, value] : patch.items()) {
      require(key == "source" || key == "normative" || key == "sections" || key == "roles" || key == "presets",
              "unknown override key '" + key + "'");
      if (key == "source" || key == "normative") continue;
      require(value.is_object(), key + " override must be an object");
      for (const auto& [field, unused] : value.items()) {
        (void)unused;
        require(field == "entries", "override only entries; provenance is recorded separately");
      }
      require(value.contains("entries") && value.at("entries").is_object(), key + ": entries must be an object");
      data[key].merge_patch(value);
    }
    // Keep original sources and user provenance separately. The guide and
    // list_sections return both, rather than attributing a user size to a source.
    data["overrides"] = patch;
  }
  validate(data);
  return data;
}

std::string display(const json& v) { return v.is_string() ? v.get<std::string>() : v.dump(); }
std::string cell(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '|') out += "\\|";
    else if (c == '\n' || c == '\r') out += ' ';
    else out += c;
  }
  return out;
}
void sourceText(std::ostream& out, const json& table) {
  const auto& s = table.at("source");
  out << "出典: " << cell(s.at("name")) << " / 版 " << cell(s.at("version")) << " / " << cell(s.at("date"))
      << " / " << cell(s.at("url")) << " / " << cell(table.at("normative")) << "\n\n";
}
}  // namespace

std::string guideData(const std::string& system, const std::string& overrides) { return effective(system, overrides).dump(); }
void validateOverrides(const std::string& system, const std::string& overrides) { (void)effective(system, overrides); }

geom::Section section(const std::string& id, const std::string& overrides) {
  const auto data = effective("timber", overrides);
  const auto& entries = data.at("sections").at("entries");
  require(entries.contains(id), "unknown section '" + id + "'");
  return asSection(id, entries.at(id));
}

const geom::CatalogSection* findSection(const std::string& id) {
  static const std::vector<geom::CatalogSection> catalog = [] {
    std::vector<geom::CatalogSection> out;
    const auto data = effective("timber", "{}");
    for (const auto& [key, e] : data.at("sections").at("entries").items()) {
      if (e.at("kind") != "rectangle") continue;
      geom::CatalogSection c;
      c.id = key; c.name = e.at("name"); c.family = "TIMBER";
      c.standard = "market_practice"; c.material = e.at("material_class");
      c.section = asSection(key, e);
      c.dimensions = {{"B", c.section.width}, {"H", c.section.depth}};
      out.push_back(std::move(c));
    }
    return out;
  }();
  for (const auto& e : catalog) if (e.id == id) return &e;
  return nullptr;
}

std::string guideMarkdown(const std::string& system, const std::string& overrides) {
  const json data = effective(system, overrides);
  std::ostringstream out;
  out << "# " << display(data.at("title")) << "\n\n" << display(data.at("notice")) << "\n\n";
  if (data.contains("overrides")) {
    out << "## 利用者の上書き（このプロジェクト）\n\n";
    sourceText(out, data.at("overrides"));
    out << "以下は上書き後の値です。元の表は変更していません。\n\n";
  }
  out << "## 役割と既定の断面（目安・mm）\n\n";
  sourceText(out, data.at("roles"));
  out << "| kind | 役割 | 既定 | 選べる断面 | 備考 |\n|---|---|---|---|---|\n";
  const auto& entries = data.at("sections").at("entries");
  for (const auto& [id, r] : data.at("roles").at("entries").items()) {
    const auto& def = entries.at(r.at("default_section").get<std::string>());
    out << "| " << cell(id) << " | " << cell(r.at("name")) << " | " << cell(def.at("name"));
    if (def.at("kind") == "rectangle") out << " (" << display(def.at("width")) << "×" << display(def.at("depth")) << ")";
    if (def.at("kind") == "sheet") out << " (厚" << display(def.at("thickness")) << ")";
    out << " | ";
    for (const auto& s : r.at("sections")) out << cell(s.get<std::string>()) << " ";
    out << "| " << cell(r.at("note")) << " |\n";
  }
  out << "\n## 断面カタログ（目安・mm）\n\n";
  sourceText(out, data.at("sections"));
  out << "| ID | 名称 | 種類 | 幅 | せい | 厚み | 材料 |\n|---|---|---|---|---|---|---|\n";
  for (const auto& [id, s] : entries.items()) {
    out << "| " << cell(id) << " | " << cell(s.at("name")) << " | " << cell(s.at("kind")) << " | "
        << display(s.value("width", json("—"))) << " | " << display(s.value("depth", json("—"))) << " | "
        << display(s.value("thickness", json("—"))) << " | " << cell(s.at("material_class")) << " |\n";
  }
  out << "\n## プリセット\n\n";
  sourceText(out, data.at("presets"));
  out << "| ID | 項目 | 初期値 | 単位 | 候補 | 備考 |\n|---|---|---|---|---|---|\n";
  for (const auto& [id, p] : data.at("presets").at("entries").items()) {
    out << "| " << cell(id) << " | " << cell(p.at("name")) << " | " << cell(display(p.at("value")))
        << " | " << cell(p.at("unit")) << " | " << cell(p.value("choices", json::array()).dump())
        << " | " << cell(p.at("note")) << " |\n";
  }
  for (const char* key : {"recipes", "hardware"}) {
    out << "\n## " << (std::string(key) == "recipes" ? "木造レシピ" : "金物の簡略表示") << "\n\n";
    sourceText(out, data.at(key));
    for (const auto& [id, e] : data.at(key).at("entries").items()) {
      out << "- " << cell(e.at("name")) << " (`" << cell(id) << "`)";
      if (e.contains("defaults")) out << "：" << e.at("defaults").dump(-1, ' ', false);
      out << "\n";
      if (e.contains("notes")) for (const auto& note : e.at("notes")) out << "  - " << cell(note) << "\n";
    }
  }
  out << "\ngenerate_frame {system: timber, recipe: post_beam_2storey, inputs, preview: true} で試し、"
         "返されたrevisionをexpected_revisionにして preview: false で確定します。"
         "画面では木造架構メニューからプレビューし、適用・却下を選べます。\n";
  out << "\n役割は既存属性 kind、材料は material_class を使います。"
         "AI は list_sections {system: timber, role: column} で断面を選び、"
         "create_member の section に返された ID、role に役割を渡します。"
         "section オブジェクトを使う場合は material_class も明示します。\n\n"
         "上書きは set_construction_overrides で登録し、.sbr と一緒に保存・共有します。"
         "get_construction_guide の markdown は文書として保存できます。\n";
  return out.str();
}
}  // namespace sbr::construction
