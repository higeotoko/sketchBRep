#pragma once
// Model Context Protocol (MCP) server logic, transport independent.
//
// MCP clients (Claude Desktop, Claude Code, ...) speak JSON-RPC 2.0, one
// message per line over stdio. This class turns those messages into
// CommandApi requests handed to a backend: an in-process CommandApi
// (headless) or the running SketchBRep app over its local socket.

#include <functional>
#include <optional>
#include <string>

namespace sbr::api {

class McpServer {
 public:
  // Executes one CommandApi request (JSON text) and returns its response.
  using Backend = std::function<std::string(const std::string& requestJson)>;

  explicit McpServer(Backend backend, std::string serverName = "sketchbrep")
      : backend_(std::move(backend)), serverName_(std::move(serverName)) {}

  // Handles one JSON-RPC message. Returns the response line, or nothing for
  // notifications.
  std::optional<std::string> handle(const std::string& message);

  // Usage notes sent to the model on initialize.
  static const char* instructions();

 private:
  Backend backend_;
  std::string serverName_;
};

// Name of the app's local socket (named pipe on Windows), per user.
std::string bridgeServerName();

}  // namespace sbr::api
