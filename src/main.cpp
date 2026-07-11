#include <bits/stdc++.h>

#include <chrono>
#include <iostream>

#include "engine/MatchingEngine.hpp"
#include "engine/Order.hpp"
#include "types/OrderBookId.hpp"
#include "types/OrderId.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"
#include "types/OrderTime.hpp"

int main() {
  using namespace Exchange;
  Exchange::Engine::MatchingEngine me;
  auto _ = me.addOrder(
      Types::OrderBookId{1},
      Engine::Order{
          .id = Types::OrderId{1},
          .price = Types::OrderPrice{100},
          .time = Types::OrderTime{std::chrono::high_resolution_clock::now()},
          .quantity = Types::OrderQuantity{3},
          .side = Types::OrderSide{Exchange::Types::OrderSide::Buy},
      });
      std::cout << 0 << '\n';
}
