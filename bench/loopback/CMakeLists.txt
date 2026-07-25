# Latency benchmark for the networking layer.
#
# RELEASE ONLY, and deliberately not part of the default build: net_io links
# `engine`, which propagates debug_options, so a Debug build of this is
# sanitized and every number it prints is meaningless.
if(TARGET net_io)
  add_executable(net_loopback_bench EXCLUDE_FROM_ALL net_loopback_bench.cpp)
  target_link_libraries(net_loopback_bench PRIVATE net_io)
  target_compile_options(net_loopback_bench PRIVATE -O3)
else()
  message(STATUS
    "net_loopback_bench needs EXCHANGE_BUILD_NET=ON (skipping)")
endif()
