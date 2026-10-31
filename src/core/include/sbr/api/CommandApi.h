#pragma once
// JSON command interface for scripts and AI agents.
//
// Every command goes through doc::Modeler, exactly like the GUI tools, so
// validation, rollback and undo/redo apply to AI edits as well. Requests and
// responses are JSON text, keeping this header free of any JSON library.
//
//   request : {"command": "create_box", "args": {...}}
//   response: {"ok": true, "result": {...}}  or  {"ok": false, "error": "..."}
//
// Units: millimetres, angles in degrees, colours as [r, g, b] in 0..1.

#include <functional>
#include <string>
#include <vector>

#include "sbr/doc/Modeler.h"

namespace sbr::api {

class CommandApi {
 public:
  struct Options {
    // save / open / export_step. The in-app bridge turns these off so an
    // agent cannot replace or overwrite the user's files.
    bool allowFileCommands = true;
  };

  // What a command may touch (review C). Ordered: a batch has the largest
  // effect of its steps.
  enum class Effect {
    Read,        // reads the model or app state only (also preview: a scratch copy)
    ViewWrite,   // camera, selection, proposal display: not the model
    ModelWrite,  // the document (geometry, attributes, persistent settings)
    ExternalIo,  // files outside the model
  };
  static const char* toString(Effect effect);

  // Host-specific command (e.g. the GUI's selection or view capture).
  // The handler receives the "args" object as JSON text and returns the
  // result as JSON text; throw std::exception to report an error.
  struct Extension {
    std::string name;
    std::string description;
    std::string inputSchema;  // JSON Schema of "args"
    std::function<std::string(const std::string& argsJson)> run;
    Effect effect = Effect::ModelWrite;  // when unsure, assume it writes
  };

  explicit CommandApi(doc::Modeler& modeler) : modeler_(modeler) {}
  CommandApi(doc::Modeler& modeler, Options options) : modeler_(modeler), options_(options) {}

  void addExtension(Extension extension) { extensions_.push_back(std::move(extension)); }

  // Executes one request (or a "batch"). Never throws.
  std::string execute(const std::string& requestJson);

  // The effect a request would have, without running it. Unknown or
  // malformed requests count as ModelWrite. Hosts use it to leave a
  // half-finished user input alone while an agent only reads.
  Effect effectOf(const std::string& requestJson) const;

  // JSON array of {name, description, input_schema} for every available
  // command: usable as tool definitions for LLM tool use / MCP.
  std::string schemaJson() const;

  // Commands only the running app provides (selection, camera, capture),
  // without handlers. Shared so sbr_mcp can list them before the app starts.
  static std::vector<Extension> appCommandSpecs();

 private:
  doc::Modeler& modeler_;
  Options options_;
  std::vector<Extension> extensions_;
};

}  // namespace sbr::api
