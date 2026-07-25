#include <chrono>
#include <print>
#include <string_view>

#include "engine/MatchingEngine.hpp"
#include "engine/Order.hpp"
#include "engine/OrderBook.hpp"
#include "types/EngineError.hpp"
#include "types/OrderBookId.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"
#include "types/OrderTime.hpp"
#include "types/Symbol.hpp"

namespace {
int g_failures{0};

// Not `assert`: Release defines NDEBUG, and this is the only correctness
// harness in the project, so the checks must run in every config.
void check(bool condition, std::string_view what) {
  if (!condition) {
    std::println("FAIL: {}", what);
    ++g_failures;
  }
}
}  // namespace

int main() {
  using namespace Exchange;
  using Types::EngineError;
  using Types::Symbol;

  Engine::MatchingEngine me;

  const Symbol nvda{"NVDA"};
  const auto nvda_id{me.addOrderBook(Engine::OrderBook{nvda})};
  check(nvda_id.has_value(), "listing NVDA succeeds");
  if (!nvda_id.has_value()) return 1;
  std::println("listed {} as book {}", nvda.view(), nvda_id->value);

  // Symbols are unique: a second book on the same instrument is rejected
  // rather than silently replacing the live one.
  const auto duplicate{me.addOrderBook(Engine::OrderBook{nvda})};
  check(!duplicate.has_value() &&
            duplicate.error() == EngineError::DuplicateSymbol,
        "relisting NVDA is rejected as DuplicateSymbol");

  // An unlisted ticker resolves to an error, not to some other book.
  const auto unlisted{me.resolve(Symbol{"GOOG"})};
  check(
      !unlisted.has_value() && unlisted.error() == EngineError::SymbolNotFound,
      "unlisted GOOG resolves to SymbolNotFound");

  // Ticker-addressed and id-addressed lookups reach the same book.
  const auto by_symbol{me.getOrderBook(nvda)};
  const auto by_id{me.getOrderBook(*nvda_id)};
  check(by_symbol.has_value() && by_id.has_value(), "both lookups find a book");
  if (!by_symbol.has_value() || !by_id.has_value()) return 1;
  check(&by_symbol->get() == &by_id->get(),
        "symbol and id lookups reach the same book");
  check(by_symbol->get().symbol() == nvda, "the book knows its own symbol");
  check(me.resolve(nvda).value() == *nvda_id, "resolve agrees with the id");

  // Order entry stays id-addressed — resolve once, then route on the id.
  const auto fills{me.addOrder(
      *nvda_id, Engine::Order{
                    Types::OrderPrice{100},
                    Types::OrderTime{std::chrono::high_resolution_clock::now()},
                    Types::OrderQuantity{3},
                    Types::OrderSide::Buy,
                })};
  check(fills.has_value() && fills->empty(),
        "the lone order rests without crossing");
  check(by_symbol->get().buys().size() == 1, "it rests on the NVDA book");

  std::println("{}", g_failures == 0 ? "ok" : "FAILED");
  return g_failures == 0 ? 0 : 1;
}
