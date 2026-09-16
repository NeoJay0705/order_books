# order_books

`order_books` is a C++20 project scaffold for a future order book implementation.
The first phase intentionally contains no order book business logic, public product API,
CLI, service, or deployment image. It validates the cross-platform C++ development and
testing toolchain that future domain work will use.

## Current status

- Language: C++20
- Build system: CMake 3.25 or newer with Ninja
- Dependency manager: Conan 2
- Test framework: GoogleTest
- macOS development: Apple Clang
- Linux validation: GCC and Clang
- License: Apache-2.0

The design constraints and first-phase acceptance criteria are documented in
[`docs/project-design.md`](docs/project-design.md). Tool comparisons and rationale are
in [`docs/technical-decisions.md`](docs/technical-decisions.md).

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

Check formatting without modifying files:

```bash
clang-format --dry-run --Werror tests/toolchain_smoke_test.cpp
```

The CI workflow runs the equivalent build, test, sanitizer, clang-tidy, and formatting
checks. Formatting fixes are intentionally explicit:

```bash
clang-format -i tests/toolchain_smoke_test.cpp
```

## Repository scope

This phase does not define order book data structures, matching rules, order types,
threading, persistence, networking, performance targets, or a stable C++ API. Those
decisions require product use cases and will be added to the design before product code
is introduced.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the development workflow and the rules for
recording necessary design deviations.
