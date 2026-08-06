# Contributing

Open changes through a pull request. Pull-request titles must use Conventional
Commits style, such as `fix(packet): reject a truncated header`. The required
library validation must pass before merge.

Public API changes need tests and an explicit account of ownership, error,
lifetime, thread-safety, and compatibility consequences. Update the protocol
dependency lock only to a published immutable release whose checksum has been
verified independently.

