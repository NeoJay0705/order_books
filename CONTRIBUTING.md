# Contributing

Thank you for contributing to `order_books`. The repository contains a deterministic
matching core, local persistence, and a multi-shard runtime; keep changes aligned with
the requirements and design documents.

## Before making a change

Read [`docs/Order-book-spec.md`](docs/Order-book-spec.md) for requirements and
[`docs/order-book-design.md`](docs/order-book-design.md) for the current implementation
source of truth. [`docs/project-design.md`](docs/project-design.md) describes the initial
repository skeleton only. Do not add product APIs, empty domain modules, or new
third-party dependencies without a corresponding requirement and design update.

## Local checks

Follow the setup and build commands in [`README.md`](README.md). Before submitting a
change, run the relevant build and test presets, then run formatting and clang-tidy when
the change affects C++ or CMake target configuration.

CI treats compiler warnings as errors. Keep compiler flags target-scoped and avoid
global flag mutation.

## Design deviations

If implementation exposes a contradiction or omission in the design, update the design
in the same change. Record the original decision, the observed problem, the adjustment,
and its impact and trade-off. Persistence format changes must include a version and
round-trip/corruption coverage. Do not silently widen the project scope.

## Pull requests

- Keep each change focused and explain its observable behavior.
- Add or update tests for behavior introduced by the change.
- Keep generated build, Conan, IDE, and local cache files out of commits.
- Keep product behavior changes covered by deterministic, persistence, and runtime tests.
