#!/usr/bin/env bash
# Basic script to add / remove firewall configuration for SURFXR
# Usage: ./surfxr_fw_config.sh add|remove
# Exit codes:
# 1 - Invalid command
# 2 - Invalid action
# 3 - Failed to copy UFW configuration
# 99 - Firewall not found
# 126 - pkexec failed - Request dismissed

firewalld_cfg() {
    # Iterate around each active zone
    for zone in $(firewall-cmd --get-active-zones | grep -P --only-matching '^[\w/-]+'); do
        if [ "${1}" == 'add' ]; then
            # If running or permanent surfxr service is missing, add it
            if ! firewall-cmd --zone="${zone}" --list-services | grep 'surfxr' >/dev/null 2>&1; then
                firewall-cmd --zone="${zone}" --add-service='surfxr'
            fi
            if ! firewall-cmd --zone="${zone}" --list-services --permanent | grep 'surfxr' >/dev/null 2>&1; then
                firewall-cmd --zone="${zone}" --add-service='surfxr' --permanent
            fi
        elif [ "${1}" == 'remove' ]; then
            # If running or persistent surfxr service exists, remove it
            if firewall-cmd --zone="${zone}" --list-services | grep 'surfxr' >/dev/null 2>&1; then
                firewall-cmd --zone="${zone}" --remove-service='surfxr'
            fi
            if firewall-cmd --zone="${zone}" --list-services --permanent | grep 'surfxr' >/dev/null 2>&1; then
                firewall-cmd --zone="${zone}" --remove-service='surfxr' --permanent
            fi
        else
            exit 2
        fi
    done
}

ufw_cfg() {
    # Try and install the application file
    if ! ufw app info 'surfxr'; then
        # Pull application file from local build first if the script lives inside it
        if [ -f "$(dirname "$(realpath "${0}")")/ufw-surfxr" ]; then
            cp "$(dirname "$(realpath "${0}")")/ufw-surfxr" '/etc/ufw/applications.d/'
        elif [ -f '/usr/share/surfxr/ufw-surfxr' ]; then
            cp '/usr/share/surfxr/ufw-surfxr' '/etc/ufw/applications.d/'
        else
            exit 3
        fi
    fi

    if [ "${1}" == 'add' ] && ! ufw status | grep 'surfxr' >/dev/null 2>&1; then
        ufw allow 'surfxr'
    elif [ "${1}" == 'remove' ] && ufw status | grep 'surfxr' >/dev/null 2>&1; then
        ufw delete allow 'surfxr'
    else
        exit 2
    fi
}

iptables_cfg() {
    case "${1}" in add|remove) ;; *) exit 2 ;; esac
    for protocol in tcp udp; do
        if [ "$protocol" == tcp ]; then
            ports="19943 19944 19945 19946 19950"
        else
            ports="19943 19944 19945 19950 19951 19952"
        fi
        for port in $ports; do
            for chain in INPUT OUTPUT; do
                if [ "$chain" == INPUT ]; then direction=--dport; else direction=--sport; fi
                if [ "${1}" == add ]; then
                    iptables -C "$chain" -p "$protocol" "$direction" "$port" -j ACCEPT 2>/dev/null ||
                        iptables -I "$chain" -p "$protocol" "$direction" "$port" -j ACCEPT
                elif iptables -C "$chain" -p "$protocol" "$direction" "$port" -j ACCEPT 2>/dev/null; then
                    iptables -D "$chain" -p "$protocol" "$direction" "$port" -j ACCEPT
                fi
            done
        done
    done
    mkdir -p /etc/iptables
    iptables-save >/etc/iptables/rules.v4
}

main() {
    # If we're not root use pkexec for GUI prompt
    if [ "${USER}" == 'root' ]; then
        # Check if firewall-cmd exists and firewalld is running
        if which firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
            firewalld_cfg "${1,,}"
        # Check if ufw exists and is running
        elif which ufw >/dev/null 2>&1 && ! ufw status | grep 'Status: inactive' >/dev/null 2>&1; then
            ufw_cfg "${1,,}"
        elif which iptables >/dev/null 2>&1; then
            iptables_cfg "${1,,}"
        else
            exit 99
        fi
    else
        pkexec "$(realpath "${0}")" "${@}"
    fi
}

main "${@}"
