find_package(GTest REQUIRED)

add_executable(sbr_core_tests
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
  test_ux_gate1.cpp
  test_rhino3dm.cpp
  test_scenarios.cpp
)
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
  add_executable(sbr_gui_tests test_gui.cpp ${PROJECT_SOURCE_DIR}/src/app/branding.qrc)
  set_target_properties(sbr_gui_tests PROPERTIES AUTOMOC ON AUTORCC ON)
  target_link_libraries(sbr_gui_tests PRIVATE sbr_ui Qt6::Test nlohmann_json::nlohmann_json)
  target_compile_definitions(sbr_gui_tests PRIVATE SBR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  add_test(NAME gui_phase1 COMMAND sbr_gui_tests)
  set_tests_properties(gui_phase1 PROPERTIES LABELS gui ENVIRONMENT "QT_QPA_PLATFORM=xcb")
endif()
