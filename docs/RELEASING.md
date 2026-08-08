# Unified classic releases

Classic releases will use one version and one source commit for the complete
monorepo. The first consolidation release is planned as `v6.0.0`.

The active historical baseline was rebuilt as one unprefixed sequence. It
starts at `v5.0.19` on commit
`f2cdf68710d157d4fae44a0582972129e6c4db9e` and follows the classic server
release commits through `v5.5.1`. `docs/history/release-tags.json` is the
machine-readable authority. Component-prefixed and archive-namespace tags must
not be recreated.

Before enabling an automatic root release workflow, all of these must be true:

- version discovery works from the monorepo root and follows only unprefixed
  unified classic tags;
- source archives select the intended subtree or full tree deterministically;
- client and server packages build from sibling protocol and libatrinik sources;
- the server image has an explicit root build context that includes siblings;
- protocol wheels and standalone library archives receive the unified version;
- checksums cover every artifact and the wrapper supply-chain inventory records
  the release commit.

The five nested release workflows must not be copied or enabled at the root:
their contexts and independent version trains would publish incorrect assets.
Historical releases remain in the archived source repositories.
