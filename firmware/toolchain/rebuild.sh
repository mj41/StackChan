#!/usr/bin/env bash
# Rebuild Espressif's Xtensa toolchain from source, then the release firmware with it, and
# compare with a reference build (diverse double-compiling: if our compiler gives the same
# firmware as Espressif's prebuilt one, that compiler adds nothing its source does not explain).
#
#   toolchain/rebuild.sh ubuntu          the toolchain, on Ubuntu 22.04 (about an hour)
#   toolchain/rebuild.sh guix            the same with every host tool from Guix
#   toolchain/rebuild.sh firmware ubuntu|guix VERSION REFERENCE_SHA256SUMS
#                                        ./release.sh VERSION with that toolchain, compared with
#                                        the SHA256SUMS of a reference (e.g. the CI artifact)
#
# WORK (default ~/.cache/stackchan-toolchain) holds the checkouts, the source tarballs (shared,
# so both builds compile the same bytes) and Guix. Needs podman.
set -euo pipefail

TAG=esp-14.2.0_20260121    # ESP-IDF v5.5.4's xtensa-esp-elf
IDF_IMAGE=docker.io/espressif/idf:v5.5.4@sha256:b9f2d6ea1c19e0c9f7959bdb74a9e3c775642f9d0f3b841937c5fa3363db892b
GUIX=1.5.0
GUIX_KEY=A28BF40C3E551372662D14F741AAE7DCCA3D8351   # Efraim Flashner, listed in guix-install.sh
WORK=${WORK:-$HOME/.cache/stackchan-toolchain}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
FIRMWARE=$(dirname "$HERE")
# Espressif builds in this path, and newlib's assert() strings keep it: the same path, the same bytes.
CTNG_PATH=/builds/idf/crosstool-NG

mkdir -p "$WORK/tarballs"

checkout() {  # a clean crosstool-NG at the tag, with its submodules
    local dir=$WORK/ctng-$1
    if [[ ! -d $dir ]]; then
        git clone -q --branch "$TAG" --recurse-submodules --shallow-submodules \
            https://github.com/espressif/crosstool-NG.git "$dir"
    fi
    podman unshare rm -rf "$dir/.build" "$dir/builds"   # files from the container's user namespace
    git -C "$dir" checkout -q -- .
    [[ $(git -C "$dir" describe --tags --dirty) == "$TAG" ]] || { echo "$dir is not at $TAG" >&2; exit 1; }
}

image() {
    podman build -q -t stackchan-ctng-build -f "$HERE/Containerfile" "$HERE" >/dev/null
}

guix_store() {  # Guix's binary release, its signature checked, unpacked into WORK/guix/root
    local g=$WORK/guix tar=guix-binary-$GUIX.x86_64-linux.tar.xz
    mkdir -p "$g"
    [[ -d $g/root/gnu ]] && return
    (cd "$g" && curl -sSfLO "https://ftp.gnu.org/gnu/guix/$tar" && curl -sSfLO "https://ftp.gnu.org/gnu/guix/$tar.sig")
    export GNUPGHOME=$g/gnupg
    mkdir -p -m 700 "$GNUPGHOME"
    curl -sSfL "https://keys.openpgp.org/vks/v1/by-fingerprint/$GUIX_KEY" | gpg -q --import
    gpg --status-fd 1 --verify "$g/$tar.sig" "$g/$tar" 2>/dev/null | grep -q "^\[GNUPG:\] VALIDSIG .* $GUIX_KEY\$" \
        || { echo "Guix tarball: no valid signature by $GUIX_KEY" >&2; exit 1; }
    mkdir -p "$g/root"
    podman run --rm --security-opt label=disable -v "$g:/g" stackchan-ctng-build \
        bash -c "cd /g/root && tar --warning=no-timestamp -xf /g/$tar"
}

guix_run() {  # a command in the Guix container (seccomp off for guix-daemon's personality())
    local g=$WORK/guix
    podman run --rm --security-opt seccomp=unconfined --security-opt label=disable -e "CT_JOBS=${CT_JOBS:-6}" \
        -v "$g/root/gnu:/gnu" -v "$g/root/var/guix:/var/guix" -v "$HERE:/t:ro" \
        -v "$HERE/guix-env.sh:/guix-env.sh:ro" -v "$HERE/guix-packages.txt:/packages.txt:ro" \
        -v "$WORK:/w" -v "$WORK/ctng-guix:$CTNG_PATH" stackchan-ctng-build /guix-env.sh "$@"
}

case ${1:-} in
    ubuntu)
        image
        checkout ubuntu
        podman run --rm --security-opt label=disable -e "CT_JOBS=${CT_JOBS:-6}" -v "$HERE:/t:ro" -v "$WORK:/w" \
            -v "$WORK/ctng-ubuntu:$CTNG_PATH" stackchan-ctng-build bash /t/build-ubuntu.sh
        ;;
    guix)
        image
        guix_store
        checkout guix
        guix_run bash /t/build-guix.sh
        ;;
    firmware)
        host=${2:?ubuntu or guix}; version=${3:?version, e.g. dev-1234abcd}; ref=${4:?reference SHA256SUMS}
        tc=$WORK/ctng-$host/builds/xtensa-esp-elf
        [[ -x $tc/bin/xtensa-esp-elf-gcc ]] || { echo "no toolchain in $tc: run '$0 $host' first" >&2; exit 1; }
        extra=()
        [[ $host == guix ]] && extra=(-v "$WORK/guix/root/gnu:/gnu:ro")   # its programs use Guix's libc
        podman run --rm --security-opt label=disable -v "$FIRMWARE:/project" "${extra[@]}" \
            -v "$tc:/opt/esp/tools/xtensa-esp-elf/$TAG/xtensa-esp-elf:ro" -w /project -e HOME=/tmp \
            -e "SOURCE_DATE_EPOCH=$(git -C "$FIRMWARE" log -1 --format=%ct)" \
            -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
            "$IDF_IMAGE" bash -c ". \$IDF_PATH/export.sh >/dev/null; ./release.sh $version"
        if diff <(sort -k2 "$FIRMWARE/build-release/dist/SHA256SUMS") <(sort -k2 "$ref"); then
            echo "The same bytes as the reference: all files."
        else
            echo "DIFFERENT from the reference (above)." >&2; exit 1
        fi
        ;;
    *)
        sed -n '2,13p' "$0"; exit 2 ;;
esac
