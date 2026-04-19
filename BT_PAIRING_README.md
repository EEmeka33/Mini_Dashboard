# Bluetooth Device Pairing Setup

## Scripts Created

### 1. bt_pair_device.sh (Full-featured)
Comprehensive Bluetooth pairing utility with multiple actions.

**Location:** `/home/pi5/dashboard/bt_pair_device.sh`

**Usage:**
```bash
# Scan for available devices
./bt_pair_device.sh scan

# Pair with a specific device
./bt_pair_device.sh pair XX:XX:XX:XX:XX:XX

# List all paired devices
./bt_pair_device.sh list

# Connect to a device
./bt_pair_device.sh connect XX:XX:XX:XX:XX:XX

# Interactive auto-pairing (recommended for UI)
./bt_pair_device.sh auto
```

**Log:** `/tmp/bt_pair.log`

### 2. bt_pair_quick.sh (Quick pairing for UI button)
Streamlined script optimized for calling from the dashboard UI.

**Location:** `/home/pi5/dashboard/bt_pair_quick.sh`

**Usage:**
```bash
# Scan and pair with first new device found
./bt_pair_quick.sh
```

**Output Files:**
- `/tmp/bt_pair_quick.log` - Detailed log with timestamps
- `/tmp/bt_pair_result.txt` - Result status (PAIRED: or FAILED:)

**Result Format:**
```
PAIRED: AA:BB:CC:DD:EE:FF - Device Name
```

### 3. bt_pairing.h (C Library)
C header file with inline functions for calling the pairing script from the dashboard UI.

**Location:** `/home/pi5/dashboard/lv_port_pc_vscode_no_thread/src/bt_pairing.h`

**Functions:**

#### `bluetooth_pair_device_start()`
Spawns background pairing process (non-blocking).
```c
int result = bluetooth_pair_device_start();
if (result == 0) {
    printf("Pairing started in background\n");
}
```

#### `bluetooth_pair_device_check_result(char *device_name, size_t max_len)`
Check if pairing completed.
```c
char device_name[128];
int status = bluetooth_pair_device_check_result(device_name, sizeof(device_name));

if (status == 1) {
    printf("Success! Paired with: %s\n", device_name);
} else if (status == 0) {
    printf("Pairing failed\n");
} else {
    printf("Still in progress...\n");
}
```

#### `bluetooth_show_pairing_status()`
Show pairing status (call periodically in UI timer).
```c
bluetooth_show_pairing_status();
```

## Integration Example

To add the "Pair Device" button to main.c:

### 1. Add include at top:
```c
#include "bt_pairing.h"
```

### 2. Add button callback:
```c
static void on_btn_pair_device_clicked(lv_event_t *e)
{
    fprintf(stderr, "[UI] Starting Bluetooth pairing...\n");
    
    if (bluetooth_pair_device_start() == 0) {
        fprintf(stderr, "[UI] Pairing task spawned in background\n");
        lv_label_set_text(g_music_artist_label, "Pairing...");
    } else {
        fprintf(stderr, "[UI] Failed to start pairing\n");
    }
}
```

### 3. In create_ui() function (after music controls):
```c
/* Create Pair Device button */
lv_obj_t *btn_pair = lv_button_create(music_container);
lv_obj_add_event_cb(btn_pair, on_btn_pair_device_clicked, LV_EVENT_CLICKED, NULL);
lv_obj_set_style_bg_color(btn_pair, lv_color_hex(0x00a8e8), 0);
lv_obj_set_size(btn_pair, 60, 60);
lv_obj_align(btn_pair, LV_ALIGN_BOTTOM_RIGHT, -20, -20);

lv_obj_t *label_pair = lv_label_create(btn_pair);
lv_label_set_text(label_pair, "Pair");
lv_obj_center(label_pair);
```

### 4. In ui_update_timer() function (to show pairing progress):
```c
static void ui_update_timer(lv_timer_t *timer)
{
    update_nav_display();
    update_music_display();
    update_call_display();
    speedo_set_speed((int32_t)g_state.nav.speed);
    
    /* Check for completed pairing */
    char device_name[128];
    int pairing_status = bluetooth_pair_device_check_result(device_name, sizeof(device_name));
    if (pairing_status == 1) {
        fprintf(stderr, "[UI] Pairing completed: %s\n", device_name);
        lv_label_set_text(g_music_artist_label, device_name);
    }
}
```

## Testing Manual Pairing

To test the scripts manually:

```bash
# 1. Full-featured scan
/home/pi5/dashboard/bt_pair_device.sh scan

# 2. View results
cat /tmp/bt_pair.log

# 3. Pair with specific device (requires address from scan)
/home/pi5/dashboard/bt_pair_device.sh pair AA:BB:CC:DD:EE:FF

# 4. Check result
cat /tmp/bt_pair_result.txt
```

## Troubleshooting

**"bluetoothctl not found"**
```bash
sudo apt install bluez bluez-tools
```

**Permission denied when pairing**
```bash
sudo usermod -aG bluetooth pi5
# Then log out and log back in
```

**Device won't stay connected**
- Check device battery
- Run: `sudo systemctl restart bluetooth`
- Try: `/home/pi5/dashboard/bt_pair_device.sh trust <ADDRESS>`

**Logs**
- Main pairing log: `/tmp/bt_pair_quick.log`
- Full script log: `/tmp/bt_pair.log`

## Files Summary

| File | Purpose | User |
|------|---------|------|
| bt_pair_device.sh | Full-featured pairing utility | Administrator |
| bt_pair_quick.sh | Quick auto-pairing for UI button | UI Button Handler |
| bt_pairing.h | C library for UI integration | Dashboard UI |
