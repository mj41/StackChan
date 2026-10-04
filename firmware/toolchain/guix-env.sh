#!/bin/bash
# Inside the container: Guix from its binary release, daemon in the background.
set -euo pipefail
if [ ! -e /var/guix/profiles/per-user/root/current-guix ]; then
    echo "Guix store missing" >&2; exit 1
fi
export PATH=/var/guix/profiles/per-user/root/current-guix/bin:$PATH
mkdir -p /root/.config/guix
ln -sfn /var/guix/profiles/per-user/root/current-guix /root/.config/guix/current
getent group guixbuild >/dev/null || groupadd --system guixbuild
for i in $(seq -w 1 8); do id guixbuilder$i >/dev/null 2>&1 || useradd -g guixbuild -G guixbuild -d /var/empty -s /usr/sbin/nologin -c "Guix build user $i" --system guixbuilder$i; done
guix-daemon --build-users-group=guixbuild --disable-chroot --max-jobs=4 > /var/log-guix-daemon.txt 2>&1 &
sleep 2
for k in ci.guix.gnu.org bordeaux.guix.gnu.org; do guix archive --authorize < /var/guix/profiles/per-user/root/current-guix/share/guix/$k.pub; done
exec "$@"
