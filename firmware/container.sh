#!/usr/bin/env bash
# Build and flash the firmware inside Espressif's ESP-IDF container: no local ESP-IDF
# install, and the same toolchain for everyone (pinned by digest).
#
#   ./container.sh build              fetch the dependencies if missing, then build
#   ./container.sh release            the published image (no server or token): build-release/stackchan-embody.bin
#   ./container.sh flash [PORT]       flash (the CoreS3's USB serial port is found by itself)
#   ./container.sh menuconfig         the ESP-IDF configuration menu
#   ./container.sh shell              a shell in the container, in this directory
#   ./container.sh idf <args…>        any idf.py command, e.g. ./container.sh idf size
#
# ENGINE=docker|podman picks the container engine (default: podman, else docker).
# IDF_IMAGE=… overrides the image. The build goes to build-container/, so a build with
# a local ESP-IDF (build/) is not disturbed.
set -euo pipefail

IDF_IMAGE=${IDF_IMAGE:-docker.io/espressif/idf:v5.5.4@sha256:b9f2d6ea1c19e0c9f7959bdb74a9e3c775642f9d0f3b841937c5fa3363db892b}
ENGINE=${ENGINE:-$(command -v podman || command -v docker || true)}
BUILD_DIR=build-container
cd "$(dirname "${BASH_SOURCE[0]}")"

if [[ -z $ENGINE ]]; then
    echo "Install podman or docker first." >&2
    exit 1
fi

run() {
    local args=(--rm -v "$PWD:/project:Z" -w /project -e HOME=/tmp
        -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*')
    # Rootless podman maps container root to you; docker needs --user for files you own.
    if [[ $(basename "$ENGINE") == docker ]]; then
        args+=(--user "$(id -u):$(id -g)")
    fi
    if [[ -t 0 ]]; then
        args+=(-it)
    fi
    "$ENGINE" run "${args[@]}" "${EXTRA_ARGS[@]}" "$IDF_IMAGE" "$@"
}
EXTRA_ARGS=()

need_deps() {
    [[ -d components && -d xiaozhi-esp32 ]] || run python3 fetch_repos.py
}

case ${1:-build} in
    build)
        need_deps
        run idf.py -B "$BUILD_DIR" build
        ;;
    flash)
        port=${2:-$(ls /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*-if00 2>/dev/null | head -n 1)}
        if [[ -z $port || ! -e $port ]]; then
            echo "No serial port found. Plug in the robot (USB-C on the head) or pass the port." >&2
            exit 1
        fi
        dev=$(readlink -f "$port")
        # The serial device is root:dialout on the host. Rootless podman keeps your groups
        # (keep-groups); SELinux (Fedora) would still block a container from opening a
        # host serial device, so this one run goes without SELinux labels.
        # Same device name inside: esptool reads the port's USB ids from sysfs by name to
        # pick the ESP32-S3's USB-JTAG reset; under another name it resets the wrong way.
        EXTRA_ARGS=(--device "$dev:$dev" --group-add keep-groups --security-opt label=disable)
        if [[ $(basename "$ENGINE") == docker ]]; then
            EXTRA_ARGS=(--device "$dev:$dev" --group-add "$(stat -c %g "$dev")")
        fi
        run idf.py -B "$BUILD_DIR" -p "$dev" flash
        ;;
    release)
        # The published image: sdkconfig.defaults + sdkconfig.defaults.release, never the
        # local overlay (it holds your token). One merged file for flashing at 0x0.
        need_deps
        run bash -c 'idf.py -B build-release -D STACKCHAN_RELEASE=1 -D SDKCONFIG=build-release/sdkconfig \
            -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.release" build &&
            cd build-release && python -m esptool --chip esp32s3 merge_bin -o stackchan-embody.bin @flash_args'
        # The parts and a manifest, as the release workflow publishes them (for /setup).
        version=$(git describe --tags --match 'embody-v*' --always 2>/dev/null || echo dev)
        rm -rf build-release/dist && mkdir -p build-release/dist
        parts=""
        while read -r offset file; do
            cp "build-release/$file" build-release/dist/
            parts="$parts{\"path\":\"$(basename "$file")\",\"offset\":$((offset)),\"sha256\":\"$(sha256sum "build-release/$file" | cut -d' ' -f1)\"},"
        done < <(tail -n +2 build-release/flash_args)
        cp build-release/stackchan-embody.bin build-release/dist/
        printf '{"name":"Stackchan Embody Mode","version":"%s","chipFamily":"ESP32-S3","parts":[%s]}\n' \
            "${version#embody-}" "${parts%,}" > build-release/dist/manifest.json
        echo "build-release/dist: $(ls build-release/dist | tr '\n' ' ')"
        ;;
    menuconfig)
        run idf.py -B "$BUILD_DIR" menuconfig
        ;;
    shell)
        run bash
        ;;
    idf)
        shift
        run idf.py -B "$BUILD_DIR" "$@"
        ;;
    *)
        echo "usage: $0 build | release | flash [PORT] | menuconfig | shell | idf <args…>" >&2
        exit 2
        ;;
esac
