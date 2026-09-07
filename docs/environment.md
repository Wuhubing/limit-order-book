# Environment — Stage A Baseline

## Upstream

- Repo: `brprojects/Limit-Order-Book` (MIT license)
- Remote: `https://github.com/brprojects/Limit-Order-Book.git`
- Upstream baseline commit on `main`: `af6e5349874649fe196bd6c26653d357f5a751f2`
  (merge `"Merge branch 'main' of github.com:brprojects/central-limit-order-book"`, 2024-06-12 22:53:48 +0100)

## Host

- OS: macOS 26.4.1 (Build `25E253`)
- Kernel: `Darwin mac 25.4.0` (`arm64`, Apple silicon, T6041 / M4 Pro)
- Shell: zsh

## Toolchain

| Tool | Version | Notes |
|------|---------|-------|
| Apple clang | `Apple clang version 16.0.0 (clang-1600.0.26.6)` | Target `arm64-apple-darwin25.4.0`, from `/Applications/Xcode.app/.../XcodeDefault.xctoolchain` |
| cmake | `4.4.3` | `/opt/homebrew/bin/cmake` |
| ninja | `1.13.2` | `/opt/homebrew/bin/ninja` |
| python3 | `3.13.2` (Homebrew) | also `/opt/anaconda3/bin/python3` available |

Note: `cmake` and `ninja` live in `/opt/homebrew/bin`, which is not on the default
`PATH` for this shell. All build commands below prepend it via
`export PATH="/opt/homebrew/bin:$PATH"`.

## Dependencies

- googletest: NOT vendored in the repo (the `googletest/` directory is gitignored
  and absent). Bootstrapped at configure time via CMake `FetchContent` at tag
  `v1.14.0` (see `CMakeLists.txt`; this is the only build-system change made for
  this stage).

## Exact build commands (as run)

```sh
# configure (clean build dir)
export PATH="/opt/homebrew/bin:$PATH" && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# build
export PATH="/opt/homebrew/bin:$PATH" && cmake --build build -j8
```

Result: configure succeeds (fetches and configures googletest), and the build
produces:

- `build/libLimitOrderBook_lib.a` (library target `LimitOrderBook_lib`)
- `build/LimitOrderBook` (executable target `LimitOrderBook` from `main.cpp`)
- `build/test/LimitOrderBookTests` (the unit-test binary)

The `LimitOrderBook` executable was built but NOT run: `main.cpp` reads
`./initialOrders.txt` and `./Orders.txt` from the working directory, which are not
present in the repo. Building it is fine; running it is out of scope for this
stage (see `baseline-tests.md`).

## Test command (as run)

```sh
export PATH="/opt/homebrew/bin:$PATH"
mkdir -p .agent/results
./build/test/LimitOrderBookTests --gtest_color=no \
    --gtest_output=xml:.agent/results/LOB-001-unit.xml \
    > .agent/results/LOB-001-unit.log 2>&1
# exit code 0, no crash/signal
```

Raw stdout/stderr and the gtest XML report are saved under `.agent/results/`
(gitignored).
