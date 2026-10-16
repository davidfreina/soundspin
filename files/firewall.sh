#!/bin/sh
#
# Firewall rules for the soundspin auto-boot override.
#
# Bose's Firewall daemon (/opt/Bose/Firewall) watches the configured interfaces
# and runs a rules script with their names whenever an address changes. Stock,
# that script is /etc/init.d/Firewalls/update_iptables, which accepts inbound
# traffic only from the speaker's own subnet and drops the rest (established
# connections, e.g. to Music Assistant, are always allowed back in).
#
# That is too strict for a network where you manage the speakers from another
# subnet or VLAN, and SSH on these devices is passwordless root, so turning the
# firewall off is not a good answer either. This wrapper applies Bose's rules
# unchanged, then additionally accepts the subnets listed in soundspin.conf:
#
#     firewall_allow = 10.0.20.0/24 10.0.30.0/24
#
# Installed to /mnt/nv/firewall.sh by `make install`.

CONF=/mnt/nv/soundspin.conf

/etc/init.d/Firewalls/update_iptables "$@" || exit $?

allow=$(grep '^[[:space:]]*firewall_allow[[:space:]]*=' "$CONF" 2>/dev/null \
        | tail -n 1 | cut -d= -f2- | sed 's/#.*//')

for net in $allow; do
    if iptables -I INPUT 1 -s "$net" -j ACCEPT; then
        logger -t firewall "allowing inbound from $net"
    else
        logger -t firewall "ignoring invalid firewall_allow entry '$net'"
    fi
done
