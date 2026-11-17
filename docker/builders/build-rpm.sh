#!/usr/bin/env bash
# Build the confide RPMs locally, the same way the `build-rpm` / `test-rpm`
# jobs in .github/workflows/release.yaml do, but from the current working tree
# (uncommitted changes included). Packages are copied to <repo>/dist/elN/:
# confide, confide-utils, confide-client (x86_64) and confide-client-nautilus
# (noarch, the GNOME Files integration).
#
# Usage: docker/builders/build-rpm.sh [options] [8|9|all]   (default: all)
#   -v, --version X.Y.Z   package version (default: APP_VERSION from CMakeLists.txt)
#       --skip-tests      do not run ctest
#       --skip-install    skip the install test on a clean rockylinux image
#       --rebuild-image   rebuild the builder image without the docker cache
#   -h, --help
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
DIST_DIR="$REPO_ROOT/dist"

VERSION=""
RUN_TESTS=1
INSTALL_TEST=1
NO_CACHE=()
TARGETS=()

usage() { sed -n '2,15s/^# \{0,1\}//p' "$0"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    -v|--version)    VERSION="$2"; shift 2 ;;
    --skip-tests)    RUN_TESTS=0; shift ;;
    --skip-install)  INSTALL_TEST=0; shift ;;
    --rebuild-image) NO_CACHE=(--no-cache); shift ;;
    -h|--help)       usage; exit 0 ;;
    8|9)             TARGETS+=("$1"); shift ;;
    all)             TARGETS+=(8 9); shift ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done
[[ ${#TARGETS[@]} -eq 0 ]] && TARGETS=(8 9)

if [[ -z "$VERSION" ]]; then
  VERSION="$(sed -nE 's/^set\(APP_VERSION "([0-9]+\.[0-9]+\.[0-9]+)".*/\1/p' "$REPO_ROOT/CMakeLists.txt")"
fi
if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "version '$VERSION' is not X.Y.Z" >&2
  exit 1
fi

# Tracked + untracked-but-not-ignored files: the actual code, without build/,
# .cache/, dist/ and other ignored stuff. Deleted tracked files are skipped.
source_tarball() {
  git -C "$REPO_ROOT" ls-files -z --cached --others --exclude-standard \
    | tar -C "$REPO_ROOT" --null --ignore-failed-read -T - -cf - 2>/dev/null
}

build_el() {
  local el="$1" devel_repo image out
  case "$el" in
    8) devel_repo=powertools ;;
    9) devel_repo=crb ;;
  esac
  image="confide-rpm-builder:el$el"
  out="$DIST_DIR/el$el"

  echo "==> [EL$el] building image $image"
  docker build "${NO_CACHE[@]}" \
    --build-arg BASE_IMAGE="almalinux:$el" \
    --build-arg DEVEL_REPO="$devel_repo" \
    -t "$image" -f "$SCRIPT_DIR/Dockerfile.rpm" "$SCRIPT_DIR"

  rm -rf "$out"
  mkdir -p "$out"

  echo "==> [EL$el] building confide $VERSION"
  # vcpkg binary/download caches live in a named volume so dependencies are
  # only compiled once per EL release.
  source_tarball | docker run --rm -i \
    -v "confide-vcpkg-cache-el$el:/cache" \
    -v "$out:/out" \
    -e VERSION="$VERSION" -e EL="$el" -e RUN_TESTS="$RUN_TESTS" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    "$image" bash -euo pipefail -c '
      mkdir -p /cache/vcpkg-binary /cache/vcpkg-downloads
      tar -xf - -C /work

      g++ --version | head -1
      cmake --preset x64-release-linux \
        -DAPP_VERSION="$VERSION" \
        -DVCPKG_MANIFEST_FEATURES=boost \
        -DWITH_EXAMPLES=OFF
      cmake --build --preset x64-release-linux

      if [[ "$RUN_TESTS" == 1 ]]; then
        ctest --test-dir build --output-on-failure
      fi

      ./build/bin/confide-server --version | grep -Fx "confide-server $VERSION"

      cd build
      cpack -G RPM

      # Boost must be linked statically; the rest must be EL system libraries.
      shopt -s nullglob
      rpms=(./*-"$VERSION"-*.el"$EL".*.rpm)
      [[ ${#rpms[@]} -gt 0 ]] || { echo "no RPMs produced" >&2; exit 1; }
      # The desktop integration is its own noarch package; confide-client
      # must be the bare ftc (for servers and headless VMs).
      for name in confide confide-utils confide-client; do
        ls ./"$name-$VERSION"-*.el"$EL".x86_64.rpm >/dev/null \
          || { echo "ERROR: no $name x86_64 RPM" >&2; exit 1; }
      done
      ls ./confide-client-nautilus-"$VERSION"-*.el"$EL".noarch.rpm >/dev/null \
        || { echo "ERROR: no confide-client-nautilus noarch RPM" >&2; exit 1; }
      if rpm -qpl ./confide-client-"$VERSION"-*.el"$EL".x86_64.rpm | grep -E "dialog|nautilus"; then
        echo "ERROR: confide-client contains desktop integration files" >&2
        exit 1
      fi
      for rpm in "${rpms[@]}"; do
        echo "== $rpm"
        rpm -qpi "$rpm"
        rpm -qpl "$rpm"
        if rpm -qpR "$rpm" | tee /dev/stderr | grep -q libboost; then
          echo "ERROR: $rpm depends on a shared Boost library" >&2
          exit 1
        fi
      done

      cp "${rpms[@]}" /out/
      chown -R "$HOST_UID:$HOST_GID" /out
    '

  if [[ "$INSTALL_TEST" == 1 ]]; then
    echo "==> [EL$el] install test on rockylinux:$el"
    docker run --rm -v "$out:/rpms:ro" "rockylinux:$el" bash -euo pipefail -c '
      # As on a headless VM: everything but the desktop integration, which
      # must then be absent.
      dnf -y install $(ls /rpms/*.rpm | grep -v nautilus)
      if rpm -q zenity; then echo "ERROR: zenity installed without the integration" >&2; exit 1; fi
      test ! -e /usr/bin/ftc-send-dialog
      confide-server --version
      ftc --help
      echo secret | confide-passwd
      getent passwd confide
      test "$(stat -c "%U:%G %a" /etc/confide/server.toml)" = "root:confide 640"
      rpm -qc confide | grep -Fx /etc/confide/server.toml
      test -f /usr/lib/systemd/system/confide-server.service

      # The desktop integration on top: pulls in zenity, needs the same ftc.
      dnf -y install /rpms/confide-client-nautilus-*.rpm
      rpm -q zenity
      test -x /usr/bin/ftc-send-dialog
      test -x /usr/bin/ftc-get-dialog
      test -f /usr/share/nautilus-python/extensions/ftc_nautilus.py
      test ! -e /usr/share/nautilus-python/extensions/__pycache__
      echo "install test OK"
    '
  fi

  echo "==> [EL$el] done:"
  ls -1 "$out"
}

for el in "${TARGETS[@]}"; do
  build_el "$el"
done
