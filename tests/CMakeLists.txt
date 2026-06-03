# SwiftNet test suite (doctest + ctest)

# Helper: one doctest executable + ctest registration per source file.
function(swiftnet_add_test test_name src)
    add_executable(${test_name} ${src})
    target_link_libraries(${test_name}
        PRIVATE
            swiftnet
            doctest::doctest
            nlohmann_json::nlohmann_json
            spdlog::spdlog
    )
    target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME ${ARGV2} COMMAND ${test_name})
endfunction()

swiftnet_add_test(swiftnet_unit_tests test_units.cpp unit)
swiftnet_add_test(swiftnet_detect_tests test_detect.cpp detect)
swiftnet_add_test(swiftnet_config_tests test_config.cpp config)
swiftnet_add_test(swiftnet_simd_tests test_simd.cpp simd)
swiftnet_add_test(swiftnet_glaze_tests test_glaze.cpp glaze)
swiftnet_add_test(swiftnet_validate_tests test_validate.cpp validate)

swiftnet_add_test(swiftnet_integration_tests test_integration.cpp integration)
set_tests_properties(integration PROPERTIES TIMEOUT 60)
