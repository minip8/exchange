# Networking layer.
#
# Every target here is STATIC or INTERFACE — nothing is SHARED, so the fact
# that `engine` is built without -fPIC never becomes a problem.
#
# Note also what this deliberately does NOT do: it does not copy the flash1
# adapter's "compile OrderBook.cpp directly" trick. That is sound only for a
# self-contained .so. Here a net TU built without _GLIBCXX_DEBUG while
# `engine` has it would give std::vector<Fill> mismatched layouts across the
# link boundary — silent ODR corruption. We link `engine` normally and
# inherit its sanitizers in Debug, which is what development wants anyway.

# CONFIG mode is mandatory: CMake 4.4 removed FindBoost.cmake, so module-mode
# find_package(Boost) hard-fails. BoostConfig.cmake ships with the distro
# package at /usr/lib/x86_64-linux-gnu/cmake/Boost-1.83.0.
find_package(Boost 1.83 REQUIRED CONFIG COMPONENTS headers)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# net_protocol — header-only core + wire. No Boost, no threads, no engine.
# ---------------------------------------------------------------------------
add_library(net_protocol INTERFACE)
target_include_directories(net_protocol INTERFACE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(net_protocol INTERFACE debug_options)

# ---------------------------------------------------------------------------
# net_boost — Boost headers plus the separate-compilation defines.
#
# Without these, every TU that touches Asio or Beast instantiates the whole
# implementation, and the edit-build loop becomes unusable. With them, exactly
# one TU (boost_impl.cpp) carries it.
# ---------------------------------------------------------------------------
add_library(net_boost INTERFACE)
target_link_libraries(net_boost INTERFACE Boost::headers Threads::Threads)
target_compile_definitions(net_boost INTERFACE
    BOOST_ASIO_SEPARATE_COMPILATION
    BOOST_BEAST_SEPARATE_COMPILATION
    BOOST_ASIO_NO_DEPRECATED
)

# ---------------------------------------------------------------------------
# net_gateway — all matching-thread logic. Links `engine` and net_protocol,
# and deliberately NOT Boost: the gateway must be exercisable synchronously
# with no sockets and no threads, which is what makes net_smoke a real test.
# ---------------------------------------------------------------------------
add_library(net_gateway STATIC
    gateway/BookDirectory.cpp
    gateway/MarketDataPublisher.cpp
    gateway/MatchingLoop.cpp
    gateway/MatchingThread.cpp
    gateway/OrderStore.cpp
    gateway/Positions.cpp
    gateway/RejectCode.cpp
    # The binary codec lives here, not in net_io: it needs nothing but
    # memcpy, and keeping it Boost-free is what lets net_smoke fuzz the
    # frame parser without linking Asio. The JSON codec, which does need
    # Boost.JSON, is in net_io.
    wire/BinaryProtocol.cpp
)
# MatchingThread runs the poll loop on a thread of its own. Still no Boost.
target_link_libraries(net_gateway PUBLIC engine net_protocol Threads::Threads)

# ---------------------------------------------------------------------------
# net_io — sockets, sessions, listeners. Owns the one Boost impl TU.
# ---------------------------------------------------------------------------
add_library(net_io STATIC
    boost_impl.cpp
    io/EgressQueue.cpp
    io/HttpSession.cpp
    io/IoThread.cpp
    io/Listener.cpp
    io/PostingEventSink.cpp
    io/RingEgressSink.cpp
    io/Server.cpp
    io/TcpSession.cpp
    io/TraderDirectory.cpp
    io/WebSocketSession.cpp
    # The JSON codec needs Boost.JSON, so it lives here rather than beside
    # the binary codec in net_gateway. Both share MessageNames.hpp.
    wire/JsonProtocol.cpp
)
target_link_libraries(net_io PUBLIC net_gateway net_boost)

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_executable(exchange_server server_main.cpp)
target_link_libraries(exchange_server PRIVATE net_io)

# The reference client, and the only thing that makes the exchange usable
# before the browser GUI exists.
add_executable(exchange_cli ${CMAKE_SOURCE_DIR}/tools/exchange_cli.cpp)
target_link_libraries(exchange_cli PRIVATE net_io)

# net_smoke links net_gateway only — no Boost, no sockets — so gateway
# scenarios run deterministically and the build stays fast.
# net_smoke links net_io because its last layer runs a real server over a
# real loopback socket. The first three layers (ring, gateway scenarios,
# codec) touch none of it — that separation lives in net_gateway, which
# stays Boost-free so the gateway itself is provably testable without I/O.
add_executable(net_smoke smoke/net_smoke.cpp)
target_link_libraries(net_smoke PRIVATE net_io)
