add_executable(sbr_bench main.cpp)
target_link_libraries(sbr_bench PRIVATE sbr::core nlohmann_json::nlohmann_json)
