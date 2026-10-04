#!/bin/bash
# Espressif's GitLab CI steps for xtensa-esp-elf on x86_64 Linux (.gitlab-ci.yml:
# build_ctng, configure_common without their internal mirrors, configure_docker,
# configure_linux_no_pie, build_toolchain), plus parallel jobs and a download cache.
set -euxo pipefail
cd /builds/idf/crosstool-NG
./bootstrap
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
CT_PARALLEL_JOBS=${CT_JOBS:-$(nproc)}
CT_LOCAL_TARBALLS_DIR="/w/tarballs"
CT_SAVE_TARBALLS=y
CFG
if gcc -v 2>&1 | grep -q -- --enable-default-pie; then echo 'CT_EXTRA_LDFLAGS_FOR_HOST="-no-pie"' >> .config; fi
./ct-ng upgradeconfig
./ct-ng build
echo BUILD-DONE
