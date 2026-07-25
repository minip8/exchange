/*
 * The whole GUI. No framework, no bundler, no CDN, no npm.
 *
 * It speaks the same Command/Event vocabulary as the binary protocol, through
 * the JSON codec, so every field name here matches the corresponding C++
 * struct field byte for byte. If you are reading this next to
 * net/wire/BinaryProtocol.hpp, the names line up on purpose.
 *
 * Two things worth knowing before changing anything:
 *
 *   1. PRICES ARE INTEGERS ON THE WIRE. Divide by 10^price_scale to display,
 *      multiply to send. There are no floats in the protocol, in either
 *      direction, and introducing one here would make this client disagree
 *      with every other one about the same fill.
 *
 *   2. Rendering is coalesced onto requestAnimationFrame and triggered by the
 *      end-of-batch flag. A sweep through ten price levels is one paint, not
 *      ten.
 */
'use strict';

const EVENT_FLAG_END_OF_BATCH = 1;
const EVENT_FLAG_AGGRESSOR = 2;
const SIDE_BUY = 0;

const el = (id) => document.getElementById(id);
const $status = el('status');
const $ladder = el('ladder');
const $tape = el('tape');
const $working = el('working');
const $positions = el('positions');
const $fills = el('fills');
const $bookSelect = el('book');

const state = {
  socket: null,
  loggedOn: false,
  sessionId: 0,
  traderId: 0,
  books: new Map(),        // book_id -> {symbol, price_scale}
  bookId: null,
  bids: new Map(),         // price -> quantity
  asks: new Map(),
  mdSeq: 0,
  inSnapshot: false,
  working: new Map(),      // order_id -> {side, price, leaves}
  positions: new Map(),    // book_id -> payload
  tape: [],
  fills: [],
  nextCoid: 1,
  reconnectDelay: 500,
  dirty: false,
};

// ---------------------------------------------------------------------------
// formatting
// ---------------------------------------------------------------------------

function scaleOf(bookId) {
  const book = state.books.get(bookId);
  return book ? book.price_scale : 2;
}

function fromWire(price, bookId) {
  return Number(price) / Math.pow(10, scaleOf(bookId));
}

function toWire(price, bookId) {
  return Math.round(Number(price) * Math.pow(10, scaleOf(bookId)));
}

function showPrice(price, bookId) {
  return fromWire(price, bookId).toFixed(scaleOf(bookId));
}

function clockNow() {
  return new Date().toTimeString().slice(0, 8);
}

// ---------------------------------------------------------------------------
// transport
// ---------------------------------------------------------------------------

function send(message) {
  if (state.socket && state.socket.readyState === WebSocket.OPEN) {
    state.socket.send(JSON.stringify(message));
  }
}

function connect() {
  const key = el('key').value.trim();
  if (!key) return;
  // Persisted so a refresh does not mean retyping it. This is a toy exchange
  // for friends; localStorage is the right amount of ceremony.
  localStorage.setItem('exchange.key', key);

  const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
  const socket = new WebSocket(`${scheme}://${location.host}/ws`);
  state.socket = socket;

  socket.onopen = () => {
    setStatus('connected', true);
    state.reconnectDelay = 500;
    send({ type: 'logon', api_key: key });
  };

  socket.onmessage = (message) => {
    for (const line of message.data.split('\n')) {
      if (line) onEvent(JSON.parse(line));
    }
  };

  socket.onclose = () => {
    setStatus('disconnected', false);
    state.loggedOn = false;
    // Exponential backoff, capped. On reconnect everything is re-derived
    // from a fresh logon and snapshot rather than being patched up — the
    // server restating the world is always cheaper than the client guessing.
    setTimeout(connect, state.reconnectDelay);
    state.reconnectDelay = Math.min(state.reconnectDelay * 2, 10000);
  };

  socket.onerror = () => socket.close();
}

function setStatus(text, up) {
  $status.textContent = text;
  $status.className = `status ${up ? 'up' : 'down'}`;
}

// ---------------------------------------------------------------------------
// event handling
// ---------------------------------------------------------------------------

