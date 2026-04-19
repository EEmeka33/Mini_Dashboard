# Quick Setup & Usage Guide

## One-Time Installation Setup

Run this ONCE when you first set up the system:

```bash
/home/pi5/dashboard/SETUP.sh
```

This will:
- ✅ Install all required packages (PulseAudio, Bluez, etc.)
- ✅ Configure Bluetooth for audio and calls
- ✅ Set up PulseAudio for Bluetooth devices
- ✅ Configure oFono for phone call handling
- ✅ Create systemd service (optional)

**Duration:** ~5-10 minutes

---

## Daily Usage

### 1. Start the Dashboard
```bash
/home/pi5/dashboard/lv_port_pc_vscode/bin/main
```

### 2. Make Pi Discoverable (First Time Only)
- Click the blue **"Pair"** button in the dashboard
- Screen shows: `Made Discoverable`
- This lasts for 3 minutes

### 3. Pair Your Phone
On your phone:
1. Open Bluetooth settings
2. Search for devices
3. Find and tap **"raspberrypi"**
4. Confirm the pairing request
5. Enter PIN if prompted

### 4. Automatic Setup
Once paired, the following happen automatically:
- ✅ Music plays through the phone speaker
- ✅ Microphone input works for calls
- ✅ Phone calls use the Bluetooth connection
- ✅ Navigation data flows through rfcomm

---

## What the Pair Button Does

| Click | Action | Status |
|-------|--------|--------|
| 1st time | Makes Pi discoverable | "Made Discoverable" |
| After pairing | Shows "Made Discoverable" | (Can pair additional devices) |
| On error | Shows error status | "Error" |

---

## Troubleshooting

### Phone Can't Find "raspberrypi"

**Solution 1: Check Bluetooth is enabled on Pi**
```bash
bluetoothctl power on
bluetoothctl show
# Should show: Powered: yes
```

**Solution 2: Make discoverable manually**
```bash
bluetoothctl discoverable on
# Your phone should now find it
```

### Audio Not Working After Pairing

**Check PulseAudio:**
```bash
pactl list short sinks
# Should show Bluetooth device
```

**Restart PulseAudio:**
```bash
systemctl --user restart pulseaudio
```

### Phone Can't Connect for Calls

**Verify oFono:**
```bash
systemctl status ofono
# Should be running
```

---

## Command Reference

### Make Pi Discoverable
```bash
bluetoothctl discoverable on
```

### List Connected Devices
```bash
bluetoothctl devices
```

### Disconnect a Device
```bash
bluetoothctl disconnect <device_address>
```

### Remove a Pairing
```bash
bluetoothctl remove <device_address>
```

### Restart Bluetooth
```bash
sudo systemctl restart bluetooth
```

### Check Bluetooth Status
```bash
bluetoothctl show
```

### Monitor Bluetooth Activity
```bash
sudo journalctl -u bluetooth -f
```

---

## Dashboard Features After Pairing

Once your phone is paired, the dashboard shows:

- **Navigation** (center): Maps, turns, distance
- **Music** (bottom): Current track, artist, album art, playback controls
- **Calls** (appears on incoming call): Phone number, answer/hangup buttons
- **Pair Button** (bottom): Can pair additional devices
- **Speedometer** (background): Real-time speed indicator

---

## File Locations

```
Dashboard executable:
/home/pi5/dashboard/lv_port_pc_vscode/bin/main

Setup script:
/home/pi5/dashboard/SETUP.sh

Bluetooth scripts:
/home/pi5/dashboard/bt_make_discoverable.sh
/home/pi5/dashboard/bt_setup_audio.sh

Logs:
/tmp/bt_discoverable.log
/tmp/bt_audio_setup.log
```

---

## Support

For issues, check:

```bash
# Dashboard logs
tail -f /tmp/dashboard.log

# Bluetooth logs
sudo journalctl -u bluetooth -f

# PulseAudio logs
journalctl --user -u pulseaudio -f

# oFono logs (for calls)
sudo journalctl -u ofono -f
```

---

## Complete Workflow

1. **Installation (once):**
   ```bash
   /home/pi5/dashboard/SETUP.sh
   ```

2. **Daily startup:**
   ```bash
   /home/pi5/dashboard/lv_port_pc_vscode/bin/main
   ```

3. **First-time pairing:**
   - Click Pair button
   - Pair from phone's Bluetooth settings
   - Enjoy!

4. **Daily use:**
   - Dashboard works automatically
   - Music, calls, and navigation all through Bluetooth
   - No additional setup needed

---

**Status:** ✅ Ready for deployment!
