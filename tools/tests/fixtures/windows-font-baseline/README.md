`stage_windows_runtime.py.txt` is the byte-identical historical helper from
`fd4943252ac872fce2995c286b10a0c6d249aba3:tools/ci/stage_windows_runtime.py`.
Its SHA-256 is `12d315f854726e46ec768c5c4db6a34b0aabb93e5cab959233f5c5ec1650aeca`.

This frozen fixture tests the bounded historical font adapter without requiring
Git history in a shallow CI checkout. Production still acquires the helper from
that immutable revision and verifies its bytes before executing them. Candidate
LLVM runtime staging uses the current helper and its producer inventory.
