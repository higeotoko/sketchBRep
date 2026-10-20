find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)

# UI code as a library so the GUI tests can drive the real main window.
add_library(sbr_ui STATIC
  MainWindow.cpp MainWindow.h
  PropertiesPanel.cpp PropertiesPanel.h
  Tools.cpp Tools.h
  Viewport.cpp Viewport.h
)
target_include_directories(sbr_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sbr_ui PUBLIC sbr::core Qt6::Widgets TKService TKV3d TKOpenGl)

add_executable(SketchBRep WIN32 main.cpp)
target_compile_definitions(SketchBRep PRIVATE SBR_VERSION="${PROJECT_VERSION}")
target_link_libraries(SketchBRep PRIVATE sbr_ui)
