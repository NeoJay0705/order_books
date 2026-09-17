# order_books

`order_books` is a C++20, embeddable order book and matching-engine library. The first
implementation slice provides the deterministic core, versioned binary persistence, and
single-process multi-shard runtime described in
[`docs/order-book-design.md`](docs/order-book-design.md).

## Current status

- Language: C++20
- Build system: CMake 3.25 or newer with Ninja
- Dependency manager: Conan 2
- Test framework: GoogleTest
- macOS development: Apple Clang
- Linux validation: GCC and Clang
- License: Apache-2.0

The requirements are documented in [`docs/Order-book-spec.md`](docs/Order-book-spec.md),
and the current implementation source of truth is
[`docs/order-book-design.md`](docs/order-book-design.md). The initial repository
skeleton and tool comparisons remain in [`docs/project-design.md`](docs/project-design.md)
and [`docs/technical-decisions.md`](docs/technical-decisions.md).

## Prerequisites

Install the following tools before configuring the project:

- CMake 3.25 or newer
- Ninja
- Python 3.9 or newer
- Conan 2
- A C++20 compiler
- clang-format and clang-tidy for quality checks

The supported development compiler is Apple Clang on macOS. Linux CI validates GCC and
Clang. The project does not currently promise a particular Linux distribution, glibc
version, CPU architecture, or production ABI.

## Configure Conan

From the repository root, create a local Conan profile if one does not already exist:

```bash
conan profile detect --force
```

Conan generates CMake toolchain and dependency files under the configuration-specific
`build/<BuildType>/generators` directory. These generated files are local build output
and must not be committed.

## Build and test

Install the test dependency for each configuration before using its CMake preset:

```bash
conan install . --build=missing -s build_type=Debug \
  -s compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja
conan install . --build=missing -s build_type=Release \
  -s compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja
```

Configure, build, and test the Debug configuration:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The Release configuration uses the same workflow:

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The benchmark is opt-in and has no performance gate:

```bash
cmake --preset release-benchmark
cmake --build --preset release-benchmark
./build/ReleaseBenchmark/benchmarks/order_books_benchmark --iterations=20 --warmup=5
```

To measure a single instrument through the durable Engine path (queue, group commit, WAL
append, `fsync`, matching, invariant validation, and completion callback), run the dedicated
workload on a real Linux WAL device:

```bash
./build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --workload=engine_durable_single_instrument \
  --iterations=1000000 \
  --warmup=10000 \
  --data-dir=/mnt/local-nvme/order-books-benchmark/run-001
```

`commands_per_second` is calculated from committed completion callbacks, not from
`submit()` calls that only report `queued=true`. Use a new empty data directory for each
run; do not use `tmpfs` or an overlay filesystem for a Linux production baseline.

To measure the WAL write ceiling independently from Engine, run the dedicated workload on
the same Linux filesystem used for deployment:

```bash
./build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --workload=wal_write_ceiling \
  --iterations=10000 \
  --warmup=1000 \
  --wal-group-size=256 \
  --wal-sync=per_group \
  --data-dir=/mnt/local-nvme/order-books-benchmark/wal-run-001
```

Use `--wal-sync=none` only to isolate the append-return path; it is not durable throughput.
The `per_group` result counts a command only after its group `fsync` succeeds. Each run must
use a new empty directory; the benchmark reopens and replays the WAL before printing a
successful summary.

To measure the individual Engine pipeline ceilings, use the diagnostic workload. It keeps
the existing production implementations and does not change runtime defaults:

```bash
./build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --workload=engine_pipeline_ceiling \
  --pipeline-stage=all \
  --iterations=20 \
  --warmup=4 \
  --pipeline-batch-size=64 \
  --pipeline-active-orders=1000
```

`state_machine`, `invariant_validation`, and `metrics` are cache-warm component ceilings;
`runtime_handoff` and `publisher_drain` use the real Engine/worker paths. The publisher stage
waits for the durable cursor to reach the WAL head before reporting success. For a durable
group-commit matrix, the existing Engine workload accepts benchmark-only overrides:

```bash
./build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --workload=engine_durable_single_instrument \
  --engine-group-size=512 \
  --engine-group-delay-us=200 \
  --iterations=10000 \
  --warmup=1000 \
  --data-dir=/mnt/local-nvme/order-books-benchmark/engine-group-512
```

Run each filesystem case in a new empty directory and compare at least five Release runs.
Component ceilings, WAL durable throughput, publisher drain throughput, and Engine completion
throughput are separate measurements; none can substitute for the end-to-end result.

## Quality checks

Run ASan and UBSan using the Debug Conan dependencies:

```bash
cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers
```

Run clang-tidy through the explicit preset:

```bash
cmake --preset debug-clang-tidy
cmake --build --preset debug-clang-tidy
```

The current CI formatting smoke check is non-mutating:

```bash
clang-format --dry-run --Werror tests/toolchain_smoke_test.cpp
```

The CI workflow runs the equivalent build, test, sanitizer, clang-tidy, and formatting
checks. Formatting fixes are intentionally explicit:

```bash
clang-format -i tests/toolchain_smoke_test.cpp
```

## Current scope

The current API supports limit GTC New, Amend, Replace, Cancel, price-time matching,
queries, producer idempotency, WAL/Snapshot recovery, and an at-least-once `EventSink`.
Networking, Kafka adapters, risk, replication/HA, and additional order types remain out
of scope. The C++ API is not yet an installed or ABI-stable package.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the development workflow and the rules for
recording necessary design deviations.
