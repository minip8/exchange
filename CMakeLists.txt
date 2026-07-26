cmake_minimum_required(VERSION 4.3.3)
project(exchange)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Which sanitizer set the Debug config carries. ASan and TSan are mutually
# exclusive, and TSan is the only tool that can validate the SPSC rings'
# memory ordering — hence a selectable knob rather than a hardcoded list.
# One tree per value; see the `debug` and `tsan` configure presets.
set(EXCHANGE_SANITIZER "address" CACHE STRING
    "Sanitizer set for the Debug config: address | thread | none")
set_property(CACHE EXCHANGE_SANITIZER PROPERTY STRINGS address thread none)

option(EXCHANGE_BUILD_NET "Build the networking layer (needs Boost headers)" ON)

if(EXCHANGE_SANITIZER STREQUAL "address")
    set(SANITIZERS -fsanitize=address -fsanitize=undefined -fsanitize=leak)
elseif(EXCHANGE_SANITIZER STREQUAL "thread")
    # No UBSan here: -fsanitize=undefined and =thread coexist, but the pair
    # inflates an already slow build for no extra coverage of the rings.
    set(SANITIZERS -fsanitize=thread)
elseif(EXCHANGE_SANITIZER STREQUAL "none")
    set(SANITIZERS)
else()
    message(FATAL_ERROR
        "EXCHANGE_SANITIZER must be one of: address, thread, none "
        "(got '${EXCHANGE_SANITIZER}')")
endif()

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
    src/engine/Order.cpp
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

# Networking layer (gateway + I/O + server). Everything here links `engine`
# normally and so inherits debug_options — see src/net/CMakeLists.txt.
if(EXCHANGE_BUILD_NET)
    add_subdirectory(src/net)
endif()

# Benchmarking
add_subdirectory(bench)
