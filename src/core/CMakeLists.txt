add_library(sbr_core STATIC
  src/Units.cpp
  src/Profile.cpp
  src/Operations.cpp
  src/Arrangement.cpp
  src/Validation.cpp
  src/Measure.cpp
  src/ShapeIO.cpp
  src/Stl.cpp
  src/FaceRef.cpp
  src/DisplayMesh.cpp
  src/Recipe.cpp
  src/Object.cpp
  src/Document.cpp
  src/Material.cpp
  src/Codec.cpp
  src/ChangeSummary.cpp
  src/CommandStack.cpp
  src/NativeFormat.cpp
  src/Modeler.cpp
  src/CurveJunctions.cpp
  src/SelectedFace.cpp
  src/Inference.cpp
  src/Freeform.cpp
  src/CommandApi.cpp
  src/McpServer.cpp
  src/Annotation.cpp
  src/Dxf.cpp
  src/Rhino3dm.cpp
  src/Progress.cpp
)
add_library(sbr::core ALIAS sbr_core)

target_include_directories(sbr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# OCCT headers are third party: keep their warnings out of our build output.
target_include_directories(sbr_core SYSTEM PUBLIC ${OpenCASCADE_INCLUDE_DIR})

# OCCT 7.8 merged the STEP toolkits into TKDESTEP; 7.6/7.7 use the old split.
if(TARGET TKDESTEP)
  set(SBR_OCCT_EXCHANGE TKDE TKXSBase TKDESTEP)
else()
  set(SBR_OCCT_EXCHANGE TKXSBase TKSTEP TKSTEPBase TKSTEPAttr TKSTEP209 TKXDESTEP)
endif()

target_link_libraries(sbr_core
  PUBLIC
    TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep
    TKGeomAlgo TKTopAlgo TKPrim TKBO TKBool TKShHealing TKMesh TKOffset TKFeat
    ${SBR_OCCT_EXCHANGE} TKXCAF TKLCAF TKCAF TKVCAF TKService
  PRIVATE
    nlohmann_json::nlohmann_json
)

if(SBR_WITH_OPENNURBS)
  target_link_libraries(sbr_core PRIVATE opennurbsStatic)
  target_compile_definitions(sbr_core PRIVATE SBR_WITH_OPENNURBS ON_CMAKE_BUILD)
endif()
