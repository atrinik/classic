# Unsupported access backend fixture

This isolated fixture compiles the production Windows access backend using the
real server settings, access API and protocol headers. It does not build the
server application or establish native Windows gameplay acceptance.

From an owned Classic worktree in the pinned CPU/Windows build worker:

```sh
cmake -S server/src/tests/access_unsupported -B /tmp/access-native -G Ninja
cmake --build /tmp/access-native
ctest --test-dir /tmp/access-native --output-on-failure
x86_64-w64-mingw32.shared-cmake -S server/src/tests/access_unsupported -B /tmp/access-windows -G Ninja
cmake --build /tmp/access-windows
```

Use private writable `CCACHE_DIR` and `CCACHE_TEMPDIR` paths for the MXE compiler.
The CMake configuration checks actual Linux/Windows backend source selection.
The native executable covers all 16 combinations of required, initialize, store
and inherited data descriptor settings for both public and private visibility, plus rejected
admin/auth requests, output clearing and save-failure fencing. Only all-default
open settings succeed. Cross-compilation checks the real MinGW declarations and
links every public server-backend entry point; executing the Windows binary
requires a separately qualified native Windows environment.

The backend deliberately defines no store or state-lock functions. Offline
inspection/bootstrap executables remain Linux-only. Startup must additionally
reject Windows access settings before listeners or state-changing initialization;
this isolated fixture does not prove that full application ordering.
