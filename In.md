# Exchange Take Home Exercise

## [](#Overview)Overview
Your task is to build a **matching engine** for a single trading pair on an exchange. The engine must be deterministic, correct under contention, and have a clean, well-bounded interface.

This exercise is designed to take **2 hours**. We care about correctness, clean invariants, and how you reason about tradeoffs — not about covering every edge case. Be explicit about what you chose to skip.

### [](#Rules-of-engagement)Rules of engagement

- Use any language you prefer. Rust, Go, TypeScript, and C++ are all fine.

- You may use Claude, Cursor, or any AI tool.

- The end product should be a **git repo plus a short writeup** (markdown is fine) describing your design, invariants, and tradeoffs.

- You do not need to implement auth, persistence, recovery, accounts/collateral, or an HTTP/WebSocket server. Focus on the matching engine itself.

- Email clarifying questions to **maanav@infiniteworlds.xyz**.

---

## [](#Part-1-—-Single-Market-Matching-Engine)Part 1 — Single-Market Matching Engine
Implement a continuous limit order book for a single market (e.g. `BTC`) with price-time priority.

### [](#The-engine-must-support)The engine must support

- **Place order** — limit and market orders, both sides.

- **Cancel order** — by order id.

- **Query** — top of book, full book depth, and the status of a specific order.

Orders have, at minimum: `order_id`, `account_id`, `side`, `price` (null for market), `size`, `time_in_force`. Support at least two TIFs: **GTC** (resting) and **IOC** (match what you can, cancel the rest). **Post-only** is a nice-to-have.

### [](#The-matching-engine-must-enforce)The matching engine must enforce

- **Price-time priority**: best price first; within a price level, earliest order first.

- **No self-trade**: a single `account_id` cannot match against itself. Pick a policy (cancel newest, cancel oldest, cancel both) and document it.

- **Partial fills**: orders that fill partially stay resting (for GTC) with their original priority.

- **Deterministic fill output**: given the same sequence of inputs, the engine produces the same sequence of fills and book states every time. This is the single most important property.

### [](#Interface)Interface
The engine should be callable as a pure function or a single-threaded actor. Provide a small CLI, test harness, or scripted replay entry point so we can drive it with a sequence of orders without importing your code into a test. Plain text or JSON in, fills and book snapshots out — whatever is cleanest.

### [](#What-we-will-look-at)What we will look at

- The core data structures for the book (and why you chose them).

- How you keep the engine deterministic.

- Tests that demonstrate price-time priority, partial fills, IOC behavior, self-trade prevention, and cancel-during-match edge cases.

---

## [](#Part-2-—-Multiple-Markets-and-Throughput)Part 2 — Multiple Markets and Throughput
Extend the system to support **multiple independent markets** (e.g. `BTC`, `CL`, `SILVER`).

You should be prepared to discuss:

- How you shard or serialize work across markets. Is each market its own engine instance? Its own thread? Its own queue?

- What the throughput ceiling of a single market is in your design, and what the bottleneck is.

- How you would produce a consistent per-market event stream (fills, book updates) that a downstream consumer could replay.

- What ordering guarantees you offer across markets vs. within a market.

Implement enough of this to show the shape of the solution. A benchmark or a short written analysis of measured throughput on one market is valuable.

---

## [](#Part-3-—-Toward-a-Distributed-Exchange-optional-design-only)Part 3 — Toward a Distributed Exchange (optional, design only)
Only attempt this if you have time after finishing Parts 1 and 2. No code required — a short design section (roughly one page) is enough.

Suppose this exchange runs matching engines in **two regions** (e.g. Illinois and Tokyo), and each market is owned by exactly one region — so a `BTC` order placed in New York must reach the Tokyo engine to match.

Discuss:

1. **Order routing and acknowledgement**: what does a New York client see and when? What are the latency floors and what determines them?

2. **Determinism and replay**: how would you log events so that the engine's state is reconstructable after a crash, and so that a replica in another region could follow along?

3. **Failure modes**: what happens if the link between regions degrades or the owning region goes down? Can the other region take over? What are the risks of doing so?

4. **What would change** in your Part 1 and Part 2 design if you had known from the start that the engine would run in this distributed setting?

Be concrete about what is easy, what is hard, and what tradeoffs you would push back on.

---

## [](#Deliverables)Deliverables

- A **git repo** with the code and tests.

- A **short writeup** (markdown, bullet points are fine — no need for formality) covering:

How to build and run.

- Your design: data structures, concurrency model, determinism strategy.

- Explicit tradeoffs you made, especially ones driven by time constraints.

- What you would change for a production setting.

- A rough sense of how you spent your time across the parts.

- Part 3 writeup if you attempted it.

The writeup does not need to be polished — we mostly want to understand how you were thinking.