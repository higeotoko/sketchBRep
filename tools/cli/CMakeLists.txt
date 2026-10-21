add_executable(sbr_cli main.cpp)
target_link_libraries(sbr_cli PRIVATE sbr::core nlohmann_json::nlohmann_json)
