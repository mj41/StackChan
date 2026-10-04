#!/bin/bash
# Level 3: the same crosstool-NG build as build.sh, with every host tool from Guix (compiler,
# binutils, libc, make, Python, meson, Rust...), run inside `guix shell --pure`. The sources are
# the tarballs saved by the level-2 build, so both builds compile the same bytes.
set -euxo pipefail
export PATH=/var/guix/profiles/per-user/root/current-guix/bin:$PATH
exec guix shell --pure $(cat /packages.txt) -- bash -euxo pipefail -c '
export CONFIG_SHELL=$(command -v bash) SHELL=$(command -v bash)
export CT_SYSTEM_CARGO=$(command -v cargo)
# crosstool-NG refuses LIBRARY_PATH (its cross compiler would see it too). Guix'"'"'s host gcc
# needs it to find its C library, so host gcc/g++ get those paths from small wrappers instead.
mkdir -p /tmp/hostwrap
E=$GUIX_ENVIRONMENT
for t in gcc cc g++ c++; do
    case $t in
        gcc|cc) inc="-isystem $E/include" ;;
        *)      inc="-isystem $E/include/c++ -isystem $E/include" ;;
    esac
    real=$(command -v $t || true)
    [ -n "$real" ] || case $t in cc) real=$(command -v gcc) ;; c++) real=$(command -v g++) ;; esac
    printf "#!%s\nexec %s %s -B%s/lib/ -L%s/lib \"\$@\"\n" "$(command -v bash)" "$real" "$inc" "$E" "$E" > /tmp/hostwrap/$t
    chmod +x /tmp/hostwrap/$t
done
export PATH=/tmp/hostwrap:$PATH
unset LIBRARY_PATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH CPATH
cat /tmp/hostwrap/gcc
echo "#include <stdio.h>
int main(){puts(\"c ok\");return 0;}" > /tmp/t.c && gcc /tmp/t.c -o /tmp/t && /tmp/t
echo "#include <iostream>
int main(){std::cout << \"c++ ok\" << std::endl;}" > /tmp/t.cc && g++ /tmp/t.cc -o /tmp/tcc && /tmp/tcc
gcc --version | head -1
cd /builds/idf/crosstool-NG
git -c safe.directory="*" checkout -- scripts/build/companion_libs/901-xtensa_esp_bin_wrappers.sh
./maintainer/git-version-gen .tarball-version; echo
./bootstrap                              # fixes the version: the tree must be clean here
patch -p1 < /t/system-cargo.patch        # then: Guix'"'"'s cargo instead of the rustup download
./configure --enable-local
make
./ct-ng xtensa-esp-elf
cat >> .config <<CFG
# CT_LOG_PROGRESS_BAR is not set
# CT_PREFIX_DIR_RO is not set
CT_LOG_EXTRA=y
CT_LOG_LEVEL_MAX="EXTRA"
CT_LOG_TO_FILE=y
# CT_LOG_FILE_COMPRESS is not set
CT_ALLOW_BUILD_AS_ROOT=y
CT_ALLOW_BUILD_AS_ROOT_SURE=y
CT_CONNECT_TIMEOUT=30
CT_PARALLEL_JOBS=6
CT_LOCAL_TARBALLS_DIR="/w/tarballs"
CT_SAVE_TARBALLS=n
CFG
if gcc -v 2>&1 | grep -q -- --enable-default-pie; then echo "CT_EXTRA_LDFLAGS_FOR_HOST=\"-no-pie\"" >> .config; fi
./ct-ng upgradeconfig
./ct-ng build
echo BUILD-DONE
'
