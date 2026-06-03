# README front-door examples (built in CI so they can never go stale).
foreach(ex hello typed_json)
    add_executable(${ex} ${ex}.cpp)
    target_link_libraries(${ex} PRIVATE swiftnet nlohmann_json::nlohmann_json spdlog::spdlog)
    target_include_directories(${ex} PRIVATE ${CMAKE_SOURCE_DIR}/include)
endforeach()

# Basic HTTP server example
add_executable(basic_server basic_server.cpp)
target_link_libraries(basic_server
    PRIVATE
        swiftnet
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)
target_include_directories(basic_server
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

# File server example
add_executable(file_server file_server.cpp)
target_link_libraries(file_server 
    PRIVATE 
        swiftnet
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)
target_include_directories(file_server 
    PRIVATE 
        ${CMAKE_SOURCE_DIR}/include
)

# REST API example
add_executable(rest_api rest_api.cpp)
target_link_libraries(rest_api 
    PRIVATE 
        swiftnet
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)
target_include_directories(rest_api 
    PRIVATE 
        ${CMAKE_SOURCE_DIR}/include
)

# Simple test for virtual threads
add_executable(simple_test simple_test.cpp)
target_link_libraries(simple_test 
    PRIVATE 
        swiftnet
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)
target_include_directories(simple_test 
    PRIVATE 
        ${CMAKE_SOURCE_DIR}/include
)

# Performance test for virtual threads
add_executable(performance_test performance_test.cpp)
target_link_libraries(performance_test
        PRIVATE
            swiftnet
            nlohmann_json::nlohmann_json
            spdlog::spdlog
    )
target_include_directories(performance_test
        PRIVATE
            ${CMAKE_SOURCE_DIR}/include
    )

# Async-handler demo (co_await inside a route handler)
add_executable(async_demo async_demo.cpp)
target_link_libraries(async_demo
    PRIVATE
        swiftnet
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)
target_include_directories(async_demo
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

# WebSocket echo server
add_executable(websocket_echo websocket_echo.cpp)
target_link_libraries(websocket_echo
    PRIVATE
        swiftnet
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)
target_include_directories(websocket_echo
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)