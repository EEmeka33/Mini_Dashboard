#!/bin/bash

###############################################################################
# Bluetooth Device Pairing Script
# Usage: ./bt_pair_device.sh [action] [device_address]
# Actions:
#   scan          - Scan for available devices
#   pair ADDRESS  - Pair with device
#   trust ADDRESS - Trust a paired device for auto-connection
#   connect ADDR  - Connect to a device
#   disconnect    - Disconnect from device
#   list          - List paired devices
#   auto          - Interactive mode (scan and pair)
###############################################################################

LOG_FILE="/tmp/bt_pair.log"

log_message() {
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    echo "[$timestamp] $*" >> "$LOG_FILE"
    echo "$*"
}

error_message() {
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    echo "[$timestamp] ERROR: $*" >> "$LOG_FILE"
    echo "ERROR: $*" >&2
}

# Initialize log
echo "=== Bluetooth Pairing Script Started ===" > "$LOG_FILE"
log_message "Script started with args: $@"

# Check if bluetoothctl is available
if ! command -v bluetoothctl &> /dev/null; then
    error_message "bluetoothctl not found. Install bluez: sudo apt install bluez"
    exit 1
fi

ACTION="${1:-auto}"
DEVICE_ADDR="$2"

case "$ACTION" in
    scan)
        log_message "Starting Bluetooth device scan..."
        echo "Scanning for Bluetooth devices. This may take 10-15 seconds..."
        
        # Power on adapter
        bluetoothctl power on >> "$LOG_FILE" 2>&1
        
        # Start scan
        bluetoothctl scan on &
        SCAN_PID=$!
        
        # Scan for 15 seconds
        sleep 15
        
        # Stop scan
        kill $SCAN_PID 2>/dev/null || true
        
        log_message "Scan completed"
        echo ""
        echo "Available Bluetooth devices:"
        bluetoothctl devices | tee -a "$LOG_FILE"
        ;;
        
    pair)
        if [ -z "$DEVICE_ADDR" ]; then
            error_message "Device address required for pair action"
            echo "Usage: $0 pair <device_address>"
            exit 1
        fi
        
        log_message "Pairing with device: $DEVICE_ADDR"
        echo "Attempting to pair with $DEVICE_ADDR..."
        
        # Power on adapter
        bluetoothctl power on >> "$LOG_FILE" 2>&1
        
        # Scan first to ensure device is visible
        log_message "Scanning for device..."
        bluetoothctl scan on &
        SCAN_PID=$!
        sleep 5
        kill $SCAN_PID 2>/dev/null || true
        
        # Pair
        if bluetoothctl pair "$DEVICE_ADDR" >> "$LOG_FILE" 2>&1; then
            log_message "Pairing SUCCESS: $DEVICE_ADDR"
            echo "✓ Pairing successful!"
            
            # Trust device for auto-connection
            if bluetoothctl trust "$DEVICE_ADDR" >> "$LOG_FILE" 2>&1; then
                log_message "Device trusted: $DEVICE_ADDR"
                echo "✓ Device trusted for auto-connection"
            fi
            
            # Try to connect
            if bluetoothctl connect "$DEVICE_ADDR" >> "$LOG_FILE" 2>&1; then
                log_message "Connected to device: $DEVICE_ADDR"
                echo "✓ Connected to device"
            else
                log_message "Connection attempt completed (device may connect automatically)"
                echo "Device will connect when available"
            fi
            
            exit 0
        else
            error_message "Pairing FAILED: $DEVICE_ADDR"
            echo "✗ Pairing failed. Check the log: $LOG_FILE"
            exit 1
        fi
        ;;
        
    trust)
        if [ -z "$DEVICE_ADDR" ]; then
            error_message "Device address required for trust action"
            exit 1
        fi
        
        log_message "Trusting device: $DEVICE_ADDR"
        if bluetoothctl trust "$DEVICE_ADDR" >> "$LOG_FILE" 2>&1; then
            log_message "Device trusted: $DEVICE_ADDR"
            echo "✓ Device trusted for auto-connection"
            exit 0
        else
            error_message "Failed to trust device: $DEVICE_ADDR"
            exit 1
        fi
        ;;
        
    connect)
        if [ -z "$DEVICE_ADDR" ]; then
            error_message "Device address required for connect action"
            exit 1
        fi
        
        log_message "Connecting to device: $DEVICE_ADDR"
        echo "Connecting to $DEVICE_ADDR..."
        
        if bluetoothctl connect "$DEVICE_ADDR" >> "$LOG_FILE" 2>&1; then
            log_message "Connected: $DEVICE_ADDR"
            echo "✓ Connected"
            exit 0
        else
            error_message "Connection failed: $DEVICE_ADDR"
            echo "✗ Connection failed"
            exit 1
        fi
        ;;
        
    disconnect)
        log_message "Disconnecting from device"
        if bluetoothctl disconnect >> "$LOG_FILE" 2>&1; then
            log_message "Disconnected successfully"
            echo "✓ Disconnected"
            exit 0
        else
            error_message "Disconnect failed"
            exit 1
        fi
        ;;
        
    list)
        log_message "Listing paired devices"
        echo "Paired Bluetooth devices:"
        bluetoothctl paired-devices | tee -a "$LOG_FILE"
        ;;
        
    auto)
        log_message "Starting interactive pairing mode"
        
        # Power on
        bluetoothctl power on >> "$LOG_FILE" 2>&1
        
        echo ""
        echo "======================================"
        echo "  Bluetooth Device Pairing"
        echo "======================================"
        echo ""
        
        # Show currently paired devices
        echo "Currently paired devices:"
        bluetoothctl paired-devices | sed 's/^/  /'
        echo ""
        
        # Scan for devices
        echo "Scanning for available Bluetooth devices..."
        echo "(This will take about 10 seconds)"
        echo ""
        
        DEVICES_FILE=$(mktemp)
        
        # Start scan and collect new devices
        bluetoothctl scan on > "$DEVICES_FILE" 2>&1 &
        SCAN_PID=$!
        
        sleep 12
        
        # Stop scan
        kill $SCAN_PID 2>/dev/null || true
        wait $SCAN_PID 2>/dev/null || true
        
        # Get all devices
        DEVICE_LIST=$(bluetoothctl devices | grep -v "^Device")
        
        if [ -z "$DEVICE_LIST" ]; then
            error_message "No devices found during scan"
            exit 1
        fi
        
        # Display devices with numbers
        echo "Found devices:"
        echo ""
        
        DEVICE_ARRAY=()
        COUNT=1
        
        while IFS= read -r line; do
            ADDR=$(echo "$line" | awk '{print $2}')
            NAME=$(echo "$line" | cut -d' ' -f3-)
            echo "  [$COUNT] $ADDR - $NAME"
            DEVICE_ARRAY+=("$ADDR")
            ((COUNT++))
        done <<< "$DEVICE_LIST"
        
        echo ""
        
        if [ ${#DEVICE_ARRAY[@]} -eq 0 ]; then
            error_message "No devices available"
            exit 1
        fi
        
        # For automated UI button press, just pair the first device
        # In interactive mode, this would prompt the user
        SELECTED_DEVICE="${DEVICE_ARRAY[0]}"
        
        echo "Pairing with first device: $SELECTED_DEVICE"
        echo ""
        
        # Execute pairing
        "$0" pair "$SELECTED_DEVICE"
        
        rm -f "$DEVICES_FILE"
        ;;
        
    *)
        echo "Bluetooth Device Pairing Script"
        echo ""
        echo "Usage: $0 <action> [device_address]"
        echo ""
        echo "Actions:"
        echo "  scan              - Scan for available devices"
        echo "  pair ADDRESS      - Pair with a specific device (ADDRESS format: XX:XX:XX:XX:XX:XX)"
        echo "  trust ADDRESS     - Trust a device for auto-connection"
        echo "  connect ADDRESS   - Connect to a device"
        echo "  disconnect        - Disconnect from current device"
        echo "  list              - List paired devices"
        echo "  auto              - Automatic pairing mode (scans and pairs first device)"
        echo ""
        echo "Examples:"
        echo "  $0 scan"
        echo "  $0 pair 14:3F:A6:2F:0B:DE"
        echo "  $0 auto"
        echo ""
        echo "Log file: $LOG_FILE"
        exit 0
        ;;
esac

log_message "Script completed successfully"
exit 0
