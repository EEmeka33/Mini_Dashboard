# Testing Bluetooth Pairing Button - Original Threaded Version

## Setup Instructions

### 1. Verify the Build Completed
Wait for the build to complete:
```bash
# Check if executable was built
ls -lah /home/pi5/dashboard/lv_port_pc_vscode/bin/main
```

If successful, you should see a 3+ MB executable.

### 2. Verify Scripts Are Executable
```bash
ls -lah /home/pi5/dashboard/bt_pair*.sh
# Should show -rwxrwxr-x permissions
```

### 3. Install Bluetooth Tools (if not already installed)
```bash
sudo apt update
sudo apt install bluez bluez-tools
```

### 4. Add User to Bluetooth Group
```bash
sudo usermod -aG bluetooth pi5
# Log out and log back in for changes to take effect
```

## Testing Procedures

### Test 1: Manual Script Testing

Before testing the UI button, test the scripts directly:

#### Test 1a: Scan for available devices
```bash
/home/pi5/dashboard/bt_pair_device.sh scan
cat /tmp/bt_pair.log
```

#### Test 1b: Quick auto-pairing (IMPROVED - handles both new and previously paired devices)
```bash
/home/pi5/dashboard/bt_pair_quick.sh
cat /tmp/bt_pair_result.txt
cat /tmp/bt_pair_quick.log
```

This script now:
- Makes Pi discoverable (allows your phone to find it)
- Sets up pairing agent (easier PIN handling)
- Finds both new AND previously paired devices
- Better PIN code support

#### Test 1c: Full-featured pairing with device address
```bash
/home/pi5/dashboard/bt_pair_device.sh pair AA:BB:CC:DD:EE:FF
cat /tmp/bt_pair.log
```

#### Test 1d: Interactive pairing (NEW - for PIN code handling)
```bash
/home/pi5/dashboard/bt_pair_interactive.sh AA:BB:CC:DD:EE:FF
cat /tmp/bt_pair_adv.log
```

This script is useful when you need to handle PIN code prompts.

### Test 2: Dashboard UI Button Testing

#### 2a. Start the Dashboard
```bash
/home/pi5/dashboard/lv_port_pc_vscode/bin/main
```

You should see:
```
=== Round Dashboard (1080x1080) ===
Navigation | Music Player | Calls
Connected to /dev/rfcomm1
Nav updated: 33 m, towards Rue des Mésanges
```

#### 2b. Test the Pair Button

1. **Locate the Pair Button**
   - It's below the music player controls (Prev, Play, Next)
   - Blue button with "Pair" label
   - Located at the bottom center of the music area

2. **Click the Pair Button**
   - Press/tap the blue "Pair" button
   - You should see debug output in terminal:
     ```
     [UI] Starting Bluetooth pairing...
     [UI] Pairing task spawned in background
     [BT Pairing] Background task started (PID: XXXX)
     ```

3. **Wait for Pairing to Complete**
   - The button area will show "Pairing..." message
   - Pairing takes about 15-20 seconds
   - Check the logs while pairing proceeds:
     ```bash
     tail -f /tmp/bt_pair_quick.log
     ```

4. **Verify Results**
   - On success: The device name appears below the music controls
   - On failure: "Failed" message appears
   - Check the result file:
     ```bash
     cat /tmp/bt_pair_result.txt
     ```

### Test 3: Automated Testing Flow

```bash
#!/bin/bash
# automated_test.sh

echo "=== Bluetooth Pairing Button Test ==="
echo ""

# Clean up old results
rm -f /tmp/bt_pair_*.log /tmp/bt_pair_result.txt

echo "1. Testing manual script..."
/home/pi5/dashboard/bt_pair_device.sh scan | head -20
echo ""

echo "2. Starting dashboard..."
timeout 120 /home/pi5/dashboard/lv_port_pc_vscode/bin/main &
DASHBOARD_PID=$!
sleep 5

echo "3. Waiting for pairing to complete..."
sleep 30

echo "4. Checking results..."
cat /tmp/bt_pair_result.txt 2>/dev/null || echo "No result yet"
cat /tmp/bt_pair_quick.log 2>/dev/null | tail -10

kill $DASHBOARD_PID 2>/dev/null

echo ""
echo "=== Test Complete ==="
```

## Expected Output Logs

### Successful Pairing
```
[2026-04-02 12:34:56] Quick Pairing Started
[2026-04-02 12:34:56] Found new device: AA:BB:CC:DD:EE:FF (Sony Headphones)
[2026-04-02 12:35:10] Pairing successful!
[2026-04-02 12:35:11] Pairing SUCCESS
```

**Result file:**
```
PAIRED: AA:BB:CC:DD:EE:FF - Sony Headphones
```

### Failed Pairing
```
[2026-04-02 12:34:56] Quick Pairing Started
[2026-04-02 12:34:56] No new devices found
```

