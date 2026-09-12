#!/bin/sh
#
# Boot safety net for the soundspin auto-boot override.
#
# The override replaces the stock daemon set, and a soundspin build that takes
# the network down at boot would leave the device unreachable -- recoverable
# only with physical access (USB gadget, serial console, or factory reset).
#
# This runs as a shepherd daemon alongside soundspin. A couple of minutes after
# boot it checks whether the default gateway is reachable. If not, it restores
# the networking-only fallback and reboots, so the device always comes back.
#
# Installed to /mnt/nv/netwatch.sh by `make install`.

SETTLE_SECONDS=120
FALLBACK=/mnt/nv/good-shepherd.xml
OVERRIDE_DIR=/mnt/nv/shepherd

sleep "$SETTLE_SECONDS"

GW=$(ip route 2>/dev/null | awk '/^default/{print $3; exit}')
[ -z "$GW" ] && GW=$(route -n 2>/dev/null | awk '/^0\.0\.0\.0/{print $2; exit}')

if [ -n "$GW" ] && ping -c3 -W2 "$GW" >/dev/null 2>&1; then
    logger -t netwatch "network OK ($GW), standing down"
elif [ ! -f "$FALLBACK" ]; then
    logger -t netwatch "network DOWN but $FALLBACK is missing; not reverting"
else
    logger -t netwatch "network DOWN (gw='$GW'), restoring fallback and rebooting"
    rm -f "$OVERRIDE_DIR"/Shepherd-*.xml
    cp "$FALLBACK" "$OVERRIDE_DIR/Shepherd-fallback.xml"
    sync
    reboot
fi

# Stay resident: shepherdd restarts daemons that exit, and we only want to run
# this check once per boot.
while true; do
    sleep 3600
done
