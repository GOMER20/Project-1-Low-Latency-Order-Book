// A client for HFT in Node.js, with no dependencies: the reference for how a
// program in another language talks to it. The messages are laid out in
// ../wire.hpp; this file packs and unpacks the same bytes.
//
//   const hft = new HftClient('/path/to/hft.sock');
//   await hft.connect();
//   await hft.hello([{ symbol: 'AAPL', minPrice: 1, numLevels: 100_000, maxOrders: 16_384 }]);
//   hft.on('fill', (fill) => { ... });
//   hft.quote(0, [{ price: 22849, quantity: 500 }], [{ price: 22851, quantity: 400 }]);
//   hft.order({ tag: 1n, symbol: 0, side: Side.Buy, price: 22851, quantity: 100 });
//
// Prices are whole numbers of ticks (cents, for Gomer Trading) and come back
// as ordinary numbers. Tags and order IDs are 64-bit and come back as BigInt;
// either a number or a BigInt may be passed in.
//
// Orders, cancels and quotes are not awaited: their answers arrive as events
// ('accepted', 'rejected', 'fill', 'cancelled', 'cancelRejected'), in the order
// HFT dealt with them. hello(), depth(), stats() and ping() return promises.
import { EventEmitter } from 'node:events';
import net from 'node:net';

export const PROTOCOL_VERSION = 1;
export const MAX_LEVELS = 32;

export const Side = Object.freeze({ Buy: 0, Sell: 1 });
export const OrderType = Object.freeze({ Limit: 0, Market: 1, IOC: 2, FOK: 3 });

const Kind = Object.freeze({
  Hello: 1,
  Order: 2,
  Cancel: 3,
  Quote: 4,
  DepthRequest: 5,
  StatsRequest: 6,
  Ping: 7,
  Shutdown: 8,
  Welcome: 101,
  Accepted: 102,
  Rejected: 103,
  Fill: 104,
  Cancelled: 105,
  CancelRejected: 106,
  Depth: 107,
  Stats: 108,
  Pong: 109,
  Error: 110,
});

const HEADER = 8;
const HELLO_FRESH = 1;

/** A whole message: the 8-byte header, then `size` bytes for the caller to fill. */
function message(kind, size) {
  const buffer = Buffer.alloc(HEADER + size);
  buffer.writeUInt16LE(kind, 0);
  buffer.writeUInt32LE(size, 4);
  return buffer;
}

export class HftClient extends EventEmitter {
  #path;
  #socket = null;
  #pending = Buffer.alloc(0);
  // HFT answers in the order it was asked, so the answers to requests of one
  // kind come back in the order those requests were made.
  #waiting = { welcome: [], depth: [], stats: [], pong: [] };
  #pings = 0n;

  constructor(socketPath) {
    super();
    this.#path = socketPath;
  }

