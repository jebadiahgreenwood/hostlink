#!/usr/bin/env bash
# setup_dgx_spark.sh — Configure HostLink on the DGX Spark for OpenClaw integration
#
# Run as root on the Spark:
#   git clone https://github.com/jebadiahgreenwood/hostlink ~/hostlink
#   cd ~/hostlink && bash setup_dgx_spark.sh
#
# What this does:
#   1. Builds hostlinkd from source (ARM64, gb10 architecture)
#   2. Installs systemd service with TCP transport (for 10GbE link to desktop)
#   3. Configures node name = "spark"
#   4. Prints the token and connection details for adding to desktop's targets.conf
#
# After running, add to desktop's workspace/.hostlink/targets.conf:
#   [spark]
#   transport = tcp
#   address = 10.0.0.2
#   port = 9876
#   token = <printed below>

set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Must be run as root" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPARK_IP="${SPARK_IP:-10.0.0.2}"
SPARK_PORT="${SPARK_PORT:-9876}"

echo "╔══════════════════════════════════════════════════════╗"
echo "║     DGX Spark — HostLink Setup                       ║"
echo "║     OpenClaw compute node configuration              ║"
echo "╚══════════════════════════════════════════════════════╝"
echo ""
echo "Architecture: $(uname -m)"
echo "Hostname:     $(hostname)"
echo "IP:           $SPARK_IP (expected 10GbE address)"
echo ""

# ── Step 1: Build ──────────────────────────────────────────────────────────────
echo "[1/5] Building hostlinkd..."
cd "$SCRIPT_DIR"

# Ensure build tools available
if ! command -v gcc &>/dev/null; then
    echo "  Installing build tools..."
    apt-get update -qq && apt-get install -y -qq gcc make
fi

make clean && make
echo "  ✅ Build complete: $(ls -lh build/hostlinkd | awk '{print $5}')"

# ── Step 2: Install binary ─────────────────────────────────────────────────────
echo "[2/5] Installing binary..."
cp build/hostlinkd /usr/local/bin/hostlinkd
chmod 755 /usr/local/bin/hostlinkd
echo "  ✅ /usr/local/bin/hostlinkd"

# ── Step 3: Config ────────────────────────────────────────────────────────────
echo "[3/5] Creating config..."
mkdir -p /etc/hostlink /tmp/hostlink_spark_output

TOKEN=$(openssl rand -hex 32)

cat > /etc/hostlink/hostlink.conf << CONF
# DGX Spark HostLink configuration
node_name = spark
auth_token = $TOKEN

# TCP transport for 10GbE link to desktop
unix_enabled = 0
tcp_enabled = 1
tcp_bind = 0.0.0.0
tcp_port = $SPARK_PORT

max_concurrent = 16
default_timeout_ms = 60000
max_timeout_ms = 600000

# Use bash for full environment (needed for SGLang/conda commands)
shell = /bin/bash

default_max_output_bytes = 4194304
max_output_bytes = 134217728
output_tmpdir = /tmp/hostlink_spark_output

log_target = stderr
log_level = info
CONF

chmod 640 /etc/hostlink/hostlink.conf
echo "  ✅ /etc/hostlink/hostlink.conf"

# ── Step 4: Systemd service ────────────────────────────────────────────────────
echo "[4/5] Installing systemd service..."

cat > /etc/systemd/system/hostlinkd.service << SVCEOF
[Unit]
Description=HostLink Command Execution Daemon (DGX Spark)
After=network-online.target
Wants=network-online.target
Documentation=https://github.com/jebadiahgreenwood/hostlink

[Service]
Type=simple
ExecStart=/usr/local/bin/hostlinkd -f -c /etc/hostlink/hostlink.conf
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
SVCEOF

systemctl daemon-reload
systemctl enable --now hostlinkd
sleep 2
STATUS=$(systemctl is-active hostlinkd)
echo "  ✅ hostlinkd: $STATUS"

# ── Step 5: Verify ────────────────────────────────────────────────────────────
echo "[5/5] Verifying..."
sleep 1
if /usr/local/bin/hostlink-cli -a "127.0.0.1:$SPARK_PORT" -k "$TOKEN" ping 2>/dev/null | grep -q "pong"; then
    echo "  ✅ TCP connection verified (localhost)"
else
    echo "  ⚠️  Local ping check skipped (hostlink-cli not installed or service starting)"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "╔══════════════════════════════════════════════════════╗"
echo "║     Setup Complete!                                  ║"
echo "╚══════════════════════════════════════════════════════╝"
echo ""
echo "Node name:  spark"
echo "Transport:  TCP"
echo "Address:    $SPARK_IP:$SPARK_PORT"
echo "Token:      $TOKEN"
echo ""
echo "Add to desktop's workspace/.hostlink/targets.conf:"
echo ""
echo "  [spark]"
echo "  transport = tcp"
echo "  address = $SPARK_IP"
echo "  port = $SPARK_PORT"
echo "  token = $TOKEN"
echo ""
echo "Then from the OpenClaw container:"
echo "  hostlink -t spark ping"
echo "  → [spark] pong - uptime Xs"
echo ""
echo "Check service status:  systemctl status hostlinkd"
echo "View logs:             journalctl -u hostlinkd -f"
echo ""

# Also check if we should install hostlink-cli
if [ ! -f build/hostlink-cli ]; then
    echo "Note: hostlink-cli not built (client not needed on Spark, only daemon)"
fi

# Print GPU info for the record
echo "GPU info:"
nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv,noheader 2>/dev/null \
    | sed 's/^/  /' || echo "  (nvidia-smi not available or no GPU detected)"
