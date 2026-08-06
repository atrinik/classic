#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 || ! $1 =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "usage: $0 MAJOR.MINOR.PATCH" >&2
  exit 2
fi

tag=v$1
for attempt in {1..6}; do
  if gh workflow run package-release.yml \
      --repo "${GITHUB_REPOSITORY}" --ref main --field "tag=${tag}"; then
    exit 0
  fi
  ((attempt < 6)) || exit 1
  sleep 5
done
