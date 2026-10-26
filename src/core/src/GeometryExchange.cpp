#include "sbr/geom/ShapeIO.h"

#include <filesystem>
#include <fstream>
#include <locale>
#include <mutex>
#include <type_traits>
#include <iostream>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <IGESControl_Controller.hxx>
#include <IGESControl_Reader.hxx>
#include <IGESControl_Writer.hxx>
#include <Interface_Static.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_PrinterOStream.hxx>
#include <Standard_CLocaleSentry.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>

namespace sbr::geom {
namespace {
bool hasGeometry(const TopoDS_Shape& shape) {
  return !shape.IsNull() && (TopExp_Explorer(shape,TopAbs_VERTEX).More() ||
      TopExp_Explorer(shape,TopAbs_EDGE).More() || TopExp_Explorer(shape,TopAbs_FACE).More());
}
std::mutex exchangeMutex;
// OCCT's IGES reader emits diagnostics through the global messenger. Keep
// stdout as JSON for CLI/MCP, and restore the application's printers after I/O.
struct ExchangeDiagnostics {
  Handle(Message_Messenger) messenger=Message::DefaultMessenger();
  std::decay_t<decltype(Message::DefaultMessenger()->Printers())> saved=messenger->Printers();
  std::streambuf* stdoutBuffer=nullptr;
  ExchangeDiagnostics() {
    Handle(Message_PrinterOStream) printer=new Message_PrinterOStream("cerr",Standard_True,Message_Warning);
    messenger->ChangePrinters().Clear();
    printer->SetToColorize(Standard_False);messenger->AddPrinter(printer);
    stdoutBuffer=std::cout.rdbuf(std::cerr.rdbuf());
  }
  ~ExchangeDiagnostics(){std::cout.rdbuf(stdoutBuffer);messenger->ChangePrinters()=saved;}
};
}
Status writeBrep(const TopoDS_Shape& shape,const std::string& path) {
  if(!hasGeometry(shape))return Status::failure("Nothing to export");
  Standard_CLocaleSentry locale;
  try {
    if(!BRepCheck_Analyzer(shape).IsValid())return Status::failure("Cannot export invalid BRep geometry");
    std::ofstream file(std::filesystem::u8path(path),std::ios::binary|std::ios::trunc);
    if(!file)return Status::failure("Could not write BRep file: "+path);
    file.imbue(std::locale::classic());
    BRepTools::Write(shape,file,Standard_False,Standard_False,TopTools_FormatVersion_CURRENT);
    file.close();
    return file ? Status::success() : Status::failure("Could not finish writing BRep file: "+path);
  }catch(const Standard_Failure& e){return Status::failure(std::string("BRep export failed: ")+e.GetMessageString());}
   catch(const std::exception& e){return Status::failure(std::string("BRep export failed: ")+e.what());}
}
Result<TopoDS_Shape> readBrep(const std::string& path) {
  using R=Result<TopoDS_Shape>;
  std::lock_guard<std::mutex> guard(exchangeMutex);Standard_CLocaleSentry locale;ExchangeDiagnostics diagnostics;
  try {
    std::ifstream file(std::filesystem::u8path(path),std::ios::binary);
    if(!file)return R::failure("Cannot open BRep file: "+path);
    file.imbue(std::locale::classic());TopoDS_Shape shape;BRep_Builder builder;
    BRepTools::Read(shape,file,builder);
    if(file.fail() || !hasGeometry(shape))return R::failure("Empty or unreadable BRep file");
    BRepTools::Clean(shape);
    if(!BRepCheck_Analyzer(shape).IsValid())return R::failure("Invalid BRep geometry");
    return shape;
  }catch(const Standard_Failure& e){return R::failure(std::string("BRep import failed: ")+e.GetMessageString());}
   catch(const std::exception& e){return R::failure(std::string("BRep import failed: ")+e.what());}
}
Status writeIges(const TopoDS_Shape& shape,const std::string& path) {
  if(!hasGeometry(shape))return Status::failure("Nothing to export");
  std::lock_guard<std::mutex> guard(exchangeMutex);Standard_CLocaleSentry locale;ExchangeDiagnostics diagnostics;
  try {
    if(!BRepCheck_Analyzer(shape).IsValid())return Status::failure("Cannot export invalid IGES geometry");
    IGESControl_Controller::Init();Interface_Static::SetCVal("xstep.cascade.unit","MM");
    IGESControl_Writer writer("MM",1);
    const auto copy=BRepBuilderAPI_Copy(shape,Standard_True,Standard_False).Shape();
    if(!writer.AddShape(copy))return Status::failure("IGES geometry transfer failed");
    std::ofstream file(std::filesystem::u8path(path),std::ios::binary|std::ios::trunc);
    if(!file)return Status::failure("Could not write IGES file: "+path);
    file.imbue(std::locale::classic());const bool written=writer.Write(file);file.close();
    return written&&file ? Status::success() : Status::failure("Could not finish writing IGES file: "+path);
  }catch(const Standard_Failure& e){return Status::failure(std::string("IGES export failed: ")+e.GetMessageString());}
   catch(const std::exception& e){return Status::failure(std::string("IGES export failed: ")+e.what());}
}
Result<std::vector<NamedShape>> readIges(const std::string& path) {
  using R=Result<std::vector<NamedShape>>;
  std::lock_guard<std::mutex> guard(exchangeMutex);Standard_CLocaleSentry locale;ExchangeDiagnostics diagnostics;
  try {
    IGESControl_Controller::Init();Interface_Static::SetCVal("xstep.cascade.unit","MM");
    IGESControl_Reader reader;reader.SetReadVisible(Standard_True);
    if(reader.ReadFile(path.c_str())!=IFSelect_RetDone)return R::failure("Cannot read IGES file: "+path);
    const int roots=reader.NbRootsForTransfer();const int transferred=reader.TransferRoots();
    if(roots==0 || transferred==0 || reader.NbShapes()==0)return R::failure("No supported geometry in IGES file");
    if(transferred!=roots)return R::failure("IGES transfer was incomplete; nothing imported");
    std::vector<NamedShape> parts;
    for(int i=1;i<=reader.NbShapes();++i) {
      auto shape=reader.Shape(i);
      if(!hasGeometry(shape) || !BRepCheck_Analyzer(shape).IsValid())return R::failure("Invalid IGES geometry; nothing imported");
      BRepTools::Clean(shape);
      parts.push_back({shape,"IGES "+std::to_string(i),std::nullopt,{}});
    }
    return parts;
  }catch(const Standard_Failure& e){return R::failure(std::string("IGES import failed: ")+e.GetMessageString());}
   catch(const std::exception& e){return R::failure(std::string("IGES import failed: ")+e.what());}
}
} // namespace sbr::geom
