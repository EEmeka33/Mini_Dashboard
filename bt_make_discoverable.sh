#!/bin/bash

###############################################################################
# Make Bluetooth Discoverable
# Simply makes the Pi discoverable so your phone can find and pair with it
# via the Pi OS Bluetooth settings GUI
###############################################################################

LOG_FILE="/tmp/bt_discoverable.log"

{
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Making Pi discoverable..."
    
    # Turn on Bluetooth
    bluetoothctl power on 2>&1 | tee -a "$LOG_FILE"
    
    # Make discoverable for 180 seconds (3 minutes) - enough time to pair via GUI
    bluetoothctl discoverable on 2>&1 | tee -a "$LOG_FILE"
    
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Pi is now DISCOVERABLE"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Look for 'raspberrypi' in your phone's Bluetooth settings"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] You have 3 minutes to pair from your phone"
    
    # Keep discoverable for 3 minutes
    sleep 180
    
    # Turn off discoverable
    bluetoothctl discoverable off 2>&1 | tee -a "$LOG_FILE"
    
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Discoverable mode ended"
    
    # Check if anything was paired
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Checking device connections..."
    CONNECTED_DEVICES=$(bluetoothctl devices | wc -l)
    
    if [ "$CONNECTED_DEVICES" -gt 0 ]; then
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Found device(s). Running audio setup..."
        /home/pi5/dashboard/bt_setup_audio.sh
    else
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] No devices found"
    fi
    
} 2>&1 | tee -a "$LOG_FILE"
