# Offline bake dependencies

Install the clone's `requirements.txt` before running the bake. Its pinned
`zstandard` package compresses the synthetic fixtures (including raw-content
dictionaries) and reads compressed containers on Windows and Linux. Enumeration
also supports standalone system `libzstd` when that Python package is absent.
Without the package, fixture generation retains its external `zstd` fallback;
dictionary fixtures require that executable. Missing dependencies are errors,
not successful enumeration of an incomplete fixture.

Frames without a declared content size are counted in bounded chunks before
complete-frame validation, rather than reserving the maximum output limit for a
small frame. The regression reads a 2 MB frame under a 512 MB address-space limit
on Linux and also checks truncated unknown-size frames.

The translator needs a C++23 compiler. `BAKE_CXX` explicitly selects one; without
it, the existing discovery chooses an available Clang/GCC compiler. The language
probe tests the required constexpr failure path: the static assertion counter
does not prevent successful calls from being constant expressions. No C++20
fallback is used to conceal an unsupported compiler. Compiler discovery and cache
identity improvements are coordinated separately with PR #1872.

`tests/test_bake_portability.py` exercises real dictionary frames, malformed
frames, native-reader fallback and the compiler's constexpr capability.
The existing DS, Mimo and skeleton checks retain their synthetic enumeration and
translation assertions. DS enumeration without its optional offline translator
reports translation as skipped; it does not certify the translation/bank step.
These are CPU-only checks and do not establish GPU pipeline or gameplay coverage.
