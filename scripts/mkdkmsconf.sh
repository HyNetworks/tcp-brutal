#!/usr/bin/env bash

set -e

cd "$(dirname "$0")/.."

# The version comes from the BRUTAL_VERSION_* macros in brutal.c.
# Builds that are not on a release tag get a ".r<commits>.<hash>" suffix.
module_version() {
  sed -nE 's/^#define BRUTAL_VERSION_(MAJOR|MINOR|PATCH)[[:space:]]+([0-9]+).*/\2/p' brutal.c | paste -sd.
}

pkgver() {
  local _version
  _version="$(module_version)"
  if git describe --tags --exact-match > /dev/null 2>&1 || ! git rev-parse HEAD > /dev/null 2>&1; then
    echo "$_version"
  else
    printf "%s.r%s.%s\n" "$_version" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
  fi
}

PACKAGE_VERSION=${PACKAGE_VERSION:-$(pkgver)}

cat << EOF
PACKAGE_NAME="tcp-brutal"
PACKAGE_VERSION="$PACKAGE_VERSION"

MAKE[0]="make KERNEL_DIR=\${kernel_source_dir} all"
CLEAN="make KERNEL_DIR=\${kernel_source_dir} clean"

BUILT_MODULE_NAME[0]="brutal"
DEST_MODULE_LOCATION[0]="/extra"

AUTOINSTALL="yes"
EOF
