# SwiftNet test suite (doctest + ctest)

add_executable(swiftnet_unit_tests test_units.cpp)
target_link_libraries(swiftnet_unit_tests
    PRIVATE
        swiftnet
        doctest::doctest
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)
target_include_directories(swiftnet_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME unit COMMAND swiftnet_unit_tests)

add_executable(swiftnet_integration_tests test_integration.cpp)
target_link_libraries(swiftnet_integration_tests
    PRIVATE
        swiftnet
        doctest::doctest
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)
target_include_directories(swiftnet_integration_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME integration COMMAND swiftnet_integration_tests)
set_tests_properties(integration PROPERTIES TIMEOUT 60)
