// LayOut (.layout) documents from drawing sheets (docs/185), through Trimble's LayOut C API in
// the SketchUp SDK. As for SketchUp (Skp.cpp): compiled against the SDK's headers only when the
// build is pointed at it (SBR_SKETCHUP_SDK_DIR), never linked; LayOutAPI.dll is loaded when first
// needed: from the "layout" folder next to the program (the ZIP), else the SDK folder.
#include "sbr/draw/Drawing.h"

#ifdef SBR_WITH_SKETCHUP

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <mutex>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <io.h>

#include <LayOutAPI/layout.h>

#endif

namespace sbr::draw {

#ifndef SBR_WITH_SKETCHUP

bool layoutAvailable(std::string* why) {
  if (why) *why = "This build of SketchBRep has no LayOut support (built without the SketchUp SDK)";
  return false;
}

Status writeLayout(const std::vector<Sheet>&, const std::vector<std::string>&, const std::string&) {
  std::string why;
  layoutAvailable(&why);
  return Status::failure(why);
}

Result<int> layoutPageCount(const std::string&) {
  std::string why;
  layoutAvailable(&why);
  return Result<int>::failure(why);
}

Status layoutToPdf(const std::string&, const std::string&) {
  std::string why;
  layoutAvailable(&why);
  return Status::failure(why);
}

#else

namespace {

#define SBR_LO_FUNCTIONS(X)                                                                             \
  X(LOInitialize) X(LODocumentCreateEmpty) X(LODocumentRelease) X(LODocumentSaveToFile)                   \
  X(LODocumentGetPageInfo) X(LODocumentGetPages) X(LODocumentAddPage) X(LODocumentAddEntityUsingIndexes) \
  X(LOPageInfoSetWidth) X(LOPageInfoSetHeight) X(LOPageListCreate) X(LOPageListRelease)                  \
  X(LOPageListGetNumberOfPages) X(LOPageListGetPageAtIndex) X(LOPageSetName) X(LOPathCreate)             \
  X(LOPathCreateArc) X(LOPathAppendLineTo) X(LOPathClose) X(LOPathRelease) X(LOPathToEntity)             \
  X(LOFormattedTextCreateAtPoint) X(LOFormattedTextRelease) X(LOFormattedTextToEntity) X(LOStyleCreate)  \
  X(LOStyleRelease) X(LOStyleSetStroked) X(LOStyleSetStrokeWidth) X(LOStyleSetStrokeColor)                \
  X(LOStyleSetStrokePattern) X(LOStyleSetSolidFilled) X(LOStyleSetSolidFillColor) X(LOStyleSetFontSize)  \
  X(LOEntitySetStyle) X(LODocumentCreateFromFile) X(LODocumentExportToPDF)

struct Api {
#define SBR_LO_POINTER(f) decltype(&::f) f = nullptr;
  SBR_LO_FUNCTIONS(SBR_LO_POINTER)
#undef SBR_LO_POINTER
};

std::vector<std::wstring> dllFolders() {
  std::vector<std::wstring> out;
  {
    std::wstring exe(MAX_PATH, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (n > 0 && n < exe.size()) {
      exe.resize(n);
      const auto slash = exe.find_last_of(L"\\/");
      if (slash != std::wstring::npos) out.push_back(exe.substr(0, slash) + L"\\layout");
    }
  }
  auto add = [&](std::wstring root) {
    if (root.empty()) return;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    out.push_back(root + L"\\binaries\\layout\\x64");
  };
  if (const wchar_t* env = _wgetenv(L"SBR_SKETCHUP_SDK")) add(env);
#ifdef SBR_SKETCHUP_SDK_DIR
  add(std::filesystem::u8path(SBR_SKETCHUP_SDK_DIR).wstring());
#endif
  return out;
}

const Api* api(std::string* why) {
  static Api a;
  static std::string error;
  static bool ok = false;
  static std::once_flag once;
  std::call_once(once, [] {
    HMODULE module = nullptr;
    for (const std::wstring& folder : dllFolders()) {
      const std::wstring dll = folder + L"\\LayOutAPI.dll";
      if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
      module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
      if (module) break;
      error = "LayOutAPI.dll was found but could not be loaded (Windows error " + std::to_string(GetLastError()) + ")";
    }
    if (!module) {
      if (error.empty()) error = "The LayOut part of the SketchUp SDK was not found (binaries\\layout\\x64)";
      return;
    }
    bool all = true;
#define SBR_LO_LOAD(f)                                                                             \
  a.f = reinterpret_cast<decltype(a.f)>(reinterpret_cast<void*>(GetProcAddress(module, #f)));      \
  if (!a.f && all) {                                                                               \
    all = false;                                                                                   \
    error = std::string("The LayOut SDK on this PC has no ") + #f + " (too old a version)";        \
  }
    SBR_LO_FUNCTIONS(SBR_LO_LOAD)
#undef SBR_LO_LOAD
    if (!all) return;
    a.LOInitialize();
    ok = true;
  });
  if (!ok) {
    if (why) *why = error;
    return nullptr;
  }
  return &a;
}

constexpr double kInch = 25.4;
constexpr double kPointsPerMm = 72.0 / 25.4;

// LayOut's DLLs write notes of their own (graphics cards...) to the standard output, where the CLI
// and the MCP server answer in JSON: while they run, the standard output goes to the standard
// error (both the C runtime's and Windows' handle), and comes back after.
struct QuietStdout {
  int saved = -1;
  HANDLE savedHandle = nullptr;
  QuietStdout() {
    std::fflush(stdout);
    saved = _dup(1);
    if (saved >= 0) _dup2(2, 1);
    savedHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
  }
  ~QuietStdout() {
    std::fflush(stdout);
    if (saved >= 0) {
      _dup2(saved, 1);
      _close(saved);
    }
    SetStdHandle(STD_OUTPUT_HANDLE, savedHandle);
  }
};

}  // namespace

bool layoutAvailable(std::string* why) {
  QuietStdout quiet;
  return api(why) != nullptr;
}

Result<int> layoutPageCount(const std::string& path) {
  QuietStdout quiet;
  std::string why;
  const Api* loaded = api(&why);
  if (!loaded) return Result<int>::failure(why);
  const Api& lo = *loaded;
  LODocumentRef document = SU_INVALID;
  if (lo.LODocumentCreateFromFile(&document, path.c_str()) != SU_ERROR_NONE)
    return Result<int>::failure("The LayOut file could not be read: " + path);
  LOPageListRef pages = SU_INVALID;
  lo.LOPageListCreate(&pages);
  lo.LODocumentGetPages(document, pages);
  size_t count = 0;
  lo.LOPageListGetNumberOfPages(pages, &count);
  lo.LOPageListRelease(&pages);
  lo.LODocumentRelease(&document);
  return static_cast<int>(count);
}

Status layoutToPdf(const std::string& layout, const std::string& pdf) {
  QuietStdout quiet;
  std::string why;
  const Api* loaded = api(&why);
  if (!loaded) return Status::failure(why);
  const Api& lo = *loaded;
  LODocumentRef document = SU_INVALID;
  if (lo.LODocumentCreateFromFile(&document, layout.c_str()) != SU_ERROR_NONE) return Status::failure("The LayOut file could not be read: " + layout);
  const SUResult r = lo.LODocumentExportToPDF(document, pdf.c_str(), LODictionaryRef SU_INVALID);
  lo.LODocumentRelease(&document);
  if (r != SU_ERROR_NONE) return Status::failure("LayOut could not write the PDF (error " + std::to_string(r) + ")");
  return Status::success();
}

Status writeLayout(const std::vector<Sheet>& sheets, const std::vector<std::string>& names, const std::string& path) {
  QuietStdout quiet;
  std::string why;
  const Api* loaded = api(&why);
  if (!loaded) return Status::failure(why);
  const Api& lo = *loaded;
  if (sheets.empty()) return Status::failure("Nothing to write");
  LODocumentRef document = SU_INVALID;
  if (lo.LODocumentCreateEmpty(&document) != SU_ERROR_NONE) return Status::failure("LayOut could not make a document");
  struct Release {
    const Api& lo;
    LODocumentRef& document;
    ~Release() { lo.LODocumentRelease(&document); }
  } release{lo, document};
  // One page size for the document: the largest paper of the sheets (LayOut has one per document).
  double width = 0, height = 0;
  for (const Sheet& s : sheets) {
    width = std::max(width, s.paperWidth);
    height = std::max(height, s.paperHeight);
  }
  LOPageInfoRef info = SU_INVALID;
  lo.LODocumentGetPageInfo(document, &info);
  lo.LOPageInfoSetWidth(info, width / kInch);
  lo.LOPageInfoSetHeight(info, height / kInch);
  // A style per pen: its width (points) and line type; fills grey without a stroke.
  LOStyleRef styles[kPenCount];
  for (int p = 0; p < kPenCount; ++p) {
    styles[p] = SU_INVALID;
    lo.LOStyleCreate(&styles[p]);
    const Pen pen = static_cast<Pen>(p);
    const SUColor black{0, 0, 0, 255};
    if (pen == Pen::Fill) {
      lo.LOStyleSetStroked(styles[p], false);
      lo.LOStyleSetSolidFilled(styles[p], true);
      lo.LOStyleSetSolidFillColor(styles[p], SUColor{200, 200, 200, 255});
      continue;
    }
    lo.LOStyleSetStroked(styles[p], true);
    lo.LOStyleSetSolidFilled(styles[p], false);
    lo.LOStyleSetStrokeColor(styles[p], black);
    lo.LOStyleSetStrokeWidth(styles[p], std::max(0.1, penWidth(pen) * kPointsPerMm));
    const auto dashes = penDashes(pen);
    lo.LOStyleSetStrokePattern(styles[p], dashes.empty() ? LOStrokePattern_Solid
                                          : dashes.size() > 2 ? LOStrokePattern_DashDot
                                                              : LOStrokePattern_Dash);
  }
  struct Styles {
    const Api& lo;
    LOStyleRef* s;
    ~Styles() {
      for (int p = 0; p < kPenCount; ++p) lo.LOStyleRelease(&s[p]);
    }
  } freeStyles{lo, styles};

  LOPageListRef pages = SU_INVALID;
  lo.LOPageListCreate(&pages);
  struct Pages {
    const Api& lo;
    LOPageListRef& p;
    ~Pages() { lo.LOPageListRelease(&p); }
  } freePages{lo, pages};
  int written = 0;
  for (std::size_t si = 0; si < sheets.size(); ++si) {
    const Sheet& sheet = sheets[si];
    // The empty document has one page: use it for the first sheet, add one for each other.
    if (si > 0) {
      LOPageRef added = SU_INVALID;
      lo.LODocumentAddPage(document, &added);
    }
    // A fresh list each time (getting the pages into a list adds to what it holds).
    lo.LOPageListRelease(&pages);
    lo.LOPageListCreate(&pages);
    lo.LODocumentGetPages(document, pages);
    size_t count = 0;
    lo.LOPageListGetNumberOfPages(pages, &count);
    if (count != si + 1) return Status::failure("LayOut did not add a page");
    const size_t pageIndex = si;
    LOPageRef page = SU_INVALID;
    lo.LOPageListGetPageAtIndex(pages, pageIndex, &page);
    const std::string name = si < names.size() && !names[si].empty() ? names[si] : sheet.title;
    if (!name.empty()) lo.LOPageSetName(page, name.c_str());
    // Sheet coordinates are real mm at the scale, from the paper's lower left; LayOut's are
    // inches on the page from its upper left (y down).
    const double s = sheet.scale > 0 ? sheet.scale : 1.0;
    auto at = [&](const gp_Pnt2d& p) { return LOPoint2D{p.X() / s / kInch, (height - p.Y() / s) / kInch}; };
    auto add = [&](LOEntityRef entity, Pen pen) {
      lo.LOEntitySetStyle(entity, styles[static_cast<int>(pen)]);
      return lo.LODocumentAddEntityUsingIndexes(document, entity, 0, pageIndex) == SU_ERROR_NONE;
    };
    for (const Item& item : sheet.items) {
      switch (item.kind) {
        case Item::Kind::Line:
        case Item::Kind::Polyline:
        case Item::Kind::Fill: {
          if (item.points.size() < 2) break;
          const LOPoint2D a = at(item.points[0]), b = at(item.points[1]);
          LOPathRef p = SU_INVALID;
          if (lo.LOPathCreate(&p, &a, &b) != SU_ERROR_NONE) break;
          for (std::size_t k = 2; k < item.points.size(); ++k) {
            const LOPoint2D q = at(item.points[k]);
            lo.LOPathAppendLineTo(p, &q);
          }
          if (item.closed || item.kind == Item::Kind::Fill) lo.LOPathClose(p);
          if (add(lo.LOPathToEntity(p), item.kind == Item::Kind::Fill ? Pen::Fill : item.pen)) ++written;
          lo.LOPathRelease(&p);
          break;
        }
        case Item::Kind::Arc: {
          if (item.radius <= 0 || std::abs(item.sweep) < 1e-9) break;
          // y flips: an anticlockwise arc on the sheet is clockwise on the page.
          const LOPoint2D c = at(item.centre);
          const double r = item.radius / s / kInch;
          const double a0 = -(item.start + item.sweep), a1 = -item.start;
          LOPathRef p = SU_INVALID;
          if (lo.LOPathCreateArc(&p, &c, r, std::min(a0, a1), std::max(a0, a1)) != SU_ERROR_NONE) break;
          if (add(lo.LOPathToEntity(p), item.pen)) ++written;
          lo.LOPathRelease(&p);
          break;
        }
        case Item::Kind::Text: {
          if (item.points.empty() || item.text.empty()) break;
          const LOPoint2D p = at(item.points[0]);
          static const LOFormattedTextAnchorType anchors[3][3] = {
              {LOFormattedTextAnchorType_BottomLeft, LOFormattedTextAnchorType_CenterLeft, LOFormattedTextAnchorType_TopLeft},
              {LOFormattedTextAnchorType_BottomCenter, LOFormattedTextAnchorType_CenterCenter, LOFormattedTextAnchorType_TopCenter},
              {LOFormattedTextAnchorType_BottomRight, LOFormattedTextAnchorType_CenterRight, LOFormattedTextAnchorType_TopRight}};
          const int h = std::clamp(item.hAlign, 0, 2), v = std::clamp(item.vAlign, 0, 2);
          LOFormattedTextRef t = SU_INVALID;
          if (lo.LOFormattedTextCreateAtPoint(&t, &p, anchors[h][v], item.text.c_str()) != SU_ERROR_NONE) break;
          // Its own style: the pen's, with the font size of its height on paper.
          LOStyleRef style = SU_INVALID;
          lo.LOStyleCreate(&style);
          lo.LOStyleSetFontSize(style, std::max(1.0, item.height * kPointsPerMm * 1.4));  // cap height -> font size
          const LOEntityRef entity = lo.LOFormattedTextToEntity(t);
          lo.LOEntitySetStyle(entity, style);
          if (lo.LODocumentAddEntityUsingIndexes(document, entity, 0, pageIndex) == SU_ERROR_NONE) ++written;
          lo.LOStyleRelease(&style);
          lo.LOFormattedTextRelease(&t);
          break;
        }
      }
    }
  }
  if (written == 0) return Status::failure("Nothing to write");
  const SUResult saved = lo.LODocumentSaveToFile(document, path.c_str(), LODocumentVersion_Current);
  if (saved != SU_ERROR_NONE) return Status::failure("The LayOut file could not be written: " + path + " (error " + std::to_string(saved) + ")");
  return Status::success();
}

#endif

}  // namespace sbr::draw
