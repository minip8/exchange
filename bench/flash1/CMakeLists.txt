# Adapter .so for the flash1-dev/matching-engine-benchmark harness.
# Fetch and build the harness first: scripts/fetch_harness.sh
set(FLASH1_HARNESS_DIR "${CMAKE_SOURCE_DIR}/external/matching-engine-benchmark"
    CACHE PATH "Checkout of flash1-dev/matching-engine-benchmark")

if(EXISTS "${FLASH1_HARNESS_DIR}/api/matching_engine_api.h")
  # OrderBook.cpp is compiled directly into the .so instead of linking the
  # engine static lib: the lib is not PIC and PUBLIC-links debug_options,
  # whose Debug-config sanitizers/_GLIBCXX_DEBUG must not leak into the
  # adapter the harness dlopen()s.
  add_library(flash1_adapter SHARED
    adapter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/OrderBook.cpp
  )
  target_include_directories(flash1_adapter PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${FLASH1_HARNESS_DIR}/api
  )
  target_compile_options(flash1_adapter PRIVATE -O3 -march=native -fvisibility=hidden)
  set_target_properties(flash1_adapter PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    PREFIX ""
    OUTPUT_NAME "adapter"
  )
else()
  message(STATUS
    "flash1 harness not found at ${FLASH1_HARNESS_DIR}; "
    "run scripts/fetch_harness.sh (skipping flash1_adapter)")
endif()
