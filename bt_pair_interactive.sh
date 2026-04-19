#!/bin/bash

###############################################################################
# Advanced Bluetooth Pairing with PIN Handling
# This script handles pairing with PIN code requirements
# Can be called with: interactive_pair.sh [device_address]
###############################################################################

LOG_FILE="/tmp/bt_pair_adv.log"

{
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Advanced Pairing Started"
    
    # Enable Bluetooth
    bluetoothctl power on 2>&1 | tee -a "$LOG_FILE"
    sleep 1
    
    if [ -z "$1" ]; then
        echo "Usage: $0 <device_address>"
        echo "Example: $0 14:3F:A6:2F:0B:DE"
        exit 1
    fi
    
    DEVICE_ADDR="$1"
    
    # Check if device is already paired
    if bluetoothctl paired-devices 2>/dev/null | grep -q "$DEVICE_ADDR"; then
        echo "Device already paired: $DEVICE_ADDR"
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Device already paired" >> "$LOG_FILE"
        
        # Try to connect
        echo "Attempting to connect..."
        bluetoothctl connect "$DEVICE_ADDR" 2>&1 | tee -a "$LOG_FILE"
        exit 0
    fi
    
    echo "Attempting to pair with: $DEVICE_ADDR"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Pairing with $DEVICE_ADDR" >> "$LOG_FILE"
    
    # Make discoverable
    bluetoothctl discoverable on 2>&1 | tee -a "$LOG_FILE"
    
    # Enable pairing agent
    bluetoothctl agent on 2>&1 | tee -a "$LOG_FILE"
    bluetoothctl default-agent 2>&1 | tee -a "$LOG_FILE"
    
    # Attempt pairing with timeout
    # The --timeout flag helps handle cases where PIN is needed
    timeout 60 bluetoothctl pair "$DEVICE_ADDR" 2>&1 | tee -a "$LOG_FILE"
    PAIR_RESULT=$?
    
    if [ $PAIR_RESULT -eq 0 ] || [ $PAIR_RESULT -eq 124 ]; then
        # 124 is timeout (which might mean it's waiting for user confirmation on phone)
        echo "Pairing initiated..."
        sleep 5
        
        # Check if now paired
        if bluetoothctl paired-devices 2>/dev/null | grep -q "$DEVICE_ADDR"; then
            echo "✓ Pairing successful!"
            echo "[$(date '+%Y-%m-%d %H:%M:%S')] Pairing SUCCESS" >> "$LOG_FILE"
            
            # Trust and connect
            bluetoothctl trust "$DEVICE_ADDR" 2>&1 | tee -a "$LOG_FILE"
            sleep 2
            bluetoothctl connect "$DEVICE_ADDR" 2>&1 | tee -a "$LOG_FILE"
            
            bluetoothctl discoverable off 2>&1 | tee -a "$LOG_FILE"
            exit 0
        fi
    fi
    
    echo "✗ Pairing failed. Try these steps:"
    echo "1. Check the pairing code on your phone"
    echo "2. Confirm the pairing on your phone"
    echo "3. Or try again with the PIR address"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Pairing may need phone confirmation" >> "$LOG_FILE"
    
    bluetoothctl discoverable off 2>&1 | tee -a "$LOG_FILE"
    exit 1
    
} | tee -a "$LOG_FILE"
