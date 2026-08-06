# Security policy

Report vulnerabilities privately through GitHub's security-advisory interface
for this repository. Do not open a public issue containing exploit details,
credentials, private keys, or player data.

Release archives are trusted only after SHA-256 verification. The dependency
extractor rejects absolute paths, parent traversal, links, special files,
duplicate destinations, and unexpected archive roots. Treat changes to lock
validation, archive extraction, authentication, networking, asset serving,
plugin loading, and mutable `data/` handling as security-sensitive.

Never commit `server-custom.cfg`, `data/`, join-password files, QUIC identities,
metaserver keys, credentials, or generated runtime trees.
