# Fixed Windows WARP symbol qualification

The optional `warp_symbols_only` boolean on **Release Rehearsal** defaults to
false. Normal dispatches retain the current-main rehearsal checks and packaging
graph. An explicitly true dispatch runs only the Windows symbol diagnostic at
the exact dispatched source revision. It cannot authorize release publication
or replace native Windows client qualification.

This diagnostic resolves nine WARP addresses captured by the owned-client
timeout observer in Check run `38212864197`, job `114692900077`, source
`554efe80e24e4196d04a507ae5bfa5902972bfce`. The helper binds the log SHA256,
same-operation module SHA256, PE coordinates, PDB GUID/age, and fixed RVAs in
every report. It does not map earlier captures lacking an authenticated module
identity.

Acquisition starts with this PDB's fixed canonical HTTPS Microsoft public symbol
URL. Exactly one redirect from that URL may reach the exact HTTPS hostname
`vsblobprodscussu5shard18.blob.core.windows.net`, observed in hosted run
`38248071891`, job `114806917500`. Microsoft documents its Azure-based symbol
store; this host admission comes from the actual canonical-server response,
not a blanket Azure-domain rule. The hop rejects userinfo, explicit ports,
fragments, control characters, backslashes, and URLs over 4096 characters.
Redirect bodies are closed without being read. Further hops, other hosts,
proxies, HTTP authentication, and URL overrides are disabled. The storage
request carries only fresh fixed headers; its opaque signed query stays in
memory and is never logged or uploaded. The final response URL must equal the
admitted hop, and the downloaded PDB must still pass GUID/age validation.
Rejected redirects or unavailable PDBs remain inconclusive and require separate
review. Only a bounded HTTPS hostname is retained for rejected-redirect review.
Acquisition checks a cooperative 90-second monotonic deadline between reads,
uses a 10-second socket timeout, and accepts at most 256 MiB. The parent kills and
reaps its Python worker after 120 seconds, covering network reads and native
DbgHelp calls. MSF directory and info-stream allocations are capped at 4 MiB
and 1 MiB. All referenced stream blocks are range/overlap checked and the
embedded GUID/age must match before DbgHelp runs.

The worker loads OS System32 DbgHelp, uses an isolated local-only symbol path,
and resolves the fixed RVAs with a synthetic module base and captured image
size. It never attaches to another process or loads the game or WARP executable
image. No application, renderer setting, close deadline, SDK, or package changes
are involved. Only the bounded JSON report is uploaded; the temporary PDB is
deleted by the parent and is never redistributed as an artifact.

`resolved` means all nine addresses produced bounded symbol names and
consistent displacements. Public symbols can be incomplete, and a nearest
symbol plus displacement does not establish a function's extent. One sampled
stack cannot establish which operation occupied the entire close interval or
prove JIT attribution, recovery, or correct shutdown. Missing, mismatched,
malformed, timed-out, or unresolved inputs fail the diagnostic explicitly.

Run the pure fixtures without network or native APIs:

```sh
python3 -B -m unittest discover -s tools/tests -p test_qualify_warp_symbols.py -v
```

Native DbgHelp and the public PDB acquisition require the separately authorized
hosted dispatch. Linux fixture success is not native acceptance.

Format and API references:

- [LLVM MSF format](https://llvm.org/docs/PDB/MsfFile.html)
- [LLVM PDB info stream](https://llvm.org/docs/PDB/PdbStream.html)
- [Microsoft public symbols](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/microsoft-public-symbols)
- [Microsoft Azure-based symbol store](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/debugger-download-symbols)
- [SymLoadModuleEx](https://learn.microsoft.com/en-us/windows/win32/api/dbghelp/nf-dbghelp-symloadmoduleex)
- [SYMBOL_INFO](https://learn.microsoft.com/en-us/windows/win32/api/dbghelp/ns-dbghelp-symbol_info)
