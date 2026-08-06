# Dependency boundaries

The client consumes three independently released inputs:

- `cmake/dependencies.lock.json` pins the canonical game protocol and
  `libatrinik` source archives by release tag, commit, URL, and SHA-256.
- `dependencies.lock.json` pins the sound archive with the same metadata.
- `tools/dependencies.py` installs sound into the ignored `sound/` runtime
  directory and refuses unmanaged destinations or unsafe archive members.

CMake's standard `FETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL` and
`FETCHCONTENT_SOURCE_DIR_LIBATRINIK` overrides are the supported local
development seam. Release and CI builds use the locked artifacts.

