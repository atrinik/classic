#!/usr/bin/env bash
# Require release-tag provenance from Package Release for every subject.
#
# Consumers pin the tag commit and ref. GitHub signs each attestation with its
# own workflow run's ref and commit, so a subject passes only when a tag-ref
# Package Release run attested it: the current run, or the tag-ref candidate
# run of a retained-candidate recovery.

set -euo pipefail

fail() {
  echo "tag provenance: $*" >&2
  exit 1
}

if [[ $# -eq 0 ]]; then
  echo "usage: $0 SUBJECT..." >&2
  exit 2
fi
[[ ${RELEASE_REPOSITORY:-} =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] ||
  fail "RELEASE_REPOSITORY must be OWNER/REPOSITORY"
[[ ${RELEASE_TAG:-} =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
  fail "RELEASE_TAG must be vMAJOR.MINOR.PATCH"
[[ ${RELEASE_COMMIT:-} =~ ^[0-9a-f]{40}$ ]] ||
  fail "RELEASE_COMMIT must be a full commit SHA"
tag_commit=$(git rev-parse --verify --quiet "refs/tags/${RELEASE_TAG}^{commit}") ||
  fail "${RELEASE_TAG} is not a local tag"
[[ ${tag_commit} == "${RELEASE_COMMIT}" ]] ||
  fail "${RELEASE_TAG} resolves to ${tag_commit}, not ${RELEASE_COMMIT}"

policy=(
  --repo "${RELEASE_REPOSITORY}"
  --signer-workflow "${RELEASE_REPOSITORY}/.github/workflows/package-release.yml"
  --source-digest "${RELEASE_COMMIT}"
  --source-ref "refs/tags/${RELEASE_TAG}"
  --deny-self-hosted-runners
)
for subject in "$@"; do
  for attempt in 1 2 3; do
    if gh attestation verify "${subject}" "${policy[@]}" >/dev/null; then
      break
    fi
    ((attempt < 3)) || fail "no tag-bound provenance for ${subject}"
    sleep 10
  done
done
