# Unified classic releases

Classic releases will use one version and one source commit for the complete
monorepo. The first consolidation release is planned as `v6.0.0`.

Before enabling an automatic root release workflow, all of these must be true:

- version discovery works from the monorepo root and ignores namespaced
  component history tags;
- source archives select the intended subtree or full tree deterministically;
- client and server packages build from sibling protocol and libatrinik sources;
- the server image has an explicit root build context that includes siblings;
- protocol wheels and standalone library archives receive the unified version;
- checksums cover every artifact and the wrapper supply-chain inventory records
  the release commit.

The five nested release workflows must not be copied or enabled at the root:
their contexts and independent version trains would publish incorrect assets.
Historical releases remain in the archived source repositories.
