#!/usr/bin/env bash
# Build func_astrinoise.so (Astri-noise) against the Asterisk headers installed on this box.
# Build on a lab box with the SAME Asterisk version and build options as the PBX that will load it.
#
# FreePBX 17 / Sangoma packages (Debian 12):
#   apt-get install asterisk22-devel=<same version as asterisk22> libspeexdsp-dev gcc make autoconf automake libtool curl
# Asterisk built from source: `make install` already put the headers in /usr/include/asterisk
#   (or set ASTERISK_INCLUDE=/path/to/include), plus: apt-get install libspeexdsp-dev gcc make autoconf automake libtool curl
#
#   bash build.sh            -> ./func_astrinoise.so
#   EXTRA_CFLAGS="-DRESAMPLE_QUALITY=4" bash build.sh    (experiments; see README)
#
# RNNoise 0.2 (xiph, BSD-3) is built from its release tarball (sha256 pinned) and linked in statically.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RNN_VER=0.2
RNN_SHA=90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37
AST_INC=${ASTERISK_INCLUDE:-/usr/include}
B=$HERE/build
mkdir -p "$B"
cd "$B"
if [ ! -f rnnoise/lib/librnnoise.a ]; then
  [ -f "rnnoise-$RNN_VER.tar.gz" ] || curl -fsSL -o "rnnoise-$RNN_VER.tar.gz" \
    "https://github.com/xiph/rnnoise/releases/download/v$RNN_VER/rnnoise-$RNN_VER.tar.gz"
  echo "$RNN_SHA  rnnoise-$RNN_VER.tar.gz" | sha256sum -c --quiet
  rm -rf "rnnoise-$RNN_VER" && tar -xzf "rnnoise-$RNN_VER.tar.gz"
  # Plain build (no AVX dispatch), so the same .so runs on old CPUs without AVX.
  (cd "rnnoise-$RNN_VER" && ./configure -q --with-pic --enable-static --disable-shared --disable-examples --disable-doc \
     --prefix="$B/rnnoise" >/dev/null && make -s -j"$(nproc)" >/dev/null 2>&1 && make -s install >/dev/null)
fi
grep -q 'AST_BUILDOPT_SUM' "$AST_INC/asterisk/buildopts.h" || { echo "Asterisk headers not found in $AST_INC/asterisk"; exit 1; }
gcc -fPIC -shared -O2 -g -Wall -Wno-unused-result \
  -D_GNU_SOURCE ${EXTRA_CFLAGS:-} -DAST_MODULE=\"func_astrinoise\" -DAST_MODULE_SELF_SYM=__internal_func_astrinoise_self \
  -I"$AST_INC" -I"$B/rnnoise/include" \
  "$HERE/func_astrinoise.c" -o "$HERE/func_astrinoise.so" \
  "$B/rnnoise/lib/librnnoise.a" -lspeexdsp -lm -ldl -lpthread -Wl,--exclude-libs,ALL -Wl,-z,defs \
  -Wl,--unresolved-symbols=ignore-in-object-files
ver=$(dpkg-query -W -f='${Version}' asterisk22-devel 2>/dev/null || asterisk -V 2>/dev/null || true)
echo "built $HERE/func_astrinoise.so ($(du -h "$HERE/func_astrinoise.so" | cut -f1)) against asterisk ${ver:-?}"
