# Contributing to lapse

Thank you for helping improve `lapse`. Bug reports, documentation fixes, tests,
and focused code changes are all welcome.

## Before changing code

Search the [issue tracker](https://github.com/bell-kevin/lapse/issues) for an
existing report. For a substantial behavior or format change, open an issue
first so the approach can be discussed before implementation.

Do not report vulnerabilities in a public issue. Follow
[SECURITY.md](SECURITY.md) instead.

## Build and test

The project requires a C++17 compiler; its integration tests require
Python 3.9+. The shortest local validation path is:

```sh
make
make test
```

The equivalent CMake path is:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

If a change affects the repository structure or documentation inventory,
regenerate and verify the architecture diagram:

```sh
python3 scripts/generate_architecture_diagram.py
python3 scripts/generate_architecture_diagram.py --check
```

## Pull requests

- Keep each pull request focused on one problem.
- Match the style and error-handling conventions of the surrounding code.
- Add or update tests for user-visible behavior.
- Run the relevant build, integration test, and diagram check before submitting.
- Explain the motivation, behavior change, and validation in the description.

By contributing, you agree that your contribution is licensed under the
repository's [AGPL-3.0-only license](LICENSE).
