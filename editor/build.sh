#!/usr/bin/env bash

set -euo pipefail

usage() {
  echo "usage: $0 [--output DIRECTORY] GRIDARTA_CHECKOUT" >&2
}

output=atrinik-builds
if [[ ${1:-} == --output ]]; then
  if [[ $# -lt 2 ]]; then
    usage
    exit 2
  fi
  output=$2
  shift 2
fi
if [[ $# -ne 1 ]]; then
  usage
  exit 2
fi

gridarta=$1
if [[ ! -x ${gridarta}/gradlew ]]; then
  echo "error: ${gridarta}/gradlew is not executable" >&2
  exit 1
fi

mkdir -p "${output}"
output=$(cd "${output}" && pwd)
gridarta=$(cd "${gridarta}" && pwd)

(
  cd "${gridarta}"
  ./gradlew clean :src:atrinik:preparePublish
)

jar=${gridarta}/src/atrinik/build/libs/AtrinikEditor.jar
if [[ ! -f ${jar} ]]; then
  echo "error: Gridarta did not produce ${jar}" >&2
  exit 1
fi

dated_name=AtrinikEditor-$(date -u +%Y-%m-%d).jar
cp -- "${jar}" "${output}/${dated_name}"
ln -sfn -- "${dated_name}" "${output}/AtrinikEditor.jar"

properties=${gridarta}/src/atrinik/build/libs/update.properties
if [[ -f ${properties} ]]; then
  cp -- "${properties}" "${output}/update.properties"
fi

echo "Built ${output}/${dated_name}"
