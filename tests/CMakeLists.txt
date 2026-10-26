find_package(GTest REQUIRED)

add_executable(sbr_core_tests
  test_stl.cpp
  test_dxf.cpp
  test_units.cpp
  test_geometry.cpp
  test_document.cpp
  test_native_format.cpp
  test_inference.cpp
  test_display.cpp
  test_phase2.cpp
  test_freeform.cpp
  test_api.cpp
  test_transactions.cpp
  test_materials.cpp
  test_components.cpp
  test_reshape.cpp
  test_delete.cpp
  test_followme_edges.cpp
  test_arrays.cpp
  test_open_pushpull.cpp
  test_stretch.cpp
  test_overlaps.cpp
  test_flows.cpp
  test_split_across.cpp
  test_closing_faces.cpp
  test_follow_me.cpp
  test_ux_gate1.cpp
  test_rhino3dm.cpp
  test_scenarios.cpp
)
set_source_files_properties(test_dxf.cpp PROPERTIES
  COMPILE_DEFINITIONS SBR_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_link_libraries(sbr_core_tests PRIVATE sbr::core nlohmann_json::nlohmann_json GTest::gtest_main)
if(SBR_WITH_OPENNURBS)
  # The 3DM import report tests write their fixture with openNURBS itself.
  target_sources(sbr_core_tests PRIVATE test_rhino3dm_report.cpp)
  target_link_libraries(sbr_core_tests PRIVATE opennurbsStatic)
  target_compile_definitions(sbr_core_tests PRIVATE ON_CMAKE_BUILD)
endif()

include(GoogleTest)
# Listing the tests starts the executable; on Windows runners loading the
# OCCT / Qt DLLs can exceed the 5 s default, so allow more time.
gtest_discover_tests(sbr_core_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} DISCOVERY_TIMEOUT 120)

if(SBR_BUILD_TOOLS)
  add_test(NAME phase0_cli
           COMMAND sbr_phase0 1000 2000 3000 ${CMAKE_CURRENT_BINARY_DIR}/phase0_box.step)
  set_tests_properties(phase0_cli PROPERTIES PASS_REGULAR_EXPRESSION "PHASE 0: PASS")

  # Headless scripting: 1 slab + 4 columns, saved and re-opened.
  add_test(NAME cli_script
           COMMAND sbr_cli --save ${CMAKE_CURRENT_BINARY_DIR}/cli_columns.sbr ${CMAKE_CURRENT_SOURCE_DIR}/data/columns.jsonl)
  add_test(NAME cli_reopen
           COMMAND ${CMAKE_COMMAND} -DCOMMAND=$<TARGET_FILE:sbr_cli>|--open|${CMAKE_CURRENT_BINARY_DIR}/cli_columns.sbr
                   -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/data/list_objects.jsonl
                   "-DEXPECT=\"name\":\"C1\".*\"name\":\"Slab\"|\"name\":\"Slab\".*\"name\":\"C1\""
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/RunWithInput.cmake)
  set_tests_properties(cli_reopen PROPERTIES DEPENDS cli_script)
  # MCP over stdio (headless): initialize, list tools, call a tool.
  add_test(NAME mcp_headless_cli
           COMMAND ${CMAKE_COMMAND} -DCOMMAND=$<TARGET_FILE:sbr_cli>|--mcp
                   -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/data/mcp_session.jsonl
                   "-DEXPECT=\"protocolVersion\":\"2025-06-18\".*create_box.*\"isError\":false"
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/RunWithInput.cmake)
  if(TARGET sbr_mcp)
    add_test(NAME mcp_headless
             COMMAND ${CMAKE_COMMAND} -DCOMMAND=$<TARGET_FILE:sbr_mcp>|--headless
                     -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/data/mcp_session.jsonl
                     "-DEXPECT=\"protocolVersion\":\"2025-06-18\".*create_box.*\"isError\":false"
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/RunWithInput.cmake)
  endif()
endif()

# End-to-end GUI test (needs a display; run under xvfb-run on headless CI).
if(SBR_BUILD_APP)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  add_executable(sbr_stl_gui_tests test_stl_gui.cpp)
  set_target_properties(sbr_stl_gui_tests PROPERTIES AUTOMOC ON)
  target_link_libraries(sbr_stl_gui_tests PRIVATE sbr_ui Qt6::Test)
  add_test(NAME gui_stl_export COMMAND sbr_stl_gui_tests)
  set_tests_properties(gui_stl_export PROPERTIES LABELS gui ENVIRONMENT "QT_QPA_PLATFORM=xcb")
  add_executable(sbr_gui_tests test_gui.cpp ${PROJECT_SOURCE_DIR}/src/app/branding.qrc
                               ${PROJECT_SOURCE_DIR}/assets/materials/builtin/integration/materials.qrc)
  set_target_properties(sbr_gui_tests PROPERTIES AUTOMOC ON AUTORCC ON)
  target_link_libraries(sbr_gui_tests PRIVATE sbr_ui Qt6::Test nlohmann_json::nlohmann_json)
  target_compile_definitions(sbr_gui_tests PRIVATE SBR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  add_test(NAME gui_phase1 COMMAND sbr_gui_tests)
  set_tests_properties(gui_phase1 PROPERTIES LABELS gui ENVIRONMENT "QT_QPA_PLATFORM=xcb")
endif()

if(SBR_BUILD_APP)
  add_executable(sbr_fake_oda fake_oda.cpp)
  target_link_libraries(sbr_fake_oda PRIVATE Qt6::Core)
  add_executable(sbr_drawing_import_tests test_drawing_import.cpp)
  set_target_properties(sbr_drawing_import_tests PROPERTIES AUTOMOC ON)
  target_link_libraries(sbr_drawing_import_tests PRIVATE sbr_ui Qt6::Test)
  target_compile_definitions(sbr_drawing_import_tests PRIVATE SBR_FAKE_ODA="$<TARGET_FILE:sbr_fake_oda>")
  target_compile_definitions(sbr_drawing_import_tests PRIVATE SBR_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  add_dependencies(sbr_drawing_import_tests sbr_fake_oda)
  add_test(NAME gui_drawing_import COMMAND sbr_drawing_import_tests)
  set_tests_properties(gui_drawing_import PROPERTIES LABELS gui ENVIRONMENT "QT_QPA_PLATFORM=xcb")
endif()
