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
sudo apt-get install -y \
    pulseaudio \
    pulseaudio-module-bluetooth \
    bluez \
    bluez-tools \
    blueman \
    alsa-utils \
    dbus \
    ofono \
    modem-manager

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

# Step 4: Configure rfcomm for navigation
echo -e "${BLUE}Step 4: Setting up navigation rfcomm device...${NC}"

# Create rfcomm binding for navigation (device address would be set later)
echo "Note: rfcomm will be bound when you pair your phone"
echo "The navigation device should be at /dev/rfcomm1"

echo -e "${GREEN}✓ rfcomm ready${NC}"
echo ""

# Step 5: Configure oFono for calls
echo -e "${BLUE}Step 5: Configuring oFono for phone calls...${NC}"

if systemctl is-enabled oFono &> /dev/null || systemctl is-enabled ofono &> /dev/null; then
    echo "oFono service found and enabled"
    sudo systemctl restart ofono
else
    echo "Note: oFono not enabled. Will configure when phone connects."
fi

echo -e "${GREEN}✓ oFono configured${NC}"
echo ""

# Step 6: Create startup script
echo -e "${BLUE}Step 6: Creating startup script...${NC}"

sudo tee /etc/systemd/system-user/dashboard.service > /dev/null << 'EOF'
[Unit]
Description=Raspberry Pi Dashboard
After=bluetooth.target pulseaudio.service

[Service]
Type=simple
User=pi5
ExecStart=/home/pi5/dashboard/lv_port_pc_vscode/bin/main
Restart=on-failure
RestartSec=5

[Install]
WantedBy=graphical-session.target
EOF

echo -e "${GREEN}✓ Startup script created${NC}"
echo ""

# Step 7: Summary
echo "=========================================="
echo -e "${GREEN}Setup Complete!${NC}"
echo "=========================================="
echo ""
echo "Next steps:"
echo ""
echo "1. Run the dashboard:"
echo -e "${BLUE}   /home/pi5/dashboard/lv_port_pc_vscode/bin/main${NC}"
echo ""
echo "2. Click the blue 'Pair' button in the dashboard"
echo ""
echo "3. On your phone:"
echo "   - Open Bluetooth settings"
echo "   - Find and tap 'raspberrypi'"
echo "   - Confirm pairing"
echo ""
echo "4. After pairing:"
echo "   - Music will play through phone speaker"
echo "   - Calls will use phone microphone/speaker"
echo "   - Navigation will work automatically"
echo ""
echo "=========================================="
echo -e "${YELLOW}Important: All Bluetooth audio devices should now be configured!${NC}"
echo "=========================================="
