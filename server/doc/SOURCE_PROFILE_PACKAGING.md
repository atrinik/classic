# Source-profile Windows qualification

Official Windows packages continue to require the exact released runtime manifest
approved by the static GPU fixture provenance contract. The workspace wrapper can
instead qualify its selected source cohort with package version `0.0.0`. This is
an explicit unreleased source qualification, not official release acceptance or a
claim that all selected content gameplay has been exercised.

The wrapper supplies both staged `ATRINIK_PROFILE_CONTENT_DIR` and
`ATRINIK_PROFILE_RESOURCES_DIR`, plus `ATRINIK_SOURCE_PROFILE_QUALIFICATION` (an
absolute regular JSON file) and `ATRINIK_SOURCE_PROFILE_QUALIFICATION_SHA256`.
It captures these coordinates under its source leases and mounts the staged
inputs read-only. Ambient values are not a substitute for wrapper provenance.
The closed JSON schema is:

```json
{
  "schema_version": 1,
  "kind": "classic-source-profile-qualification",
  "package_version": "0.0.0",
  "source": {
    "classic": {"commit": "<40 lowercase hexadecimal characters>", "dirty": false},
    "content": {"commit": "<40 lowercase hexadecimal characters>", "dirty": false},
    "resources": {"commit": "<40 lowercase hexadecimal characters>", "dirty": false}
  },
  "content_manifest_sha256": "<64 lowercase hexadecimal characters>"
}
```

The native verifier always validates static GPU fixtures against their released
pin. Source mode separately verifies `manifest.json` has `release_version` equal
to `unreleased`, the attested content commit on `atrinik/content` branch `main`,
its exact digest, schema and celestial compatibility, complete sorted inventory,
and every file size and digest. Its archetype artifact must exactly match the
one approved for the static fixtures. Symlinks and unexpected files fail; only
the existing `.atrinik-dependency.json` metadata exception is retained. The
wrapper removes its validated private management marker before staging.

The package includes the exact public attestation at
`server/attribution/source-profile-qualification.json`; verifier output includes
`source_profile_qualification` and an explicitly unreleased runtime. The
attestation contains only public source commits, dirty flags and a digest.
Resources are a regular, nonempty staged tree whose source lease and inventory
are owned by the wrapper. Native verification does not turn a self-authored
attestation into an independently trusted release signature.

Without all explicit qualification inputs the default released-runtime checks
remain strict. Qualification flags cannot admit a nonzero package version, and
the official release workflow does not set them.
