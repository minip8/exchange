# The networking layer measured on the flash1 workload (target:
# net_workload_bench).
#
# RELEASE ONLY, and deliberately not part of the default build: net_io links
# `engine`, which propagates debug_options, so a Debug build of this is
# sanitized and every number it prints is meaningless.
#
# Running it needs external/matching-engine-benchmark for the order stream, but
# BUILDING it does not — Flash1Workload redeclares the on-disk record rather
# than including a harness header, so this target is never skipped.
if(TARGET net_io)
  add_executable(net_workload_bench EXCLUDE_FROM_ALL
    net_workload_bench.cpp
    Flash1Workload.cpp)
  target_link_libraries(net_workload_bench PRIVATE net_io)
  target_include_directories(net_workload_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(net_workload_bench PRIVATE -O3)
else()
  message(STATUS
    "net_workload_bench needs EXCHANGE_BUILD_NET=ON (skipping)")
endif()
