## Exchange Matching Engine

C++23 limit order book — price-time priority · STP · IOC/GTC/post-only · PMR arena · lock-free SPSC · 17M+ matched pairs/sec

**→ [View full writeup](https://htmlpreview.github.io/?https://github.com/ashishtyagi-gh/exchange/blob/main/index.html)**

---

### Quick start

```bash
# Dependencies (Ubuntu 24.04)
sudo apt install g++-13 cmake ninja-build libgtest-dev libgmock-dev

# Debug + tests
cmake -G Ninja -S . -B build_dbg -DCMAKE_BUILD_TYPE=Debug
cmake --build build_dbg -j$(nproc)
./build_dbg/tests   # 23 tests passing

# Release + benchmarks
cmake -G Ninja -S . -B build_rel -DCMAKE_BUILD_TYPE=Release
cmake --build build_rel -j$(nproc)
./build_rel/bench
```

### Structure

```
include/        order_book.hpp, types.hpp, consumer.hpp, market_arena.hpp, ...
src/            main.cpp  (JSON CLI)
test/           test_matching.cpp, test_multi_market.cpp, test_arena.cpp
bench/          bench_throughput.cpp
docs/           arch.svg, design.svg
README.html     full design writeup (dark theme, SVG diagrams)
```
