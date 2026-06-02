cmake_minimum_required(VERSION 4.3.3)
project(exchange)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Flags
set(SANITIZERS
    -fsanitize=address
    -fsanitize=undefined
    -fsanitize=leak
)

# Create a virtual target for common compiler options
add_library(debug_options INTERFACE)
target_compile_options(debug_options INTERFACE
    $<$<CONFIG:Debug>:
        -Og -g3 -ggdb -Wall -Wextra
        -Wpedantic -Wshadow
        -Wconversion
        ${SANITIZERS}
        -D_GLIBCXX_DEBUG
    >
)
target_link_options(debug_options INTERFACE
    $<$<CONFIG:Debug>:
        ${SANITIZERS}
    >
)

# Engine
add_library(engine
    src/engine/OrderBook.cpp
    src/engine/MatchingEngine.cpp
)

target_include_directories(engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(engine PUBLIC debug_options)

# Create the executable
add_executable(app src/main.cpp)

# Link the executable with your engine library and the compiler options
target_link_libraries(app PRIVATE engine debug_options)
