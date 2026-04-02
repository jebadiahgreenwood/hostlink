#!/usr/bin/env bash
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "install.sh must be run as root" >&2
    exit 1
fi

echo "=== HostLink Installer ==="

# Check binaries exist
if [ ! -f build/hostlinkd ] || [ ! -f build/hostlink-cli ]; then
    echo "Binaries not found. Run ./build.sh release first." >&2
    exit 1
fi

# Install binaries
cp build/hostlinkd   /usr/local/bin/hostlinkd
cp build/hostlink-cli /usr/local/bin/hostlink-cli
chmod 755 /usr/local/bin/hostlinkd /usr/local/bin/hostlink-cli
echo "[+] Installed binaries to /usr/local/bin/"

# Create hostlink user/group
if ! getent group hostlink >/dev/null 2>&1; then
    groupadd --system hostlink
    echo "[+] Created group: hostlink"
fi
if ! getent passwd hostlink >/dev/null 2>&1; then
    useradd --system --no-create-home --shell /usr/sbin/nologin \
            --gid hostlink --comment "HostLink daemon" hostlink
    echo "[+] Created user: hostlink"
fi

# Create config directory
mkdir -p /etc/hostlink
chmod 750 /etc/hostlink
chown root:hostlink /etc/hostlink

# Generate config if not present
if [ ! -f /etc/hostlink/hostlink.conf ]; then
    TOKEN=$(openssl rand -hex 32)
    sed "s/REPLACE_WITH_GENERATED_TOKEN/$TOKEN/" hostlink.conf.example \
        > /etc/hostlink/hostlink.conf
    chmod 640 /etc/hostlink/hostlink.conf
    chown root:hostlink /etc/hostlink/hostlink.conf
    echo "[+] Generated config: /etc/hostlink/hostlink.conf"
    echo "[+] Auth token: $TOKEN"
    echo "    (Save this token — you'll need it in your targets.conf)"
else
    echo "[=] Config already exists: /etc/hostlink/hostlink.conf (not overwritten)"
    TOKEN=$(grep auth_token /etc/hostlink/hostlink.conf | awk -F'=' '{print $2}' | tr -d ' ')
fi

# Install systemd unit
cp hostlink.service /etc/systemd/system/hostlink.service
chmod 644 /etc/systemd/system/hostlink.service
echo "[+] Installed systemd unit"

# Enable and start
systemctl daemon-reload
systemctl enable --now hostlink
echo "[+] Service enabled and started"

echo ""
echo "=== Installation Complete ==="
echo ""
echo "Auth token:   $TOKEN"
echo "Socket:       /run/hostlink/hostlink.sock"
echo "Config:       /etc/hostlink/hostlink.conf"
echo ""
echo "Configure your client targets file (~/.config/hostlink/targets.conf):"
echo ""
echo "  [$(hostname)]"
echo "  transport = unix"
echo "  socket = /run/hostlink/hostlink.sock"
echo "  token = $TOKEN"
echo ""
echo "For Docker, mount the socket and output dir:"
echo "  -v /run/hostlink/hostlink.sock:/run/hostlink/hostlink.sock"
echo "  -v /run/hostlink/output:/run/hostlink/output:ro"
