# Contributing

Open changes through a pull request whose title uses Conventional Commits
style. Run the dependency validation, CMake build, and CTest suite documented
in INSTALL. Preserve license and attribution files when changing bundled
graphics or fonts.

The squash-merge title is also the release input: breaking changes bump major,
`feat` bumps minor, and all other conventional types bump patch.

Update source and sound locks only to immutable published releases after
independently verifying their checksums.
