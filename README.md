# cpp-exchange

https://minip8.github.io/#project/cpp-exchange

A limit-order-book matching engine in C++26, plus a networked front end around
it.

## Docs

- **[docs/BUILDING.md](docs/BUILDING.md)** — prerequisites, the three build
  trees (`debug` / `tsan` / `release`), every preset, and where binaries land.
- **[docs/RUNNING.md](docs/RUNNING.md)** — the server, the CLI, the browser GUI,
  and the four checks that stand in for a unit-test suite.
- **[docs/BENCHMARKING.md](docs/BENCHMARKING.md)** — Google Benchmark, the
  flash1 conformance harness, the network benches, and how to read the numbers.

```bash
cmake --preset debug && cmake --build --preset debug     # develop here
cmake --preset release && cmake --build --preset release # benchmark here
```

`CLAUDE.md` has the architecture and design rationale.

## Benchmarks (OrderBook)

Here are some features/changes at each stage of the project for bookkeeping.

Will include the appropriate plot(s) eventually. :)

### 612785f

The simplest approach - store all `Order`s in a `std::vector`, with the exception of mapping `OrderId`s to their respective `Buy` and `Sell` sides.

### 28895d3

Store `PriceLevel`s in sorted order in a `std::vector`.

Each `PriceLevel` stores a `std::vector<Order>`, sorted by time.

Delete `PriceLevel`s that are empty.

### 83bd5ae

Replace `std::function_ref match_predicate` with inlined lambdas by templating `match` on `OrderSide`.

### 981c795

Store `PriceLevel`s in the reverse order, so the best prices are at the back of the `std::vector`.

## Networking (feat. Claude)

The engine is a pure in-process library; `exchange_server` is the networked
front end around it. Two protocols over one core: a custom binary TCP
protocol for algo clients, and WebSocket/JSON for the browser GUI. I/O
threads feed a dedicated matching thread through lock-free SPSC rings.

```bash
cmake --build --preset debug
cp config/traders.example.json config/traders.json   # then edit the keys
./build/debug/src/net/exchange_server
```

- **binary TCP, port 9001, loopback only.** Never exposed publicly. Use
  `exchange_cli --key <key>` — `help` lists the commands.
- **HTTP + WebSocket, port 8080.** Serves `web/` and the GUI's `/ws`
  endpoint. This is the only port that faces the internet.

### Exposing it

Only 8080 is meant to be reachable. Cloudflare Tunnel needs no inbound
firewall rule and terminates TLS for you:

```bash
cloudflared tunnel --url http://localhost:8080
```

The binary port stays bound to 127.0.0.1 regardless (`--binary-bind` can
change that, but there is no reason to). Give each trader their own key in
`config/traders.json`; the file is gitignored and only the example is
checked in.

### Checks

```bash
./build/debug/src/net/net_smoke   # rings, gateway, codecs, fuzz, loopback
./build/tsan/src/net/net_smoke    # the same, under ThreadSanitizer
scripts/net_e2e.sh                # binary protocol + market-data replay gate
scripts/ws_e2e.py                 # two browser clients trade; ladders agree
```

`net_smoke` must pass under both the `debug` (ASan/UBSan) and `tsan` trees.
That pair is the substitute for a unit-test suite. See `CLAUDE.md` for the
architecture and `.claude/plans/` for the design rationale.
