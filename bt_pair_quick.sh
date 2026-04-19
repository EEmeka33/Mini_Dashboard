#!/bin/bash

###############################################################################
# Quick Bluetooth Pairing Script for UI Button (IMPROVED - SILENT MODE)
# This script will:
# 1. Run silently (no terminal output when called from UI)
# 2. Log to /tmp/bt_pair_quick.log
# 3. Write results to /tmp/bt_pair_result.txt
# 4. Not interfere with dashboard UI
###############################################################################

OUTPUT_FILE="/tmp/bt_pair_result.txt"
LOG_FILE="/tmp/bt_pair_quick.log"
PAIRING_TIMEOUT=30  # seconds

# When called from UI (stdin is /dev/null), suppress all output
# When called from terminal, show output for debugging
SUPPRESS_OUTPUT=0
if [ ! -t 0 ]; then
    # stdin is not a terminal - we're being called from UI
    # Suppress all output to avoid interfering
    SUPPRESS_OUTPUT=1
    exec 1>/dev/null 2>&1
fi

{
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Quick Pairing Started (PID: $$)"
    
    # CRITICAL: Check for conflicting Bluetooth UIs and warn about them
    # The Pi OS Bluetooth GUI can interfere with our pairing
    CONFLICTING_PROCESS=$(pgrep -f "bluetooth" | grep -v $$ | head -1)
    if [ ! -z "$CONFLICTING_PROCESS" ]; then
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] WARNING: Another Bluetooth process detected (PID: $CONFLICTING_PROCESS)"
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] You should close the Raspberry Pi Bluetooth settings UI to avoid conflicts"
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Continuing anyway..."
    fi
    
    # Step 1: Turn on Bluetooth
    echo "Enabling Bluetooth..."
    bluetoothctl power on 2>&1 | tee -a "$LOG_FILE"
    sleep 1
    
    # Step 2: Make discoverable (allows phone to initiate pairing)
    echo "Making device discoverable..."
    bluetoothctl discoverable on 2>&1 | tee -a "$LOG_FILE"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Discoverable mode enabled" >> "$LOG_FILE"
    
    # Step 3: Get currently paired devices BEFORE scan
    echo "Getting current device list..."
    PAIRED_BEFORE=$(bluetoothctl paired-devices 2>/dev/null | awk '{print $2}' | sort)
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Paired before: $PAIRED_BEFORE" >> "$LOG_FILE"
    
    # Step 4: Scan for devices (simpler, more reliable approach)
    echo "Scanning for devices (${PAIRING_TIMEOUT} seconds)..."
    bluetoothctl scan on > /dev/null 2>&1 &
    SCAN_PID=$!
    
    # Show countdown
    for i in $(seq 1 "$PAIRING_TIMEOUT"); do
        echo -ne "\rScanning: $i/${PAIRING_TIMEOUT} seconds..."
        sleep 1
    done
    echo ""
    
    # Stop scan properly
    kill $SCAN_PID 2>/dev/null || true
    wait $SCAN_PID 2>/dev/null || true
    bluetoothctl scan off > /dev/null 2>&1
    sleep 2
    
    # Step 5: Get all devices after scan
    echo "Retrieving discovered devices..."
    ALL_DEVICES=$(bluetoothctl devices | awk '{if (NR>0 && $1=="Device") print $2}' | sort)
    echo "All discovered devices:"
    bluetoothctl devices | tee -a "$LOG_FILE"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] All devices: $ALL_DEVICES" >> "$LOG_FILE"
    
    # Step 6: Find a device to pair with (prefer unpaired, but accept any device found)
    CANDIDATE_DEVICES=""
    
    # First, look for new unpaired devices
    while IFS= read -r addr; do
        [ -z "$addr" ] && continue
        if ! echo "$PAIRED_BEFORE" | grep -q "^$addr$"; then
            CANDIDATE_DEVICES="$addr"
            echo "[$(date '+%Y-%m-%d %H:%M:%S')] Found new device: $addr" >> "$LOG_FILE"
            break  # Use first found
        fi
    done <<< "$ALL_DEVICES"
    
    # If no new devices, use the first discovered device anyway
    if [ -z "$CANDIDATE_DEVICES" ]; then
        echo "No new unpaired devices. Using first discovered device..."
        CANDIDATE_DEVICES=$(echo "$ALL_DEVICES" | head -1)
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Using discovered device: $CANDIDATE_DEVICES" >> "$LOG_FILE"
    fi
    
    if [ -z "$CANDIDATE_DEVICES" ]; then
        echo "❌ No devices found!" | tee "$OUTPUT_FILE"
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] No devices found" >> "$LOG_FILE"
        bluetoothctl discoverable off 2>&1 | tee -a "$LOG_FILE"
        exit 1
    fi
    
    DEVICE_ADDR="$CANDIDATE_DEVICES"
    DEVICE_NAME=$(bluetoothctl info "$DEVICE_ADDR" 2>/dev/null | grep "Name:" | cut -d' ' -f2-)
    
    if [ -z "$DEVICE_NAME" ]; then
        DEVICE_NAME="Unknown Device"
    fi
    
    echo "✓ Found device: $DEVICE_ADDR ($DEVICE_NAME)"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Found device: $DEVICE_ADDR ($DEVICE_NAME)" >> "$LOG_FILE"
    
    # Step 7: Attempt pairing
    echo "🔵 Pairing with $DEVICE_ADDR ($DEVICE_NAME)..."
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting pairing process..." >> "$LOG_FILE"
    
    # First, try removing if it was previously failed/partially paired
    echo "Clearing any previous pairing state..."
    bluetoothctl remove "$DEVICE_ADDR" 2>&1 | tee -a "$LOG_FILE"
    sleep 1
    
    # Now attempt fresh pairing
    if bluetoothctl pair "$DEVICE_ADDR" 2>&1 | tee -a "$LOG_FILE"; then
        echo "✓ Pairing successful!"
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Pairing successful" >> "$LOG_FILE"
        
        # Trust device
        echo "Trusting device..."
        bluetoothctl trust "$DEVICE_ADDR" 2>&1 | tee -a "$LOG_FILE"
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Device trusted" >> "$LOG_FILE"
        
        # Try to connect
        echo "Attempting connection..."
        sleep 2
        bluetoothctl connect "$DEVICE_ADDR" 2>&1 | tee -a "$LOG_FILE"
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Connection attempted" >> "$LOG_FILE"
        
        echo "✅ SUCCESS: $DEVICE_ADDR - $DEVICE_NAME" | tee "$OUTPUT_FILE"
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Pairing SUCCESS" >> "$LOG_FILE"
        bluetoothctl discoverable off 2>&1 | tee -a "$LOG_FILE"
        exit 0
    else
        echo "❌ Pairing failed!"
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Pairing FAILED" >> "$LOG_FILE"
        echo "FAILED: Pairing unsuccessful with $DEVICE_ADDR ($DEVICE_NAME)" | tee "$OUTPUT_FILE"
        bluetoothctl discoverable off 2>&1 | tee -a "$LOG_FILE"
        exit 1
    fi
    
} | tee -a "$LOG_FILE"
