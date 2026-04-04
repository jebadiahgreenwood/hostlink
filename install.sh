#!/usr/bin/env bash
# HostLink installer — installs hostlinkd + optional read-only daemon for AI agents
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "install.sh must be run as root" >&2
    exit 1
fi

INSTALL_RO="${INSTALL_RO:-1}"        # Set to 0 to skip RO daemon
WORKSPACE="${OPENCLAW_WORKSPACE:-}"  # Optional: path to OpenClaw workspace

echo "=== HostLink Installer ==="
echo "  Full daemon:     yes"
echo "  RO daemon:       $([ "$INSTALL_RO" = "1" ] && echo yes || echo no)"
echo ""

# ── Build if needed ────────────────────────────────────────────────────────────
if [ ! -f build/hostlinkd ] || [ ! -f build/hostlink-cli ]; then
    echo "[*] Binaries not found — building..."
    ./build.sh release
fi

# ── Install binaries ───────────────────────────────────────────────────────────
cp build/hostlinkd    /usr/local/bin/hostlinkd
cp build/hostlink-cli /usr/local/bin/hostlink-cli
chmod 755 /usr/local/bin/hostlinkd /usr/local/bin/hostlink-cli
echo "[+] Installed binaries to /usr/local/bin/"

# ── Create hostlink group and users ───────────────────────────────────────────
if ! getent group hostlink >/dev/null 2>&1; then
    groupadd --system hostlink
    echo "[+] Created group: hostlink"
fi

if ! getent passwd hostlink >/dev/null 2>&1; then
    useradd --system --no-create-home --shell /usr/sbin/nologin \
            --gid hostlink --comment "HostLink daemon" hostlink
    echo "[+] Created user: hostlink"
fi

# RO user for AI agent access (read-only restricted shell)
if [ "$INSTALL_RO" = "1" ]; then
    if ! getent passwd hostlink-ro >/dev/null 2>&1; then
        useradd -r -s /bin/bash -m -d /var/lib/hostlink-ro \
                -G hostlink --comment "HostLink read-only agent" hostlink-ro
        echo "[+] Created user: hostlink-ro"
    fi
fi

# ── Restricted shell for RO daemon ────────────────────────────────────────────
if [ "$INSTALL_RO" = "1" ]; then
    cat > /usr/local/bin/hostlink-ro-shell << 'SHELL'
#!/bin/bash
# Restricted shell for hostlink-ro — read-only operations only.
# Blocks: file modification, network tools, package managers, docker, sudo.
BLOCKED="rm|rmdir|mv|cp|chmod|chown|dd|mkfs|fdisk|wget|curl|pip|pip3|apt|apt-get|dpkg|yum|dnf|docker|sudo|su|passwd|useradd|usermod|userdel|systemctl|service|mount|umount|kill|pkill|killall|shutdown|reboot|init|nc|ncat|netcat|ssh|scp|rsync|git|make|gcc|cc"
if echo "$@" | grep -qiE "(^|[[:space:]|;(&])($BLOCKED)([[:space:];|)&]|$)"; then
    echo "Error: Command not permitted in read-only mode" >&2
    exit 126
fi
exec /bin/bash "$@"
SHELL
    chmod 755 /usr/local/bin/hostlink-ro-shell
    echo "[+] Installed restricted shell: /usr/local/bin/hostlink-ro-shell"
fi

# ── Config directory ──────────────────────────────────────────────────────────
mkdir -p /etc/hostlink
chmod 750 /etc/hostlink
chown root:hostlink /etc/hostlink

# ── Main config ───────────────────────────────────────────────────────────────
if [ ! -f /etc/hostlink/hostlink.conf ]; then
    TOKEN=$(openssl rand -hex 32)

    # Determine socket path — use workspace if available, else /run/hostlink
    if [ -n "$WORKSPACE" ] && [ -d "$WORKSPACE" ]; then
        mkdir -p "$WORKSPACE/.hostlink"
        SOCKET_PATH="$WORKSPACE/.hostlink/hostlink.sock"
        echo "[+] Using workspace socket path: $SOCKET_PATH"
    else
        SOCKET_PATH="/run/hostlink/hostlink.sock"
    fi

    sed "s|REPLACE_WITH_GENERATED_TOKEN|$TOKEN|; s|unix_path = .*|unix_path = $SOCKET_PATH|" \
        hostlink.conf.example > /etc/hostlink/hostlink.conf
    chmod 640 /etc/hostlink/hostlink.conf
    chown root:hostlink /etc/hostlink/hostlink.conf
    echo "[+] Generated config: /etc/hostlink/hostlink.conf"
    echo "[+] Auth token: $TOKEN"
    echo "    (Save this — needed for client targets.conf)"
