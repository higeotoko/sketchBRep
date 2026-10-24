#include "sbr/api/McpServer.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>

#ifdef _WIN32
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

namespace sbr::api {
namespace {

using json = nlohmann::json;

const char* const kProtocolVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

json rpcResult(const json& id, json result) {
  return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json rpcError(const json& id, int code, const std::string& message) {
  return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

std::string base64(const std::string& bytes) {
  static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) |
                       (static_cast<unsigned char>(bytes[i + 1]) << 8) | static_cast<unsigned char>(bytes[i + 2]);
    out += table[(v >> 18) & 63];
    out += table[(v >> 12) & 63];
    out += table[(v >> 6) & 63];
    out += table[v & 63];
  }
  if (i < bytes.size()) {
    unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
    if (i + 1 < bytes.size()) v |= static_cast<unsigned char>(bytes[i + 1]) << 8;
    out += table[(v >> 18) & 63];
    out += table[(v >> 12) & 63];
    out += i + 1 < bytes.size() ? table[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

// tools/call result. A command result carrying "image_path" (capture_view)
// also returns the picture itself so the model can look at the view.
json toolResult(const json& response) {
  if (!response.value("ok", false)) {
    return json{{"content", {{{"type", "text"}, {"text", response.value("error", std::string("failed"))}}}},
                {"isError", true}};
  }
  const json result = response.value("result", json::object());
  json content = json::array({{{"type", "text"}, {"text", result.dump()}}});
  if (result.is_object() && result.contains("image_path") && result["image_path"].is_string()) {
    std::ifstream in(result["image_path"].get<std::string>(), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!bytes.empty()) content.push_back({{"type", "image"}, {"data", base64(bytes)}, {"mimeType", "image/png"}});
  }
  return json{{"content", content}, {"isError", false}};
}

}  // namespace

const char* McpServer::instructions() {
  return "SketchBRep is an architectural B-rep modeler. Units are millimetres, angles degrees, Z is up. "
         "Every tool call is validated like a GUI edit: on failure nothing changes. Workflow: "
         "list_objects / get_object to inspect (get_object lists numbered faces with normals), create "
         "profiles (create_rectangle, create_circle, create_polygon) or create_box, then extrude / push_pull "
         "(select faces by index or {\"normal\": [x,y,z]}). Dimensions stay editable through set_parameter "
         "(keys from get_object.parameters). Use batch for multi-step edits: it is one undo step and is "
         "reverted as a whole if a step fails; \"$1.id\" refers to step 1's result. Prefer editing "
         "parameters over rebuilding geometry.";
}

std::optional<std::string> McpServer::handle(const std::string& message) {
  json request;
  try {
    request = json::parse(message);
  } catch (const json::exception&) {
    return rpcError(nullptr, -32700, "parse error").dump();
  }
  if (!request.is_object()) return rpcError(nullptr, -32600, "invalid request").dump();
  const json id = request.value("id", json());
  const std::string method = request.value("method", std::string());
  const bool notification = !request.contains("id");
  const json params = request.value("params", json::object());

  if (notification) return std::nullopt;  // notifications/initialized, cancelled, ...

  if (method == "initialize") {
    std::string version = kProtocolVersions[0];
    const std::string wanted = params.value("protocolVersion", std::string());
    for (const char* v : kProtocolVersions) {
      if (wanted == v) version = v;
    }
    return rpcResult(id, {{"protocolVersion", version},
                          {"capabilities", {{"tools", json::object()}}},
                          {"serverInfo", {{"name", serverName_}, {"version", "0.1.0"}}},
                          {"instructions", instructions()}})
        .dump();
  }
  if (method == "ping") return rpcResult(id, json::object()).dump();
  if (method == "tools/list") {
    const json described = json::parse(backend_(R"({"command": "describe"})"), nullptr, false);
    if (described.is_discarded() || !described.value("ok", false)) {
      const std::string why = described.is_discarded() ? "backend unavailable"
                                                       : described.value("error", std::string("backend error"));
      return rpcError(id, -32603, why).dump();
    }
    json tools = json::array();
    for (const json& t : described["result"]) {
      json tool = {{"name", t["name"]}, {"description", t["description"]}, {"inputSchema", t["input_schema"]}};
      // MCP tool annotations: clients may run read-only tools without asking.
      if (t.value("effect", std::string()) == "read") tool["annotations"] = {{"readOnlyHint", true}};
      tools.push_back(std::move(tool));
    }
    return rpcResult(id, {{"tools", tools}}).dump();
  }
  if (method == "tools/call") {
    const std::string name = params.value("name", std::string());
    if (name.empty()) return rpcError(id, -32602, "missing tool name").dump();
    const json call = {{"command", name}, {"args", params.value("arguments", json::object())}};
    json response = json::parse(backend_(call.dump()), nullptr, false);
    if (response.is_discarded()) response = json{{"ok", false}, {"error", "backend returned invalid JSON"}};
    return rpcResult(id, toolResult(response)).dump();
  }
  return rpcError(id, -32601, "method not found: " + method).dump();
}

std::string bridgeServerName() {
  // MCP clients may start sbr_mcp with a reduced environment, so ask the OS
  // for the account name where possible.
#ifdef _WIN32
  char account[257] = {};
  DWORD size = sizeof(account);
  const char* user = GetUserNameA(account, &size) ? account : std::getenv("USERNAME");
#else
  const passwd* pw = getpwuid(getuid());
  const char* user = pw ? pw->pw_name : std::getenv("USER");
#endif
  std::string name = "sketchbrep-ai";
  if (user && *user) {
    name += '-';
    for (const char* c = user; *c; ++c) {
      const unsigned char u = static_cast<unsigned char>(*c);
      name += std::isalnum(u) ? static_cast<char>(u) : '_';
    }
  }
  return name;
}

}  // namespace sbr::api
