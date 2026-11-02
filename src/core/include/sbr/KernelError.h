#pragma once
// The message of a geometry kernel (OCCT) failure, never null: some
// failures carry no message, and building a std::string from a null
// pointer crashes the program (a failed push / pull took SketchBRep down).

#include <string>

#include <Standard_Failure.hxx>

namespace sbr {

inline std::string kernelMessage(const Standard_Failure& e) {
  const char* m = e.GetMessageString();
  if (m && *m) return m;
  const char* type = e.DynamicType().IsNull() ? nullptr : e.DynamicType()->Name();
  return type && *type ? std::string(type) : std::string("geometry kernel error");
}

}  // namespace sbr
