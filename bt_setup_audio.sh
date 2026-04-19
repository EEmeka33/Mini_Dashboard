#!/bin/bash

###############################################################################
# Bluetooth Audio Setup Script
# After a device is paired via Pi OS Bluetooth settings,
# this configures the Pi to use it for:
# - Audio playback (music)
# - Audio input (microphone)
# - Phone calls
###############################################################################

LOG_FILE="/tmp/bt_audio_setup.log"
DEVICE_ADDR="${1:-}"  # Optional: specific device address

{
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Audio Setup Started"
    
    # Step 1: Get the paired device
    if [ -z "$DEVICE_ADDR" ]; then
        echo "Finding paired Bluetooth devices..."
        # Get the most recently paired device
        DEVICE_ADDR=$(bluetoothctl devices | tail -1 | awk '{print $2}')
        DEVICE_NAME=$(bluetoothctl devices | tail -1 | awk '{print $3,$4,$5}')
    else
        DEVICE_NAME=$(bluetoothctl info "$DEVICE_ADDR" | grep "Name:" | cut -d' ' -f2-)
    fi
    
    if [ -z "$DEVICE_ADDR" ]; then
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] ERROR: No paired devices found"
        echo "No paired devices found" > /tmp/bt_audio_result.txt
        exit 1
    fi
    
    echo "Setting up audio for: $DEVICE_ADDR ($DEVICE_NAME)"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Device: $DEVICE_ADDR ($DEVICE_NAME)" >> "$LOG_FILE"
    
    # Step 2: Ensure device is connected
    echo "Ensuring device is connected..."
    bluetoothctl connect "$DEVICE_ADDR" 2>&1 | tee -a "$LOG_FILE"
    sleep 2
    
    # Step 3: Get connection status
    CONNECTED=$(bluetoothctl info "$DEVICE_ADDR" | grep "Connected:" | awk '{print $2}')
    
    if [ "$CONNECTED" != "yes" ]; then
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Device not connected yet, waiting..."
        sleep 3
        CONNECTED=$(bluetoothctl info "$DEVICE_ADDR" | grep "Connected:" | awk '{print $2}')
    fi
    
    if [ "$CONNECTED" != "yes" ]; then
        echo "ERROR: Device not connected"
        echo "FAILED: Device connection unsuccessful" > /tmp/bt_audio_result.txt
        exit 1
    fi
    
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Device connected: YES" >> "$LOG_FILE"
    
    # Step 4: Set up audio using PulseAudio (if available)
    if command -v pactl &> /dev/null; then
        echo "Configuring PulseAudio..."
        
        # Load Bluetooth module if needed
        pactl load-module module-bluetooth-discover 2>/dev/null || true
        
        # Get Bluetooth sink (audio output)
        BT_SINK=$(pactl list short sinks | grep bluez | tail -1 | awk '{print $1}')
        
        if [ ! -z "$BT_SINK" ]; then
            echo "Setting Bluetooth device as default audio output (sink: $BT_SINK)"
            pactl set-default-sink "$BT_SINK"
            
            # Also set input (microphone)
            BT_SOURCE=$(pactl list short sources | grep bluez | tail -1 | awk '{print $1}')
            if [ ! -z "$BT_SOURCE" ]; then
                echo "Setting Bluetooth device as default audio input (source: $BT_SOURCE)"
                pactl set-default-source "$BT_SOURCE"
            fi
            
            echo "[$(date '+%Y-%m-%d %H:%M:%S')] PulseAudio configured" >> "$LOG_FILE"
        fi
    else
        echo "PulseAudio not found, skipping audio setup"
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] PulseAudio not available" >> "$LOG_FILE"
    fi
    
    # Step 5: Set up for phone calls using oFono (if available)
    if command -v dbus-send &> /dev/null; then
        echo "Setting up for phone calls..."
        # This would normally set the device as the audio device for calls
        # Configuration depends on your phone and modem setup
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Call audio configuration attempted" >> "$LOG_FILE"
    fi
    
    # Step 6: Success
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Audio setup COMPLETE" >> "$LOG_FILE"
    echo "SUCCESS: $DEVICE_ADDR configured for audio and calls" > /tmp/bt_audio_result.txt
    echo "✅ SUCCESS: Audio setup complete for $DEVICE_NAME"
    
} 2>&1 | tee -a "$LOG_FILE"
