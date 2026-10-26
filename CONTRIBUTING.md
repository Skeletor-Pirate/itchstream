# Contributing

## Build and test

```bash
# Debug + ASan + UBSan + warnings as errors: the correctness gate.
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DITCHSTREAM_SANITIZE=ON -DITCHSTREAM_WERROR=ON
cmake --build build -j
ctest --test-dir build --output-on-failure

# Release: no sanitizers, no -march=native unless you are benchmarking.
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DITCHSTREAM_TESTS=OFF
cmake --build build-rel -j
```

On Windows the supported path is CMake with MSVC or a MinGW/LLVM toolchain; no
published number comes from it.

## Fuzz

```bash
cmake -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DITCHSTREAM_FUZZER=ON \
    -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz -j
./build-fuzz/itchstream_fuzz data/synthetic_seed.itch -max_total_time=60
```

The always-built deterministic driver (`itchstream_fuzz_driver`) is part of
`ctest`. It logs `(seed, iteration)` before every mutation, so a crash replays
from two numbers.

## Benchmarks

```bash
bash scripts/bench.sh [file] [repeats] [core] [build-dir]
```

The script copies the input onto the Linux filesystem, pins the process, warms
the file, discards one run and reports the median of the rest, and prints the
machine and flag block that must accompany the number.

## Rules that are not preferences

- **No market data in git.** [`data/README.md`](../data/README.md) says where data
  comes from; the only committed data file is the 7 KB synthetic seed.
- **Never publish a bare number.** It needs the machine, compiler, flags, power
  state and the command that produced it. `scripts/bench.sh` exists so a number
  and its method cannot drift apart.
- **Sanitizers stay off in Release.** Turning them on to "stabilise" a Release
  number publishes invented latency instead.
- **A new message type needs a wire-size `static_assert` and a field test.** The
  point of the project is that the specification table is checked, not trusted.
- **Determinism.** The same bytes must produce the same counts and the same
  output. `--repeat`, `scripts/bench.sh`, the replay byte comparison and CI all
  check that property from different directions.
- **Malformed input is a test case, not a crash.** Truncation, zero lengths,
  oversized prefixes and every prefix of a valid stream are already covered.

## Commits

Subject prefixes used in this repository: `feat:`, `fix:`, `docs:`, `test:`,
`build:`, `chore:`. Bodies say what was verified and how, in the same style as
the existing history.

## Pull requests

Run the sanitizer suite and the seeded fuzz driver first. If a change moves a
published number, say which number and on what machine. If it touches the wire
format, point at the test that pins it.