**Result file:**
```
FAILED: Could not find or pair device
```

## Troubleshooting

### Issue: "No new devices found" in quick pairing script
Your device was previously paired - this is now fixed! The updated `bt_pair_quick.sh` now:
- Look for devices that were previously paired but forgotten
- Automatically discovers both new AND old devices
- Makes the Pi discoverable so your phone can find it

**Solution:**
```bash
# Just run the updated script again
/home/pi5/dashboard/bt_pair_quick.sh
```

### Issue: Pairing fails at PIN/code entry step
**This happens when:**
- Phone shows a code but script doesn't accept it
- Pi shows a code and phone doesn't prompt for it

**Solutions:**

**Option 1: Let the phone initiate (RECOMMENDED)**
```bash
# 1. Make Pi discoverable
bluetoothctl discoverable on

# 2. On your phone: Search for Bluetooth devices
# 3. Find "raspberrypi" and tap to pair
# 4. Enter/confirm the PIN on your phone when prompted

# 5. On Pi, check if pairing succeeded:
bluetoothctl paired-devices
```

**Option 2: Use interactive pairing script**
```bash
# This gives better control over the PIN handling
/home/pi5/dashboard/bt_pair_interactive.sh AA:BB:CC:DD:EE:FF

# When PIN prompt appears on phone, enter it there
# Script will wait up to 60 seconds for phone confirmation
```

**Option 3: Manual pairing with bluetoothctl interactive mode**
```bash
# Start interactive bluetoothctl
bluetoothctl

# In the bluetoothctl prompt:
agent on
default-agent
power on
discoverable on
scan on
# Wait for your device to appear
pair AA:BB:CC:DD:EE:FF
# When PIN appears on phone, enter it there
trust AA:BB:CC:DD:EE:FF
connect AA:BB:CC:DD:EE:FF
exit
```

### Issue: "Device forgotten" or "Device not found"
When the device was previously paired but forgotten on the Pi:

```bash
# Step 1: Forget the old device from Pi side
# Check if still in blocked list
bluetoothctl blocked-devices

# Unblock if necessary
bluetoothctl unblock AA:BB:CC:DD:EE:FF

# Step 2: Unpair to clear old pairing info
bluetoothctl remove AA:BB:CC:DD:EE:FF

# Step 3: Now try pairing fresh
/home/pi5/dashboard/bt_pair_quick.sh
```

### Issue: bt_pair_quick.sh doesn't find any devices
```bash
# Check 1: Bluetooth daemon is running
sudo systemctl status bluetooth

# Restart if needed
sudo systemctl restart bluetooth

# Check 2: Verify bluetoothctl works
bluetoothctl show

# Check 3: Try manual scan
bluetoothctl scan on
# (Should show devices - check your phone is discoverable)

## Debug Commands

Show all connected Bluetooth devices:
```bash
bluetoothctl devices
```

Show paired devices:
```bash
bluetoothctl paired-devices
```

View Bluetooth daemon status:
```bash
sudo systemctl status bluetooth
```

View daemon logs:
```bash
sudo journalctl -u bluetooth -f
```

Real-time monitoring of pairing:
```bash
# Terminal 1: Run dashboard
/home/pi5/dashboard/lv_port_pc_vscode/bin/main

# Terminal 2: Monitor logs
tail -f /tmp/bt_pair_quick.log /tmp/bt_pair_result.txt
```

## File Locations

| File | Purpose | New/Updated |
|------|---------|------------|
| `/home/pi5/dashboard/bt_pair_device.sh` | Full-featured pairing utility | - |
| `/home/pi5/dashboard/bt_pair_quick.sh` | Quick pairing for UI button | ✅ UPDATED - Now handles previously paired devices |
| `/home/pi5/dashboard/bt_pair_interactive.sh` | Interactive pairing with PIN handling | ✅ NEW |
| `/home/pi5/dashboard/lv_port_pc_vscode/src/bt_pairing.h` | C library header | - |
| `/home/pi5/dashboard/lv_port_pc_vscode/bin/main` | Dashboard executable | - |
| `/tmp/bt_pair_quick.log` | Quick script operation log | - |
| `/tmp/bt_pair_adv.log` | Interactive script operation log | ✅ NEW |
| `/tmp/bt_pair_result.txt` | Pairing result status | - |

## Success Criteria

✅ **Test Passing:**
- Pair button appears in the UI
- Clicking it shows "Pairing..." message
- After 15-20 seconds, device name appears
- Dashboard doesn't freeze or crash
- Logs show successful pairing

❌ **Test Failing:**
- Button doesn't appear in UI
- Dashboard crashes when button is clicked
- Pairing shows "Failed" message
- Timeout or stuck on "Pairing..."
- Permission errors in logs
