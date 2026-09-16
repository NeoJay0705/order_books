# Contributing

Thank you for contributing to `order_books`. The repository is currently a project
scaffold; product behavior is intentionally not implemented yet.

## Before making a change

Read [`docs/project-design.md`](docs/project-design.md). It is the implementation source
of truth for the current phase. Do not add product APIs, empty domain modules, or new
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
and its impact and trade-off. Do not silently widen the project scope.

## Pull requests

- Keep each change focused and explain its observable behavior.
- Add or update tests for behavior introduced by the change.
- Keep generated build, Conan, IDE, and local cache files out of commits.
- Do not claim order book functionality until its domain design and tests exist.