else
    TOKEN=$(grep auth_token /etc/hostlink/hostlink.conf | awk -F'= ' '{print $2}' | tr -d ' ')
    SOCKET_PATH=$(grep unix_path /etc/hostlink/hostlink.conf | awk -F'= ' '{print $2}' | tr -d ' ')
    echo "[=] Config exists (not overwritten): /etc/hostlink/hostlink.conf"
fi

# ── RO config ────────────────────────────────────────────────────────────────
if [ "$INSTALL_RO" = "1" ]; then
    if [ ! -f /etc/hostlink/hostlink-ro.conf ]; then
        RO_TOKEN=$(openssl rand -hex 32)
        mkdir -p /var/lib/hostlink-sockets
        chown hostlink-ro:hostlink /var/lib/hostlink-sockets
        chmod 775 /var/lib/hostlink-sockets

        cat > /etc/hostlink/hostlink-ro.conf << CONF
node_name = $(hostname)-readonly
auth_token = $RO_TOKEN
unix_enabled = 1
unix_path = /var/lib/hostlink-sockets/hostlink-ro.sock
unix_mode = 0666
tcp_enabled = 0
max_concurrent = 4
default_timeout_ms = 30000
max_timeout_ms = 120000
shell = /usr/local/bin/hostlink-ro-shell
default_max_output_bytes = 1048576
max_output_bytes = 4194304
output_tmpdir = /tmp/hostlink_ro_output
log_target = file
log_file = /var/log/hostlink-ro.log
log_level = info
CONF
        chmod 640 /etc/hostlink/hostlink-ro.conf
        chown root:hostlink /etc/hostlink/hostlink-ro.conf
        touch /var/log/hostlink-ro.log
        chown hostlink-ro:hostlink-ro /var/log/hostlink-ro.log
        mkdir -p /tmp/hostlink_ro_output
        echo "[+] Generated RO config: /etc/hostlink/hostlink-ro.conf"
        echo "[+] RO auth token: $RO_TOKEN"
    else
        RO_TOKEN=$(grep auth_token /etc/hostlink/hostlink-ro.conf | awk -F'= ' '{print $2}' | tr -d ' ')
        echo "[=] RO config exists (not overwritten)"
    fi
fi

# ── Systemd services ──────────────────────────────────────────────────────────
# Main service
cat > /etc/systemd/system/hostlinkd.service << SVCEOF
[Unit]
Description=HostLink Command Execution Daemon
After=network.target
Documentation=https://github.com/jebadiahgreenwood/hostlink

[Service]
Type=simple
ExecStart=/usr/local/bin/hostlinkd -f -c /etc/hostlink/hostlink.conf
Restart=always
RestartSec=3
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
SVCEOF

# RO service
if [ "$INSTALL_RO" = "1" ]; then
    cat > /etc/systemd/system/hostlinkd-ro.service << SVCEOF
[Unit]
Description=HostLink Read-Only Daemon (AI agent access)
After=network.target
Documentation=https://github.com/jebadiahgreenwood/hostlink

[Service]
Type=simple
User=hostlink-ro
Group=hostlink-ro
ExecStart=/usr/local/bin/hostlinkd -f -c /etc/hostlink/hostlink-ro.conf
Restart=always
RestartSec=3
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
SVCEOF
fi

echo "[+] Installed systemd unit(s)"

# ── Enable and start ──────────────────────────────────────────────────────────
systemctl daemon-reload
systemctl enable --now hostlinkd
echo "[+] hostlinkd enabled and started"

if [ "$INSTALL_RO" = "1" ]; then
    systemctl enable --now hostlinkd-ro
    echo "[+] hostlinkd-ro enabled and started"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "=== Installation Complete ==="
echo ""
echo "Full access daemon:"
echo "  Token:  $TOKEN"
echo "  Socket: $SOCKET_PATH"
echo "  Service: systemctl status hostlinkd"
echo ""
if [ "$INSTALL_RO" = "1" ]; then
    echo "Read-only daemon (for AI agents):"
    echo "  Token:  $RO_TOKEN"
    echo "  Socket: /var/lib/hostlink-sockets/hostlink-ro.sock"
    echo "  Shell:  /usr/local/bin/hostlink-ro-shell (write ops blocked)"
    echo "  Service: systemctl status hostlinkd-ro"
    echo ""
fi
echo "Docker-compose volume mounts:"
echo "  - $SOCKET_PATH:$SOCKET_PATH"
if [ "$INSTALL_RO" = "1" ]; then
    echo "  - /var/lib/hostlink-sockets:/var/lib/hostlink-sockets"
fi
echo ""
echo "Client targets.conf:"
echo "  [$(hostname)]"
echo "  transport = unix"
echo "  socket = $SOCKET_PATH"
echo "  token = $TOKEN"
if [ "$INSTALL_RO" = "1" ]; then
    echo ""
    echo "  [$(hostname)-ro]"
    echo "  transport = unix"
    echo "  socket = /var/lib/hostlink-sockets/hostlink-ro.sock"
    echo "  token = $RO_TOKEN"
fi
