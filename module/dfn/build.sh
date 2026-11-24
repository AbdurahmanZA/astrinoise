#!/usr/bin/env bash
# Build libastrinoise_dfn.so, the optional DeepFilterNet3 engine for func_astrinoise.
# Needs a Rust toolchain (https://rustup.rs), git, patch and a C compiler. One build works for
# every Asterisk version: the library does not use Asterisk at all.
#
#   bash module/dfn/build.sh        -> module/dfn/libastrinoise_dfn.so
#
# It fetches DeepFilterNet 0.5.6 (git tag, commit checked), applies patches/*.patch, and builds
# with Cargo.lock, which pins every other dependency. Build on the oldest Debian you deploy to
# (Debian 12 for FreePBX 17), so the library needs no newer glibc than the PBX has.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"
DFN_TAG=v0.5.6
DFN_COMMIT=978576aa8400552a4ce9730838c635aa30db5e61
SRC=$HERE/target/DeepFilterNet
if [ ! -f "$SRC/.astrinoise-patched" ]; then
  rm -rf "$SRC"
  git -c advice.detachedHead=false clone -q --depth 1 --branch "$DFN_TAG" https://github.com/Rikorose/DeepFilterNet "$SRC"
  [ "$(git -C "$SRC" rev-parse HEAD)" = "$DFN_COMMIT" ] || { echo "DeepFilterNet $DFN_TAG is not commit $DFN_COMMIT"; exit 1; }
  for p in "$HERE"/patches/*.patch; do patch -s -d "$SRC" -p1 < "$p"; done
  touch "$SRC/.astrinoise-patched"
fi
lock=; [ -f Cargo.lock ] && lock=--locked
# -j2: tract-core needs about 1.5 GB of RAM per compile job.
cargo build --release -j"${JOBS:-2}" $lock
cp target/release/libastrinoise_dfn.so "$HERE/libastrinoise_dfn.so"
strip --strip-unneeded "$HERE/libastrinoise_dfn.so"
echo "built $HERE/libastrinoise_dfn.so ($(stat -c %s "$HERE/libastrinoise_dfn.so") bytes)"
