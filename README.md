# cpp-exchange

## Benchmarks (OrderBook)

### 612785f

The simplest approach - store all `Order`s in a `std::vector`, with the exception of mapping `OrderId`s to their respective `Buy` and `Sell` sides.

### 28895d3

Store `PriceLevel`s in sorted order in a `std::vector`.

Each `PriceLevel` stores a `std::vector<Order>`, sorted by time.

Delete `PriceLevel`s that are empty.

### 83bd5ae

Replace `std::function_ref match_predicate` with inlined lambdas by templating `match` on `OrderSide`.
