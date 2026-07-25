/*
net_smoke — the correctness harness for the networking layer.

There is no unit-test framework here, and none should be added: the repo's
`check()`-not-`assert` convention already works (Release defines NDEBUG, which
would compile assertions away) and stays dependency-free.

This links net_gateway only — no Boost, no sockets, no threads for the
gateway layers — which is exactly why the EventSink seam exists.

Run it under BOTH the `debug` (ASan/UBSan) and `tsan` trees. That pair is the
substitute for a unit-test suite, and for this class of bug it is stronger
than most suites would be.
*/
#include <print>
#include <string_view>

#include "net/core/RejectCode.hpp"

namespace {
int g_failures{0};

void check(bool condition, std::string_view what) {
  if (!condition) {
    std::println("FAIL: {}", what);
    ++g_failures;
  }
}

void testRejectCodes() {
  using Exchange::Net::RejectCode;
  using Exchange::Net::toRejectCode;
  using Exchange::Net::toString;
  using Exchange::Net::toWire;
  using Exchange::Types::EngineError;

  check(toRejectCode(EngineError::OrderNotFound) == RejectCode::UnknownOrder,
        "OrderNotFound maps to UnknownOrder");
  check(toRejectCode(EngineError::OrderBookNotFound) == RejectCode::UnknownBook,
        "OrderBookNotFound maps to UnknownBook");
  // The protocol must not become an order-id enumeration oracle.
  check(toWire(RejectCode::NotYourOrder) == RejectCode::UnknownOrder,
        "NotYourOrder is indistinguishable from UnknownOrder on the wire");
  check(toString(RejectCode::Throttled) == "Throttled",
        "reject codes stringify for logging");
}
}  // namespace

int main() {
  testRejectCodes();

  if (g_failures == 0) {
    std::println("net_smoke: all checks passed");
    return 0;
  }
  std::println("net_smoke: {} failure(s)", g_failures);
  return 1;
}
