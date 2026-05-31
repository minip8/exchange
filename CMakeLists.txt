cmake_minimum_required(VERSION 3.28)
project(exchange)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Create a virtual target for common compiler options
add_library(project_options INTERFACE)
target_compile_options(project_options INTERFACE
    $<$<CONFIG:Debug>:
        -Og -g3 -ggdb -Wall -Wextra
        -Wpedantic -Wshadow
        -Wconversion -Werror
        -Wpedantic -fsanitize=address
        -fsanitize=undefined
        -fsanitize=leak
        -D_GLIBCXX_DEBUG
    >
)
target_link_options(project_options INTERFACE
    $<$<CONFIG:Debug>:
        -fsanitize=address
        -fsanitize=undefined
        -fsanitize=leak
    >
)

# OrderBook
add_library(engine src/engine/OrderBook.cpp)

target_sources(engine
    PUBLIC
    FILE_SET CXX_MODULES FILES
        modules/engine/OrderBook.cppm
)

target_link_libraries(engine PUBLIC project_options)

# Create the executable
add_executable(app src/main.cpp)

# Link the executable with your engine library and the compiler options
target_link_libraries(app PRIVATE engine project_options)
