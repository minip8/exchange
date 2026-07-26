# Running

What each binary is, how to start it, and the checks that stand in for a unit
test suite. Build first — see [BUILDING.md](BUILDING.md).

- [The engine smoke test](#the-engine-smoke-test)
- [The server](#the-server)
- [The CLI client](#the-cli-client)
- [The browser GUI](#the-browser-gui)
- [Exposing it](#exposing-it)
- [Verification](#verification)

## The engine smoke test

`app` is a hand-rolled smoke test of the engine (`src/main.cpp`). There is no
unit-test suite and none should be added — correctness lives here, in
`net_smoke`, in the benchmarks, and in the flash1 conformance harness.

```bash
./build/debug/app       # sanitized — the one you want while developing
./build/release/app     # also valid; it checks with a local check() helper,
                        # not assert(), so NDEBUG does not compile it away
```

It prints each failure and exits non-zero, so it works as a CI step.

## The server

`exchange_server` is the networked front end: I/O threads feeding one matching
thread through lock-free SPSC rings, serving two protocols over one core.

```bash
cmake --build --preset debug
cp config/traders.example.json config/traders.json   # then edit the keys
./build/debug/src/net/exchange_server
```

`config/traders.json` is gitignored; only the example is checked in. Give each
trader their own key — generate one with
`head -c 24 /dev/urandom | base64`. Trader id 0 is reserved as the
not-authenticated sentinel. **Ownership is by trader id**: two clients logged on
with the same key see each other's orders and fills.

Two listeners:

- **Binary TCP, port 9001, bound to 127.0.0.1.** For algo clients. Never exposed
  publicly.
- **HTTP + WebSocket, port 8080, bound to 0.0.0.0.** Serves `web/` and the GUI's
  `/ws` endpoint. The only port meant to face the internet.

### Flags

```
exchange_server [--binary-port N] [--binary-bind ADDR]
                [--http-port N] [--io-threads N]
                [--spin-us N] [--egress ring|post]
                [--traders PATH] [--web-root PATH]
```

| Flag | Default | Notes |
|---|---|---|
| `--binary-port` | `9001` | |
| `--binary-bind` | `127.0.0.1` | There is no good reason to change this. |
| `--http-port` | `8080` | Bind is `0.0.0.0`, not configurable. |
| `--io-threads` | `1` | Thread-per-`io_context`, not one context with N threads. |
| `--spin-us` | `0` | How long the matching thread spins before yielding. 0 keeps a laptop cool; set ~100–200 when benchmarking. |
| `--egress ring\|post` | `ring` | `ring` = per-thread SPSC + eventfd; `post` = `asio::post` per batch, kept as the measured baseline and as a fallback. |
| `--traders` | `config/traders.json` | |
| `--web-root` | `web` | Relative to the CWD, so start the server from the repo root. |

Run the Release build (`cmake --build --preset server`) for anything but
debugging — the Debug build is sanitized and slow.

## The CLI client

`exchange_cli` is the reference binary-protocol client. Interactive by default.

```bash
./build/debug/src/net/exchange_cli --key alice-dev-key-change-me
```

```
exchange_cli --key KEY [--host H] [--port N]
             [--cancel-on-disconnect]
             [--tail BOOK_ID [--seconds N] [--verify]]
             [--load N --rate MSGS_PER_SEC [--book ID] [--seconds N]]
```

Defaults: `--host 127.0.0.1`, `--port 9001`, `--seconds 5`.

### Interactive commands

```
book <SYMBOL> [scale]        create an instrument (price scale default 2)
books                        list instruments
buy  <book> <qty> <price>    limit buy   (price 0 = market)
sell <book> <qty> <price>    limit sell
ioc  <buy|sell> <book> <qty> <price>
cancel <order_id>
amend  <order_id> <qty> <price>
sub <book> | unsub <book> | snap <book>
help | quit
```

Prices are integers on both protocols — the GUI divides by `10^price_scale`.
No floats on the wire, ever.

### Non-interactive modes

**Market-data tail.** `--tail BOOK_ID` subscribes, follows deltas for
`--seconds`, and with `--verify` asks for a fresh snapshot and compares the
ladder it reconstructed against the one the server just restated. Agreement
means the delta stream is complete and correctly ordered. This is the gate that
`scripts/net_e2e.sh` drives.

**Load generator.** `--load N --rate R` opens N connections, each in its own
thread, and pushes an aggregate of R messages/second at the server for
`--seconds`. Each client quotes both sides around a wandering mid and cancels as
it goes, so the book actually churns.

```bash
./build/release/src/net/exchange_cli --key KEY --load 8 --rate 50000 --book 1 --seconds 60
```

This is the multi-I/O-thread gate. It is not looking for throughput — it is
looking for *nothing going wrong*: no market-data sequence gaps, no reordering
within a session, no throttle storm, and (run against the `tsan` tree) no data
race across the ring.

## The browser GUI

Start the server, then open <http://localhost:8080>. It is served from `web/`
(`index.html`, `app.js`, `style.css`) and talks WebSocket/JSON on `/ws`. JSON
field names are byte-identical to the binary struct fields, and a JSON client is
the same client as a binary one as far as the matching thread is concerned.

## Exposing it

Only 8080 is meant to be reachable. Cloudflare Tunnel needs no inbound firewall
rule and terminates TLS:

```bash
cloudflared tunnel --url http://localhost:8080
```

The binary port stays bound to 127.0.0.1 regardless.

## Verification

There is no unit-test suite. These four checks are the substitute, and
`net_smoke` under **both** the `debug` and `tsan` trees is the core of it.

```bash
./build/debug/src/net/net_smoke   # rings, gateway scenarios, codecs, fuzz, loopback
./build/tsan/src/net/net_smoke    # the same, under ThreadSanitizer
scripts/net_e2e.sh                # binary protocol + market-data replay gate
scripts/ws_e2e.py                 # two browser clients trade; ladders agree
```

Build them with `cmake --build --preset smoke` and
`cmake --build --preset smoke-tsan`.

**`net_smoke`** exercises the SPSC rings, the gateway scenarios, both codecs, a
fuzz pass and an in-process loopback. Its codec section is the anti-drift
mechanism between the two protocols: it encodes the same content through binary
and JSON and compares the resulting `Command`/`Event` bit-for-bit. Only the
`tsan` tree can validate the rings' memory ordering, so a Debug-only pass is
half a pass.

**`scripts/net_e2e.sh [debug|release]`** runs the shipped binaries against real
sockets, which proves two things the in-process loopback cannot: that
`exchange_cli` and `exchange_server` actually interoperate, and — the
market-data gate — that a client reconstructing the book purely from a snapshot
plus the deltas that follow ends up with the ladder the server restates in a
fresh snapshot.

**`scripts/ws_e2e.py [--port 8080] [--keys KEY1 KEY2]`** is the GUI gate: two
browser-shaped clients trade against each other and both ladders are compared.
Runs under `uv` (`#!/usr/bin/env -S uv run --script`), so just execute it.

For engine correctness specifically, the real gate is the flash1 conformance
harness — see [BENCHMARKING.md](BENCHMARKING.md#the-flash1-harness).
