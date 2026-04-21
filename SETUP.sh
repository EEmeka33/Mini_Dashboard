#!/bin/bash

###############################################################################
# Raspberry Pi Dashboard - One-Time Setup Script
# Run this ONCE after initial installation to set up:
# - Audio playback through Bluetooth
# - Audio input/microphone through Bluetooth
# - Phone calls through Bluetooth
# - Navigation through rfcomm
###############################################################################

echo "=========================================="
echo "  Raspberry Pi Dashboard Setup"
echo "=========================================="
echo ""

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo -e "${YELLOW}Note: Some commands may require sudo${NC}"
fi

# Step 1: Install required packages
echo -e "${BLUE}Step 1: Installing required packages...${NC}"
sudo apt-get update
sudo apt-get install -y pulseaudio pulseaudio-module-bluetooth bluez
sudo apt-get install -y bluez-tools alsa-utils dbus
# Note: ofono and modemmanager conflict - we choose ofono for HFP phone calls
sudo apt-get install -y ofono
sudo apt-get install -y libdbus-1-dev libcurl4-openssl-dev libjson-c-dev
sudo apt-get install -y libsdl2-dev libsdl2-image-dev

echo -e "${GREEN}✓ Packages installed${NC}"
echo ""

# Step 2: Configure PulseAudio for Bluetooth
echo -e "${BLUE}Step 2: Configuring PulseAudio...${NC}"

# Enable PulseAudio Bluetooth module
sudo usermod -aG audio pi5

# Start PulseAudio if not running
if ! systemctl --user is-active --quiet pulseaudio; then
    echo "Starting PulseAudio..."
    systemctl --user start pulseaudio
else
    echo "PulseAudio already running"
fi

echo -e "${GREEN}✓ PulseAudio configured${NC}"
echo ""

# Step 3: Configure Bluetooth
echo -e "${BLUE}Step 3: Configuring Bluetooth daemon...${NC}"

# Update Bluetooth config for A2DP (audio) and HFP (calls)
sudo tee /etc/bluetooth/main.conf > /dev/null << 'EOF'
[General]
# Enable A2DP source (for phone to send audio to Pi)
Enable=Source

# Enable HFP audio gateway (for hands-free calls)
HandsfreeDevice=true

# Trustable devices
Trustable=true

# Profile whitelisting
DisabledPlugins=pnat,media_provider

[LE]
# Enable Low Energy audio
EOF

# Restart Bluetooth to apply config
sudo systemctl restart bluetooth

echo -e "${GREEN}✓ Bluetooth configured${NC}"
echo ""

# Step 4: Configure RFCOMM daemon for navigation
echo -e "${BLUE}Step 4: Setting up RFCOMM daemon for navigation...${NC}"

# Copy nav-bind daemon script
DAEMON_SCRIPT="$(dirname "$0")/nav-bind-daemon.sh"
if [ -f "$DAEMON_SCRIPT" ]; then
    sudo cp "$DAEMON_SCRIPT" /usr/local/bin/nav-bind-daemon.sh
    sudo chmod +x /usr/local/bin/nav-bind-daemon.sh
    echo "✓ nav-bind-daemon.sh copied to /usr/local/bin/"
else
    echo "Warning: nav-bind-daemon.sh not found in $(dirname "$0")"
fi

# Create systemd service for nav-bind daemon
sudo tee /etc/systemd/system/nav-bind.service > /dev/null << 'EOF'
[Unit]
Description=RFCOMM auto-bind daemon for NavJsonService
After=bluetooth.target
Wants=bluetooth.target

[Service]
Type=simple
ExecStart=/usr/local/bin/nav-bind-daemon.sh
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

echo -e "${GREEN}✓ RFCOMM daemon installed as systemd service${NC}"
echo "Note: The phone MAC is automatically detected from main.c"
echo "      and stored in /tmp/connected_phone_mac.txt"
echo ""

# Step 5: Configure oFono for calls
echo -e "${BLUE}Step 5: Configuring oFono for phone calls...${NC}"

# Enable ofono if available (it may not be in all Debian versions)
if apt-cache show ofono >/dev/null 2>&1; then
    sudo systemctl enable ofono.service 2>/dev/null || true
    sudo systemctl restart ofono.service 2>/dev/null || true
    echo -e "${GREEN}✓ oFono configured${NC}"
else
    echo "Note: oFono not available in this Debian version - phone calls may not work"
fi
echo ""

# Step 6: Enable nav-bind service to start automatically
echo -e "${BLUE}Step 6: Enabling services to start at boot...${NC}"

sudo systemctl daemon-reload
sudo systemctl enable nav-bind.service
sudo systemctl start nav-bind.service
sudo systemctl enable bluetooth.service

echo "✓ Services enabled:"
echo "  - nav-bind.service (RFCOMM daemon)"
echo "  - bluetooth.service (Bluetooth)"
echo ""

# Step 7: Create user startup script for dashboard
echo -e "${BLUE}Step 7: Creating dashboard startup script...${NC}"

# Create user systemd directory if it doesn't exist
mkdir -p ~/.config/systemd/user

tee ~/.config/systemd/user/dashboard.service > /dev/null << 'EOF'
[Unit]
Description=Raspberry Pi Dashboard
After=bluetooth.target pulseaudio.service

[Service]
Type=simple
ExecStart=/home/pi5/dashboard/lv_port_pc_vscode/bin/main
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
EOF

echo -e "${GREEN}✓ Startup script created${NC}"
echo ""

# Step 8: Summary
echo "=========================================="
echo -e "${GREEN}Setup Complete!${NC}"
echo "=========================================="
echo ""
echo "System components installed:"
echo "  ✓ Bluetooth audio (pulseaudio)"
echo "  ✓ Phone calls (oFono)"
echo "  ✓ Navigation daemon (nav-bind-daemon.sh)"
echo "  ✓ All required libraries"
echo ""
echo "Automatic features:"
echo "  ✓ Phone MAC detection (main.c writes to /tmp/connected_phone_mac.txt)"
echo "  ✓ RFCOMM binding (nav-bind daemon auto-configures)"
echo "  ✓ Services auto-restart on crash"
echo ""
echo "Next steps to start the dashboard:"
echo ""
echo "1. Compile the dashboard:"
echo -e "${BLUE}   cd lv_port_pc_vscode && rm -rf build && mkdir build && cd build"
echo "   cmake .. && make -j4${NC}"
echo ""
echo "2. Start the dashboard:"
echo -e "${BLUE}   /home/pi5/dashboard/lv_port_pc_vscode/bin/main${NC}"
echo ""
echo "3. Pair your phone:"
echo "   - Click the blue 'Pair' button in the dashboard"
echo "   - On your phone, go to Bluetooth settings"
echo "   - Find and tap 'raspberrypi'"
echo "   - Confirm pairing"
echo ""
echo "4. Start navigation:"
echo "   - Install NavRelay on your Android phone"
echo "   - Open NavRelay and click 'Start'"
echo "   - Open Google Maps and start navigation"
echo ""
echo "5. Verify all services are running:"
echo -e "${BLUE}   sudo systemctl status bluetooth.service"
echo "   sudo systemctl status ofono.service"
echo "   sudo systemctl status nav-bind.service${NC}"
echo ""
echo "=========================================="
echo -e "${YELLOW}Documentation: see docs/DOCUMENTATION_TECHNIQUE_COMPLETE.tex${NC}"
echo "=========================================="
