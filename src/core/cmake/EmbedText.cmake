# Writes INPUT (text) into OUTPUT as a C++ array of string pieces (each well
# under the MSVC limit for one literal), joined at run time:
#   namespace sbr::embedded { extern const char* const NAME[]; extern const unsigned NAME_parts; }
file(READ "${INPUT}" content)
string(LENGTH "${content}" length)
set(piece 8000)
file(WRITE "${OUTPUT}" "// Generated from ${INPUT}; do not edit.\nnamespace sbr::embedded {\nextern const char* const ${NAME}[];\nextern const unsigned ${NAME}_parts;\nconst char* const ${NAME}[] = {\n")
set(at 0)
set(count 0)
while(at LESS length)
  string(SUBSTRING "${content}" ${at} ${piece} part)
  file(APPEND "${OUTPUT}" "R\"SBR(${part})SBR\",\n")
  math(EXPR at "${at} + ${piece}")
  math(EXPR count "${count} + 1")
endwhile()
file(APPEND "${OUTPUT}" "};\nconst unsigned ${NAME}_parts = ${count};\n}  // namespace sbr::embedded\n")
