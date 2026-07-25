#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["websockets>=13"]
# ///
"""
WebSocket/JSON end-to-end check.

Two browser-shaped clients trade against each other and both ladders are
compared, which is the gate for the GUI phase — stated as a script rather
than as "open two tabs and look", so it can actually be re-run.

It also verifies the property that makes shipping two codecs defensible: a
JSON client and a binary client are the same client as far as the matching
thread is concerned. The book here is created over the binary protocol by
exchange_cli in scripts/net_e2e.sh, or over JSON directly — either works,
which is the point.

    scripts/ws_e2e.py [--port 8080] [--keys KEY1 KEY2]
"""

import argparse
import asyncio
import json
import sys

import websockets


class Client:
    """A browser, minus the DOM. Mirrors web/app.js's state machine."""

    def __init__(self, name, key, url):
        self.name = name
        self.key = key
        self.url = url
        self.socket = None
        self.books = {}
        self.bids = {}
        self.asks = {}
        self.md_seq = 0
        self.in_snapshot = False
        self.gaps = 0
        self.fills = []
        self.position = {}
        self.rejects = []
        self.logged_on = asyncio.Event()
        self.books_listed = asyncio.Event()

    async def connect(self):
        self.socket = await websockets.connect(self.url)
        await self.send({"type": "logon", "api_key": self.key})
        asyncio.create_task(self._pump())
        await asyncio.wait_for(self.logged_on.wait(), timeout=5)

    async def send(self, message):
        await self.socket.send(json.dumps(message))

    async def _pump(self):
        try:
            async for raw in self.socket:
                self._on_event(json.loads(raw))
        except websockets.ConnectionClosed:
            pass

    def _on_event(self, event):
        kind = event["type"]
        if kind == "logon_ack":
            if event["reject_code"] != 0:
                raise SystemExit(f"{self.name}: logon rejected: {event['reject']}")
            self.logged_on.set()
        elif kind in ("book_entry", "create_book_ack"):
            if event.get("reject_code") == 0 and event.get("symbol"):
                self.books[event["symbol"]] = event["book_id"]
        elif kind == "book_list_end":
            self.books_listed.set()
        elif kind == "snapshot_begin":
            self.in_snapshot = True
            self.bids.clear()
            self.asks.clear()
            self.md_seq = event["md_seq"]
        elif kind == "snapshot_end":
            self.in_snapshot = False
            self.md_seq = event["md_seq"]
        elif kind == "level_update":
            if not self.in_snapshot:
                self._check_seq(event["md_seq"])
            book = self.bids if event["side"] == 0 else self.asks
            # quantity 0 is the standard L2 delete.
            if event["quantity"] == 0:
                book.pop(event["price"], None)
            else:
                book[event["price"]] = event["quantity"]
        elif kind == "trade_print":
            self._check_seq(event["md_seq"])
        elif kind == "exec_report":
            self.fills.append(event)
        elif kind == "position_update":
            self.position = event
        elif kind == "reject":
            self.rejects.append(event["reject"])

    def _check_seq(self, seq):
        if self.md_seq and seq != self.md_seq + 1:
            self.gaps += 1
        self.md_seq = seq

    def ladder(self):
        return (
            sorted(self.bids.items(), reverse=True),
            sorted(self.asks.items()),
        )


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument(
        "--keys",
        nargs=2,
        default=["alice-dev-key-change-me", "bob-dev-key-change-me"],
    )
    args = parser.parse_args()

    url = f"ws://{args.host}:{args.port}/ws"
    alice = Client("alice", args.keys[0], url)
    bob = Client("bob", args.keys[1], url)

    failures = []

    def check(condition, what):
        if not condition:
            failures.append(what)
            print(f"FAIL: {what}")

    await alice.connect()
    await bob.connect()
    print("both browsers logged on")

    # Book admin over JSON.
    await alice.send({"type": "create_book", "symbol": "WSTEST", "price_scale": 2})
    await asyncio.sleep(0.4)
    check("WSTEST" in alice.books, "a book can be created over JSON")
    book = alice.books.get("WSTEST")
    if book is None:
        return report(failures)

    # Cleared first: logon already delivered one listing (empty, since the
    # book did not exist yet), so waiting on a set event would return at once.
    bob.books_listed.clear()
    await bob.send({"type": "list_books"})
    await asyncio.wait_for(bob.books_listed.wait(), timeout=5)
    check(bob.books.get("WSTEST") == book,
          "the second client discovers it through list_books")

    for client in (alice, bob):
        await client.send({"type": "subscribe_md", "book_id": book, "depth": 10})
    await asyncio.sleep(0.4)

    # Alice makes a two-sided market; prices are integers, scaled by 100.
    for side, price, qty, coid in ((1, 5100, 100, 1), (1, 5200, 40, 2),
                                   (0, 4900, 70, 3)):
        await alice.send({"type": "new_order", "client_order_id": coid,
                          "book_id": book, "price": price, "quantity": qty,
                          "side": side, "tif": 0, "flags": 0})
    await asyncio.sleep(0.5)

    # Bob lifts part of the offer.
    await bob.send({"type": "new_order", "client_order_id": 1, "book_id": book,
                    "price": 5100, "quantity": 60, "side": 0, "tif": 0,
                    "flags": 0})
    await asyncio.sleep(0.6)

    check(len(bob.fills) == 1 and bob.fills[0]["quantity"] == 60,
          "the taker is filled")
    check(bob.fills and bob.fills[0]["price"] == 5100,
          "at the resting price, not the limit")
    check(len(alice.fills) == 1 and alice.fills[0]["leaves"] == 40,
          "the maker sees the same fill with 40 still working")
    check(bob.position.get("net_quantity") == 60, "the buyer is long 60")
    check(alice.position.get("net_quantity") == -60, "the seller is short 60")

    # THE GATE: both browsers agree on the book.
    check(alice.ladder() == bob.ladder(),
          "both clients' ladders agree after trading")
    if alice.ladder() != bob.ladder():
        print(f"  alice: {alice.ladder()}")
        print(f"  bob:   {bob.ladder()}")

    check(alice.gaps == 0 and bob.gaps == 0,
          "neither client saw a market-data sequence gap")

    bids, asks = alice.ladder()
    check(asks and asks[0] == (5100, 40),
          "the lifted level shows the remaining 40")
    check(bids and bids[0] == (4900, 70), "the bid is untouched")

    # Validation rules must reach a JSON client identically.
    await bob.send({"type": "new_order", "client_order_id": 2, "book_id": book,
                    "price": 5000, "quantity": 0, "side": 0, "tif": 0,
                    "flags": 0})
    await bob.send({"type": "cancel", "order_id": 999999,
                    "client_order_id": 0})
    await asyncio.sleep(0.4)
    check("InvalidQuantity" in bob.rejects, "zero quantity is rejected")
    check("UnknownOrder" in bob.rejects,
          "cancelling a nonexistent order is rejected")

    await alice.socket.close()
    await bob.socket.close()
    return report(failures)


def report(failures):
    if not failures:
        print("ws_e2e: all checks passed")
        return 0
    print(f"ws_e2e: {len(failures)} failure(s)")
    return 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
