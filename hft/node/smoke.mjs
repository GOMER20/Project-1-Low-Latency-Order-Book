// Starts HFT, trades a little through the Node client, and checks every answer.
// It proves two things the C++ tests cannot: that a program in another language
// can drive HFT from the layouts in wire.hpp alone, and that the `hft` binary
// starts, serves, shuts down and leaves a recording that verifies.
//
//   node hft/node/smoke.mjs path/to/hft
import assert from 'node:assert/strict';
import { execFileSync, spawn } from 'node:child_process';
import { existsSync, mkdtempSync, readdirSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { HftClient, OrderType, Side } from './hft-client.mjs';

const binary = process.argv[2];
if (!binary) {
  console.error('usage: node smoke.mjs path/to/hft');
  process.exit(2);
}

const dir = mkdtempSync(path.join(tmpdir(), 'hft-'));
const socketPath = path.join(dir, 'hft.sock');
const server = spawn(binary, ['serve', '--socket', socketPath, '--data', dir], {
  stdio: ['ignore', 'inherit', 'inherit'],
});
const exited = new Promise((resolve) => server.once('exit', resolve));

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

try {
  for (let waited = 0; !existsSync(socketPath); waited += 20) {
    assert.ok(waited < 10_000, 'HFT never created its socket');
    await sleep(20);
  }

  const hft = new HftClient(socketPath);
  await hft.connect();
  const events = [];
  for (const name of ['accepted', 'rejected', 'fill', 'cancelled', 'cancelRejected']) {
    hft.on(name, (event) => events.push({ name, ...event }));
  }

  const welcome = await hft.hello([
    { symbol: 'AAPL', minPrice: 1, numLevels: 100_000, maxOrders: 1_024 },
    { symbol: 'MSFT', minPrice: 1, numLevels: 100_000, maxOrders: 1_024 },
  ]);
  assert.deepEqual(welcome, { version: 1, bookCount: 2, session: 1, resumed: false });

  // The house quotes AAPL at 228.49 / 228.51.
  hft.quote(
    0,
    [{ price: 22849, quantity: 500 }, { price: 22848, quantity: 700 }],
    [{ price: 22851, quantity: 400 }, { price: 22852, quantity: 600 }],
  );
  assert.deepEqual(await hft.depth(0), {
    symbol: 0,
    bids: [{ price: 22849, quantity: 500 }, { price: 22848, quantity: 700 }],
    asks: [{ price: 22851, quantity: 400 }, { price: 22852, quantity: 600 }],
  });

  // A buy for 500 takes all 400 at 228.51 and 100 at 228.52.
  hft.order({ tag: 1n, symbol: 0, side: Side.Buy, price: 22852, quantity: 500 });
  // A sell rests inside the spread; a market buy from another trader takes it.
  hft.order({ tag: 2, symbol: 0, side: Side.Sell, price: 22850, quantity: 30 });
  hft.order({ tag: 3, symbol: 0, side: Side.Buy, quantity: 30, type: OrderType.Market });
  // An order for a symbol that does not exist, and a cancel for an order that never was.
  hft.order({ tag: 4, symbol: 9, side: Side.Buy, price: 100, quantity: 1 });
  hft.cancel({ tag: 5, symbol: 1, orderId: 12345n });
  await hft.ping();

  assert.deepEqual(
    events.map((event) => event.name),
    ['fill', 'fill', 'accepted', 'accepted', 'fill', 'fill', 'accepted', 'rejected', 'cancelRejected'],
  );
  assert.deepEqual(
    events.slice(0, 2).map(({ tag, price, quantity, remaining, maker, withHouse }) => ({
      tag, price, quantity, remaining, maker, withHouse,
    })),
    [
      { tag: 1n, price: 22851, quantity: 400, remaining: 100, maker: false, withHouse: true },
      { tag: 1n, price: 22852, quantity: 100, remaining: 0, maker: false, withHouse: true },
    ],
  );
  assert.equal(events[2].filled, 500);
  assert.equal(events[3].tag, 2n);
  assert.equal(events[3].resting, 30);
  const [taker, maker] = events.slice(4, 6);
  assert.deepEqual(
    [taker.tag, taker.price, taker.maker, taker.withHouse, taker.side],
    [3n, 22850, false, false, Side.Buy],
  );
  assert.deepEqual(
    [maker.tag, maker.orderId, maker.maker, maker.remaining, maker.side],
    [2n, events[3].orderId, true, 0, Side.Sell],
  );
  assert.equal(events[7].tag, 4n);
  assert.deepEqual([events[8].tag, events[8].orderId], [5n, 12345n]);

  // A resting order, cancelled by the ID it was given.
  hft.order({ tag: 6, symbol: 1, side: Side.Buy, price: 43100, quantity: 10 });
  await hft.ping();
  hft.cancel({ tag: 7, symbol: 1, orderId: events.at(-1).orderId });
  await hft.ping();
  assert.equal(events.at(-1).name, 'cancelled');
  assert.deepEqual(await hft.depth(1), { symbol: 1, bids: [], asks: [] });

  const stats = await hft.stats();
  assert.equal(stats.engine, 'HFT');
  assert.equal(stats.orders, 5);
  assert.equal(stats.trades, 3);
  assert.equal(stats.trades_between_clients, 1);
  assert.equal(stats.shares, 530);
  assert.equal(stats.events_dropped, 0);
  assert.equal(stats.recording, true);
  assert.equal(stats.symbols[0].symbol, 'AAPL');
  assert.equal(stats.order_time.count, 7);

  // Shut it down, and check what it left behind.
  hft.shutdown();
  assert.equal(await exited, 0, 'HFT did not exit cleanly');
  assert.ok(!existsSync(socketPath), 'HFT left its socket behind');
  const written = JSON.parse(readFileSync(path.join(dir, 'hft-stats.json'), 'utf8'));
  assert.equal(written.trades, 3);
  const recording = readdirSync(dir).find((name) => name.endsWith('.rec'));
  assert.ok(recording, 'HFT left no recording');
  const verified = execFileSync(binary, ['verify', path.join(dir, recording), written.recorded_digest], {
    encoding: 'utf8',
  });
  assert.match(verified, /replay matches the session/);

  console.log(`ok: Node drove HFT through ${stats.orders} orders and ${stats.trades} trades; the recording replays to the same session`);
} finally {
  server.kill();
  rmSync(dir, { recursive: true, force: true });
}
