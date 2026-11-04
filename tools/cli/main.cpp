// sbr_cli — headless SketchBRep for scripts and AI agents.
//
//   sbr_cli [--open in.sbr] [--save out.sbr] [script.jsonl]   run commands (stdin if no script)
//   sbr_cli --schema                                           print the command schema
//   sbr_cli --mcp [--open in.sbr]                              MCP server over stdio (headless)
//
// Script: one CommandApi request per line ({"command": ..., "args": {...}}).
// Blank lines and lines starting with '#' are skipped. One response is
// printed per request; the exit code is 1 if any request failed.

#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/api/McpServer.h"
#include "sbr/doc/Modeler.h"

namespace {

int usage() {
  std::cerr << "usage: sbr_cli [--open in.sbr] [--save out.sbr] [script.jsonl]\n"
               "       sbr_cli --schema\n"
               "       sbr_cli --mcp [--open in.sbr]\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  std::string openPath, savePath, scriptPath;
  bool schema = false, mcp = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--open" && i + 1 < argc) {
      openPath = argv[++i];
    } else if (a == "--save" && i + 1 < argc) {
      savePath = argv[++i];
    } else if (a == "--schema") {
      schema = true;
    } else if (a == "--mcp") {
      mcp = true;
    } else if (!a.empty() && a[0] != '-' && scriptPath.empty()) {
      scriptPath = a;
    } else {
      return usage();
    }
  }

  sbr::doc::Modeler modeler;
  sbr::api::CommandApi api(modeler);
  if (schema) {
    std::cout << nlohmann::json::parse(api.schemaJson()).dump(2) << "\n";
    return 0;
  }
  if (!openPath.empty()) {
    if (const auto status = modeler.load(openPath); !status) {
      std::cerr << "cannot open " << openPath << ": " << status.message() << "\n";
      return 1;
    }
  }

  if (mcp) {
    // stdout carries protocol messages only; diagnostics go to stderr.
    sbr::api::McpServer server([&api](const std::string& request) { return api.execute(request); },
                               "sketchbrep-headless");
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
      if (const auto response = server.handle(line)) std::cout << *response << "\n" << std::flush;
    }
    return 0;
  }

  std::ifstream file;
  if (!scriptPath.empty()) {
    file.open(scriptPath);
    if (!file) {
      std::cerr << "cannot read " << scriptPath << "\n";
      return 1;
    }
  }
  std::istream& in = scriptPath.empty() ? std::cin : file;
  bool failed = false;
  std::string line;
  while (std::getline(in, line)) {
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos || line[first] == '#') continue;
    const std::string response = api.execute(line);
    failed = failed || !nlohmann::json::parse(response).value("ok", false);
    std::cout << response << "\n" << std::flush;
  }
  if (!savePath.empty()) {
    if (const auto status = modeler.save(savePath); !status) {
      std::cerr << "cannot save " << savePath << ": " << status.message() << "\n";
      return 1;
    }
  }
  return failed ? 1 : 0;
}
