# Atrinik editor packaging

This repository preserves the committed packaging utility for building the
Atrinik edition of Gridarta from a separate Gridarta source checkout. It does
not contain Gridarta source code or prebuilt JAR files.

```sh
./build.sh --output build /path/to/gridarta
```

The script performs no repository update, upload, or remote synchronization.
It runs the upstream Gradle build and copies the resulting JAR and optional
update metadata into the selected local output directory.

## License

The extracted utility retains its GNU General Public License terms. See the
monorepo's root [`LICENSE.md`](../LICENSE.md).
