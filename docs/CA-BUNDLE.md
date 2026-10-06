# Public CA bundle

`client/ca-bundle.crt` and `server/ca-bundle.crt` are identical, unmodified copies
of the [dated curl/Mozilla CA bundle](https://curl.se/ca/cacert-2026-09-25.pem).
The source header records Mozilla data from 2026-09-25 and conversion with
`mk-ca-bundle.pl` 1.33. The bundle contains 121 certificates and 188,900 bytes.

SHA-256: `a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505`.
This matches the [official dated checksum](https://curl.se/ca/cacert-2026-09-25.pem.sha256).

The data remains under MPL-2.0. The license and source notice is in the shipped
[ATTRIBUTIONS.md](../ATTRIBUTIONS.md); the official PEM header is preserved.
The SHA-256 embedded in that upstream header describes its conversion input,
not the SHA-256 of the complete distributed PEM file recorded above.

This replaces the 2014 bundle that lacked current public trust roots and failed
the verified HTTPS qualification. It adds no private CA, identity, automatic
download, verification bypass, or change to platform trust-store selection.
The PEM extraction does not carry Firefox's additional per-root constraints;
see the [upstream limitation](https://curl.se/docs/caextract.html).

Updates require a reviewed dated upstream bundle downloaded through verified
HTTPS, comparison with its official checksum, matching client/server copies,
and refreshed provenance and tests. Run the offline integrity/loading check:

```sh
python3 -m unittest discover -s tools/tests -p test_ca_bundle.py
```

Actual public HTTPS success and untrusted-CA rejection must also be qualified
with the selected native libcurl/TLS provider; an offline PEM check is not a
network or provider qualification.
