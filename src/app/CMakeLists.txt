find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Network)

set(CMAKE_AUTOMOC ON)

# UI code as a library so the GUI tests can drive the real main window.
add_library(sbr_ui STATIC
  MainWindow.cpp MainWindow.h
  PropertiesPanel.cpp PropertiesPanel.h
  MaterialsPanel.cpp MaterialsPanel.h
  Tools.cpp Tools.h
  Viewport.cpp Viewport.h
  Cursors.cpp Cursors.h
  AiBridge.cpp AiBridge.h
  Messages.cpp Messages.h
  ProgressDialog.cpp ProgressDialog.h
)
target_include_directories(sbr_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sbr_ui PUBLIC sbr::core Qt6::Widgets Qt6::Network TKService TKV3d TKOpenGl
                             PRIVATE nlohmann_json::nlohmann_json)

add_executable(SketchBRep WIN32 main.cpp)
target_compile_definitions(SketchBRep PRIVATE SBR_VERSION="${PROJECT_VERSION}")
target_link_libraries(SketchBRep PRIVATE sbr_ui)
