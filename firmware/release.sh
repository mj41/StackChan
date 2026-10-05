#!/usr/bin/env bash
# The official Embody Mode firmware: this source built with sdkconfig.defaults +
# sdkconfig.defaults.release (no server or token inside: a robot gets those as settings over
# USB), never the local overlay. Run inside ESP-IDF (the release workflow, or
# ./container.sh release). Writes build-release/dist: the parts, manifest.json with their
# SHA-256 (for sm.w42.eu), one merged image for 0x0, SHA256SUMS.
#
#   ./release.sh [VERSION]      default: git describe of the last embody-v* tag
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

version=${1:-$(git -c safe.directory='*' describe --tags --match 'embody-v*' --always 2>/dev/null || echo dev)}
version=${version#embody-}
# __DATE__ and __TIME__ (e.g. mooncake's banner) from the commit, not the clock: the same
# source gives the same bytes. ./container.sh passes it in (git sees the repo only outside).
# safe.directory: in a CI container the checkout belongs to another user. No fallback: a wrong
# time would give other bytes.
if [[ -z ${SOURCE_DATE_EPOCH:-} ]]; then
    SOURCE_DATE_EPOCH=$(git -c safe.directory='*' log -1 --format=%ct) || { echo "release.sh: no commit time (git); set SOURCE_DATE_EPOCH" >&2; exit 1; }
fi
export SOURCE_DATE_EPOCH
echo "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH (version $version)"

# A clean build every time, as in CI: the configuration only from the defaults files, and every
# object compiled with this commit's SOURCE_DATE_EPOCH (an incremental build keeps old ones).
rm -rf build-release
idf.py -B build-release -D STACKCHAN_RELEASE=1 -D EMBODY_VERSION="$version" -D SDKCONFIG=build-release/sdkconfig \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.release" build

dist=build-release/dist
rm -rf "$dist" && mkdir -p "$dist"
(cd build-release && python -m esptool --chip esp32s3 merge_bin -o ../$dist/stackchan-embody.bin @flash_args)
parts=""
while read -r offset file; do
    cp "build-release/$file" "$dist/"
    parts="$parts{\"path\":\"$(basename "$file")\",\"offset\":$((offset)),\"sha256\":\"$(sha256sum "build-release/$file" | cut -d' ' -f1)\"},"
done < <(tail -n +2 build-release/flash_args)
printf '{"name":"Stackchan Embody Mode","version":"%s","chipFamily":"ESP32-S3","parts":[%s]}\n' \
    "$version" "${parts%,}" > "$dist/manifest.json"
(cd "$dist" && sha256sum -- * > SHA256SUMS)
echo "$dist: $(ls "$dist" | tr '\n' ' ')"
cat "$dist/manifest.json"
