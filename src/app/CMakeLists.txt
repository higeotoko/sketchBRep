find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Network)

set(CMAKE_AUTOMOC ON)

# UI code as a library so the GUI tests can drive the real main window.
add_library(sbr_ui STATIC
  MainWindow.cpp MainWindow.h
  DwgConverter.cpp DwgConverter.h
  PropertiesPanel.cpp PropertiesPanel.h
  MaterialsPanel.cpp MaterialsPanel.h
  MaterialEditor.cpp MaterialEditor.h
  Tools.cpp Tools.h
  Viewport.cpp Viewport.h
  WheelZoom.cpp WheelZoom.h
  Cursors.cpp Cursors.h
  ToolPalette.cpp ToolPalette.h
  AiBridge.cpp AiBridge.h
  McpHttpServer.cpp McpHttpServer.h
  Messages.cpp Messages.h
  ProgressDialog.cpp ProgressDialog.h
)
target_include_directories(sbr_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                          PRIVATE ${PROJECT_SOURCE_DIR}/assets/materials/builtin/integration)
target_link_libraries(sbr_ui PUBLIC sbr::core Qt6::Widgets Qt6::Network TKService TKV3d TKOpenGl
                             PRIVATE nlohmann_json::nlohmann_json)

# The logo (assets/branding): window icon and About box as Qt resources;
# on Windows also the executable's own icon and version information.
set(SBR_APP_RESOURCES branding.qrc)
# The pictures of the ready-made materials (assets/materials/builtin), in the
# executable: added materials embed them in the document from there.
list(APPEND SBR_APP_RESOURCES ${PROJECT_SOURCE_DIR}/assets/materials/builtin/integration/materials.qrc)
if(WIN32)
  set(SBR_ICON_FILE "${PROJECT_SOURCE_DIR}/assets/branding/05-windows/sketchbrep.ico")
  file(TO_NATIVE_PATH "${SBR_ICON_FILE}" SBR_ICON_FILE)
  string(REPLACE "\\" "\\\\" SBR_ICON_FILE "${SBR_ICON_FILE}")
  configure_file(sketchbrep.rc.in ${CMAKE_CURRENT_BINARY_DIR}/sketchbrep.rc @ONLY)
  list(APPEND SBR_APP_RESOURCES ${CMAKE_CURRENT_BINARY_DIR}/sketchbrep.rc)
endif()
add_executable(SketchBRep WIN32 main.cpp ${SBR_APP_RESOURCES})
set_target_properties(SketchBRep PROPERTIES AUTORCC ON)
target_compile_definitions(SketchBRep PRIVATE SBR_VERSION="${PROJECT_VERSION}")
target_link_libraries(SketchBRep PRIVATE sbr_ui)
