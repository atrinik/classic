#!/usr/bin/env bash

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_directory=${1:-"${root}/build/linux-debug"}
if [[ ${build_directory} != /* ]]; then
  build_directory="${root}/${build_directory}"
fi

python3 "${root}/tools/dependencies.py" verify
test -x "${build_directory}/atrinik-server"
test -f "${build_directory}/libplugin_arena.so"
test -f "${build_directory}/libplugin_python.so"

test -d "${root}/data" || cp -R "${root}/install_data" "${root}/data"
mkdir -p "${root}/data/tmp"
for managed_path in lib maps; do
  if [[ -e ${root}/${managed_path} && ! -L ${root}/${managed_path} ]]; then
    echo "Refusing to replace unmanaged runtime path: ${root}/${managed_path}" >&2
    exit 1
  fi
  ln -sfn "runtime/content/${managed_path}" "${root}/${managed_path}"
done
ln -sfn "${build_directory}/atrinik-server" "${root}/atrinik-server"
ln -sfn "${build_directory}/libplugin_arena.so" "${root}/libplugin_arena.so"
ln -sfn "${build_directory}/libplugin_python.so" "${root}/libplugin_python.so"

echo "Runtime prepared at ${root}"