function onEvent(event) {
  switch (event.type) {
    case 'logon_ack':
      if (event.reject_code !== 0) {
        setStatus(`rejected: ${event.reject}`, false);
        return;
      }
      state.loggedOn = true;
      state.sessionId = event.session_id;
      state.traderId = event.trader_id;
      setStatus(`trader ${event.trader_id}`, true);
      state.books.clear();
      break;

    case 'book_entry':
    case 'create_book_ack':
      if (event.reject_code === 0 && event.symbol) {
        state.books.set(event.book_id,
                        { symbol: event.symbol, price_scale: event.price_scale });
      }
      break;

    case 'book_list_end':
      renderBookList();
      if (state.bookId === null && state.books.size > 0) {
        selectBook([...state.books.keys()][0]);
      }
      break;

    case 'snapshot_begin':
      // A snapshot is a full restatement, so the local book is discarded
      // rather than merged into.
      state.inSnapshot = true;
      state.bids.clear();
      state.asks.clear();
      state.mdSeq = event.md_seq;
      break;

    case 'level_update':
      if (!state.inSnapshot) checkSequence(event.md_seq);
      applyLevel(event);
      break;

    case 'snapshot_end':
      state.inSnapshot = false;
      state.mdSeq = event.md_seq;
      break;

    case 'trade_print':
      checkSequence(event.md_seq);
      state.tape.unshift({
        time: clockNow(),
        price: event.price,
        quantity: event.quantity,
        side: event.side,
        book_id: event.book_id,
      });
      if (state.tape.length > 50) state.tape.pop();
      break;

    case 'order_ack':
      if (event.quantity > 0) {
        state.working.set(event.order_id, {
          side: event.side,
          price: event.price,
          leaves: event.quantity,
          book_id: event.book_id,
        });
      }
      break;

    case 'amend_ack':
      // An amend always mints a NEW order id — the engine cannot amend in
      // place, so the server does remove + re-add. Follow the chain.
      state.working.delete(event.orig_order_id);
      if (event.quantity > 0) {
        state.working.set(event.order_id, {
          side: event.side,
          price: event.price,
          leaves: event.quantity,
          book_id: event.book_id,
        });
      }
      break;

    case 'cancel_ack':
      state.working.delete(event.order_id);
      break;

    case 'exec_report': {
      const order = state.working.get(event.order_id);
      if (order) {
        order.leaves = event.leaves;
        if (event.leaves === 0) state.working.delete(event.order_id);
      }
      state.fills.unshift({
        time: clockNow(),
        side: event.side,
        price: event.price,
        quantity: event.quantity,
        aggressor: (event.flags & EVENT_FLAG_AGGRESSOR) !== 0,
        book_id: event.book_id,
      });
      if (state.fills.length > 50) state.fills.pop();
      break;
    }

    case 'position_update':
      state.positions.set(event.book_id, event);
      break;

    case 'reject':
      // Not a modal: rejects are routine (a stale cancel, a duplicate coid)
      // and interrupting the trader for each one would be worse than useless.
      setStatus(`reject: ${event.reject}`, true);
      break;

    default:
      break;
  }

  // Coalesce the paint. A snapshot or a swept book is one frame, not one per
  // level, which is exactly what the end-of-batch flag is for.
  state.dirty = true;
  if ((event.flags & EVENT_FLAG_END_OF_BATCH) !== 0) scheduleRender();
}

function checkSequence(seq) {
  if (state.mdSeq !== 0 && seq !== state.mdSeq + 1) {
    // Market data is idempotent-recoverable by design: the server may drop a
    // level update under egress pressure, and the cure is to ask for the
    // whole book again rather than to try to interpolate.
    console.warn(`md gap: expected ${state.mdSeq + 1}, got ${seq} — resyncing`);
    send({ type: 'get_snapshot', book_id: state.bookId, depth: 10 });
  }
  state.mdSeq = seq;
}

function applyLevel(event) {
  const side = event.side === SIDE_BUY ? state.bids : state.asks;
  // quantity 0 means the level is gone. Standard L2; there is no separate
  // delete message.
  if (event.quantity === 0) side.delete(event.price);
  else side.set(event.price, event.quantity);
}

function selectBook(bookId) {
  if (state.bookId === bookId) return;
  if (state.bookId !== null) {
    send({ type: 'unsubscribe_md', book_id: state.bookId });
  }
  state.bookId = bookId;
  state.bids.clear();
  state.asks.clear();
  state.mdSeq = 0;
  state.tape = [];
  const book = state.books.get(bookId);
  el('instrument').textContent = book ? book.symbol : '—';
  send({ type: 'subscribe_md', book_id: bookId, depth: 10 });
}

// ---------------------------------------------------------------------------
// rendering
// ---------------------------------------------------------------------------

let frameQueued = false;
function scheduleRender() {
  if (frameQueued) return;
  frameQueued = true;
  requestAnimationFrame(() => {
    frameQueued = false;
    if (!state.dirty) return;
    state.dirty = false;
    render();
  });
}

function clone(id) {
  return document.getElementById(id).content.firstElementChild.cloneNode(true);
}

function render() {
  renderLadder();
  renderTape();
  renderWorking();
  renderPositions();
  renderFills();
}

function renderLadder() {
  const bids = [...state.bids.entries()].sort((a, b) => b[0] - a[0]);
  const asks = [...state.asks.entries()].sort((a, b) => a[0] - b[0]);
  const rows = Math.max(bids.length, asks.length);
  const max = Math.max(
    1,
    ...bids.map((entry) => entry[1]),
    ...asks.map((entry) => entry[1]),
  );

  const body = document.createDocumentFragment();
  for (let i = 0; i < rows; i++) {
    const row = clone('tpl-ladder-row');
    const bid = bids[i];
    const ask = asks[i];

    if (bid) {
      row.querySelector('.bid .n').textContent = bid[1];
      row.querySelector('.bid .bar').style.width = `${(bid[1] / max) * 100}%`;
    }
    if (ask) {
      row.querySelector('.ask .n').textContent = ask[1];
      row.querySelector('.ask .bar').style.width = `${(ask[1] / max) * 100}%`;
    }

    const px = row.querySelector('.px');
    const price = bid ? bid[0] : ask ? ask[0] : null;
    if (price !== null) {
      px.textContent = showPrice(price, state.bookId);
      px.onclick = () => { el('price').value = fromWire(price, state.bookId); };
    }
    body.appendChild(row);
  }
  $ladder.replaceChildren(body);
}

