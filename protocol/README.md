# Atrinik protocol

This directory is the canonical source for Atrinik's classic game wire command
IDs.
It generates matching C and Python bindings so the client, server, tests, and
future automation do not maintain independent numeric constants.

The game and metaserver protocols are separate contract families. This package
currently publishes only the classic game command registry. Add another family
only with its own namespace, specification, version, fixtures, and validation.

Regenerate bindings after editing the schema:

```sh
python3 tools/generate.py
python3 -m unittest discover -s tests -p 'test_*.py'
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```
