#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACKAGES_DIR="${ROOT_DIR}/.deps/ubuntu-libcxx/packages"
EXTRACT_DIR="${ROOT_DIR}/.deps/ubuntu-libcxx/root"

mkdir -p "${PACKAGES_DIR}" "${EXTRACT_DIR}"

packages=(
  "libc++-18-dev=1:18.1.3-1ubuntu1"
  "libc++1-18=1:18.1.3-1ubuntu1"
  "libc++abi-18-dev=1:18.1.3-1ubuntu1"
  "libc++abi1-18=1:18.1.3-1ubuntu1"
  "libunwind-18-dev=1:18.1.3-1ubuntu1"
  "libunwind-18=1:18.1.3-1ubuntu1"
)

(
  cd "${PACKAGES_DIR}"
  apt download "${packages[@]}"
)

rm -rf "${EXTRACT_DIR}"
mkdir -p "${EXTRACT_DIR}"

for archive in "${PACKAGES_DIR}"/*.deb; do
  dpkg-deb -x "${archive}" "${EXTRACT_DIR}"
done

printf 'Local libc++ toolchain extracted to %s\n' "${EXTRACT_DIR}"
printf 'Headers: %s\n' "${EXTRACT_DIR}/usr/include/c++/v1"
printf 'Libraries: %s\n' "${EXTRACT_DIR}/usr/lib/llvm-18/lib"
