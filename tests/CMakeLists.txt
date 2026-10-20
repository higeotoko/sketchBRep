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
)
target_link_libraries(sbr_core_tests PRIVATE sbr::core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(sbr_core_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

if(SBR_BUILD_TOOLS)
  add_test(NAME phase0_cli
           COMMAND sbr_phase0 1000 2000 3000 ${CMAKE_CURRENT_BINARY_DIR}/phase0_box.step)
  set_tests_properties(phase0_cli PROPERTIES PASS_REGULAR_EXPRESSION "PHASE 0: PASS")
endif()

# End-to-end GUI test (needs a display; run under xvfb-run on headless CI).
if(SBR_BUILD_APP)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  add_executable(sbr_gui_tests test_gui.cpp)
  set_target_properties(sbr_gui_tests PROPERTIES AUTOMOC ON)
  target_link_libraries(sbr_gui_tests PRIVATE sbr_ui Qt6::Test)
  add_test(NAME gui_phase1 COMMAND sbr_gui_tests)
  set_tests_properties(gui_phase1 PROPERTIES LABELS gui ENVIRONMENT "QT_QPA_PLATFORM=xcb")
endif()
