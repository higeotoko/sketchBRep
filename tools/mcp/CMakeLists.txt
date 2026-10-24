find_package(Qt6 6.2 REQUIRED COMPONENTS Network)
add_executable(sbr_mcp main.cpp)
target_link_libraries(sbr_mcp PRIVATE sbr::core nlohmann_json::nlohmann_json Qt6::Network)