  /** Connects to HFT's socket. Rejects if nothing is listening there. */
  connect() {
    return new Promise((resolve, reject) => {
      const socket = net.createConnection(this.#path);
      socket.once('connect', () => {
        socket.off('error', reject);
        socket.on('error', (error) => this.emit('error', error));
        this.#socket = socket;
        resolve();
      });
      socket.once('error', reject);
      socket.on('data', (chunk) => this.#onData(chunk));
      socket.on('close', () => {
        this.#socket = null;
        const closed = new Error('the connection to HFT closed');
        for (const queue of Object.values(this.#waiting)) {
          for (const waiter of queue.splice(0)) waiter.reject(closed);
        }
        this.emit('close');
      });
    });
  }

  get connected() {
    return this.#socket !== null;
  }

  /**
   * Says which books to run. A symbol's number, used everywhere else, is its
   * position in `books`. With `fresh`, HFT starts again with empty books even
   * if it is already running these ones; without, a client that reconnects
   * finds the books as it left them (`resumed` says which happened).
   */
  hello(books, { fresh = false } = {}) {
    const buffer = message(Kind.Hello, 16 + 24 * books.length);
    buffer.writeUInt32LE(PROTOCOL_VERSION, HEADER);
    buffer.writeUInt32LE(books.length, HEADER + 4);
    buffer.writeUInt32LE(fresh ? HELLO_FRESH : 0, HEADER + 8);
    books.forEach((book, index) => {
      const at = HEADER + 16 + 24 * index;
      buffer.write(book.symbol.slice(0, 8), at, 'latin1');
      buffer.writeBigInt64LE(BigInt(book.minPrice), at + 8);
      buffer.writeUInt32LE(book.numLevels, at + 16);
      buffer.writeUInt32LE(book.maxOrders, at + 20);
    });
    return this.#ask('welcome', buffer);
  }

  /** Sends an order. `type` defaults to a limit order; `price` is ignored for a market order. */
  order({ tag, symbol, side, price = 0, quantity, type = OrderType.Limit }) {
    const buffer = message(Kind.Order, 24);
    buffer.writeBigUInt64LE(BigInt(tag), HEADER);
    buffer.writeBigInt64LE(BigInt(price), HEADER + 8);
    buffer.writeUInt32LE(quantity, HEADER + 16);
    buffer.writeUInt16LE(symbol, HEADER + 20);
    buffer.writeUInt8(side, HEADER + 22);
    buffer.writeUInt8(type, HEADER + 23);
    this.#send(buffer);
  }

  /** Cancels a resting order by the ID its 'accepted' event gave. */
  cancel({ tag, symbol, orderId }) {
    const buffer = message(Kind.Cancel, 24);
    buffer.writeBigUInt64LE(BigInt(tag), HEADER);
    buffer.writeBigUInt64LE(BigInt(orderId), HEADER + 8);
    buffer.writeUInt16LE(symbol, HEADER + 16);
    this.#send(buffer);
  }

  /**
   * Says what the house should be bidding and offering in one symbol: arrays
   * of { price, quantity }, best price first. HFT cancels and places house
   * orders until that is what is resting. Each side must run strictly away
   * from its best price and the best bid must be below the best offer, or the
   * quote is ignored.
   */
  quote(symbol, bids, asks) {
    if (bids.length > MAX_LEVELS || asks.length > MAX_LEVELS) {
      throw new RangeError(`a quote carries at most ${MAX_LEVELS} levels a side`);
    }
    const buffer = message(Kind.Quote, 8 + 16 * (bids.length + asks.length));
    buffer.writeUInt16LE(symbol, HEADER);
    buffer.writeUInt8(bids.length, HEADER + 2);
    buffer.writeUInt8(asks.length, HEADER + 3);
    [...bids, ...asks].forEach((level, index) => {
      const at = HEADER + 8 + 16 * index;
      buffer.writeBigInt64LE(BigInt(level.price), at);
      buffer.writeUInt32LE(level.quantity, at + 8);
    });
    this.#send(buffer);
  }

  /** The top of one symbol's book: { symbol, bids, asks }, each [{ price, quantity }], best first. */
  depth(symbol, levels = 10) {
    const buffer = message(Kind.DepthRequest, 8);
    buffer.writeUInt16LE(symbol, HEADER);
    buffer.writeUInt16LE(levels, HEADER + 2);
    return this.#ask('depth', buffer);
  }

  /** HFT's statistics for the session, as an object. */
  stats() {
    return this.#ask('stats', message(Kind.StatsRequest, 0));
  }

  /** Resolves once HFT has dealt with everything sent before it. */
  ping() {
    const buffer = message(Kind.Ping, 8);
    buffer.writeBigUInt64LE(++this.#pings, HEADER);
    return this.#ask('pong', buffer);
  }

  /** Asks HFT to finish its recording, write its statistics and exit. */
  shutdown() {
    this.#send(message(Kind.Shutdown, 0));
  }

  close() {
    this.#socket?.end();
  }

  #send(buffer) {
    if (this.#socket === null) throw new Error('not connected to HFT');
    this.#socket.write(buffer);
  }

  #ask(queue, buffer) {
    return new Promise((resolve, reject) => {
      if (this.#socket === null) {
        reject(new Error('not connected to HFT'));
        return;
      }
      this.#waiting[queue].push({ resolve, reject });
      this.#socket.write(buffer);
    });
  }

  #onData(chunk) {
    this.#pending = this.#pending.length === 0 ? chunk : Buffer.concat([this.#pending, chunk]);
    let at = 0;
    while (this.#pending.length - at >= HEADER) {
      const kind = this.#pending.readUInt16LE(at);
      const size = this.#pending.readUInt32LE(at + 4);
      if (this.#pending.length - at < HEADER + size) break; // the rest has not arrived yet
      this.#onMessage(kind, this.#pending.subarray(at + HEADER, at + HEADER + size));
      at += HEADER + size;
    }
    this.#pending = this.#pending.subarray(at);
  }

  #onMessage(kind, payload) {
    switch (kind) {
      case Kind.Accepted:
        this.emit('accepted', {
          tag: payload.readBigUInt64LE(0),
          orderId: payload.readBigUInt64LE(8),
          filled: payload.readUInt32LE(16),
          resting: payload.readUInt32LE(20),
          symbol: payload.readUInt16LE(24),
        });
        break;
      case Kind.Rejected:
        this.emit('rejected', { tag: payload.readBigUInt64LE(0), symbol: payload.readUInt16LE(8) });
        break;
      case Kind.Fill:
        this.emit('fill', {
          tag: payload.readBigUInt64LE(0),
          orderId: payload.readBigUInt64LE(8),
          price: Number(payload.readBigInt64LE(16)),
          quantity: payload.readUInt32LE(24),
          remaining: payload.readUInt32LE(28),
          symbol: payload.readUInt16LE(32),
          side: payload.readUInt8(34),
          maker: payload.readUInt8(35) === 1,
          withHouse: payload.readUInt8(36) === 1,
        });
        break;
      case Kind.Cancelled:
      case Kind.CancelRejected:
        this.emit(kind === Kind.Cancelled ? 'cancelled' : 'cancelRejected', {
          tag: payload.readBigUInt64LE(0),
          orderId: payload.readBigUInt64LE(8),
          symbol: payload.readUInt16LE(16),
        });
        break;
      case Kind.Welcome:
        this.#waiting.welcome.shift()?.resolve({
          version: payload.readUInt32LE(0),
          bookCount: payload.readUInt32LE(4),
          session: Number(payload.readBigUInt64LE(8)),
          resumed: payload.readUInt8(16) === 1,
        });
        break;
      case Kind.Depth: {
        const bidLevels = payload.readUInt8(2);
        const askLevels = payload.readUInt8(3);
        const level = (index) => ({
          price: Number(payload.readBigInt64LE(8 + 16 * index)),
          quantity: Number(payload.readBigUInt64LE(16 + 16 * index)),
        });
        this.#waiting.depth.shift()?.resolve({
          symbol: payload.readUInt16LE(0),
          bids: Array.from({ length: bidLevels }, (_, index) => level(index)),
          asks: Array.from({ length: askLevels }, (_, index) => level(bidLevels + index)),
        });
        break;
      }
      case Kind.Stats:
        this.#waiting.stats.shift()?.resolve(JSON.parse(payload.toString('utf8')));
        break;
      case Kind.Pong:
        this.#waiting.pong.shift()?.resolve();
        break;
      case Kind.Error:
        // HFT could not understand the last message. It closes the connection next.
        this.emit('error', new Error(`HFT: ${payload.toString('utf8')}`));
        break;
      default:
        break; // a kind from a newer HFT: nothing this client knows what to do with
    }
  }
}