function renderTape() {
  const body = document.createDocumentFragment();
  for (const print of state.tape) {
    const row = clone('tpl-tape-row');
    row.querySelector('.t').textContent = print.time;
    const px = row.querySelector('.px');
    px.textContent = showPrice(print.price, print.book_id);
    // Coloured by the AGGRESSOR's side, which is what a tape conventionally
    // shows: it is the direction the trade happened in, not who was resting.
    px.className = `px ${print.side === SIDE_BUY ? 'buy' : 'sell'}`;
    row.querySelector('.n').textContent = print.quantity;
    body.appendChild(row);
  }
  $tape.replaceChildren(body);
}

function renderWorking() {
  const body = document.createDocumentFragment();
  for (const [orderId, order] of state.working) {
    const row = clone('tpl-working-row');
    row.querySelector('.id').textContent = orderId;
    const side = row.querySelector('.side');
    side.textContent = order.side === SIDE_BUY ? 'buy' : 'sell';
    side.className = `side ${order.side === SIDE_BUY ? 'buy' : 'sell'}`;
    row.querySelector('.px').textContent = showPrice(order.price, order.book_id);
    row.querySelector('.n').textContent = order.leaves;
    row.querySelector('.cancel').onclick = () =>
      send({ type: 'cancel', order_id: orderId, client_order_id: 0 });
    body.appendChild(row);
  }
  $working.replaceChildren(body);
}

function renderPositions() {
  const body = document.createDocumentFragment();
  for (const [bookId, position] of state.positions) {
    if (position.net_quantity === 0 && position.realized_pnl === 0) continue;
    const book = state.books.get(bookId);
    const row = clone('tpl-position-row');
    row.querySelector('.sym').textContent = book ? book.symbol : bookId;
    const net = row.querySelector('.net');
    net.textContent = position.net_quantity;
    net.className = `net ${position.net_quantity < 0 ? 'neg' : 'pos'}`;
    row.querySelector('.avg').textContent = showPrice(position.avg_cost, bookId);
    for (const [field, key] of [['.realized', 'realized_pnl'],
                                ['.unreal', 'unrealized_pnl']]) {
      const cell = row.querySelector(field);
      cell.textContent = showPrice(position[key], bookId);
      cell.className = `${field.slice(1)} ${position[key] < 0 ? 'neg' : 'pos'}`;
    }
    body.appendChild(row);
  }
  $positions.replaceChildren(body);
}

function renderFills() {
  const body = document.createDocumentFragment();
  for (const fill of state.fills) {
    const row = clone('tpl-fill-row');
    row.querySelector('.t').textContent = fill.time;
    const side = row.querySelector('.side');
    side.textContent =
      (fill.side === SIDE_BUY ? 'buy' : 'sell') + (fill.aggressor ? '' : ' (m)');
    side.className = `side ${fill.side === SIDE_BUY ? 'buy' : 'sell'}`;
    row.querySelector('.px').textContent = showPrice(fill.price, fill.book_id);
    row.querySelector('.n').textContent = fill.quantity;
    body.appendChild(row);
  }
  $fills.replaceChildren(body);
}

function renderBookList() {
  const previous = state.bookId;
  $bookSelect.replaceChildren();
  for (const [bookId, book] of state.books) {
    const option = document.createElement('option');
    option.value = bookId;
    option.textContent = book.symbol;
    $bookSelect.appendChild(option);
  }
  if (previous !== null) $bookSelect.value = previous;
}

// ---------------------------------------------------------------------------
// wiring
// ---------------------------------------------------------------------------

el('connect').onclick = connect;
el('key').value = localStorage.getItem('exchange.key') || '';

$bookSelect.onchange = () => selectBook(Number($bookSelect.value));

el('ticket').onsubmit = (submitEvent) => {
  submitEvent.preventDefault();
  if (!state.loggedOn || state.bookId === null) return;
  const raw = el('price').value;
  const market = raw === '';
  send({
    type: 'new_order',
    client_order_id: state.nextCoid++,
    book_id: state.bookId,
    // An empty price means market: the gateway synthesizes a marketable
    // limit and forces IOC, so nothing rests at an absurd price.
    price: market ? 0 : toWire(raw, state.bookId),
    quantity: Number(el('qty').value),
    side: Number(el('side').value),
    tif: Number(el('tif').value),
    flags: market ? 1 : 0,
  });
};

// Keeps the connection warm through idle NATs and tunnels.
setInterval(() => {
  if (state.loggedOn) send({ type: 'heartbeat' });
}, 15000);

if (el('key').value) connect();
