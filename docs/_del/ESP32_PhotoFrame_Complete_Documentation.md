# ESP32-S3 PhotoFrame - Complete System Flow Documentation

## Telegram Settings Reference

| Setting | Default | NVS Key | Function | When Active | Impact |
|---------|---------|---------|----------|-------------|--------|
| **check_telegram_on_timer_wakeup** | `true` | `tg_check_timer` | Check for Telegram updates during auto-rotation | TIMER_WAKEUP only | WiFi ON, battery drain ++, enables polling |
| **notify_on_timer_wakeup** | `true` | `tg_notify_timer` | Send "Wake Up" notification with battery status | TIMER_WAKEUP only | WiFi ON, sends message, battery drain + |
| **notify_on_display_update** | `true` | `tg_notify_display` | Send thumbnail after display update | All display updates | WiFi ON, uploads photo, battery drain ++ |
| **notify_on_sleep** | `false` | `tg_notify_sleep` | Send config before entering deep sleep | Before deep sleep | WiFi ON if needed, sends message, battery drain + |

## WiFi Initialization Paths

```
Priority Order:
1. NVS (Permanently saved credentials)
   ├─ Load: nvs_get_str("wifi_ssid"), nvs_get_str("wifi_password")
   ├─ Connect: wifi_manager_connect(ssid, password)
   └─ Result: WiFi ON (if successful)

2. SD Card (/sdcard/wifi.txt)
   ├─ Format: Line 1 = SSID, Line 2 = Password
   ├─ Connect: wifi_manager_connect()
   ├─ On Success:
   │  ├─ Save to NVS: wifi_manager_save_credentials()
   │  └─ Rename: wifi.txt → wifi.bak
   └─ Result: WiFi ON + credentials saved

3. AP Provisioning Mode
   ├─ No credentials found
   ├─ Start AP: SSID="PhotoFrame", Password="photoframe"
   ├─ Web Interface: http://192.168.4.1
   ├─ Wait for user configuration
   └─ Result: WiFi ON (AP mode) until configured
```

## Wakeup Source Matrix

| Wakeup Source | USB Connected | Battery Mode | WiFi Initialized | Telegram Check | Display Update | Sleep After |
|---------------|---------------|--------------|------------------|----------------|----------------|-------------|
| **TIMER_WAKEUP** | ✗ | ✓ (Deep Sleep) | Only if `check_telegram_on_timer_wakeup=true` | Yes (if enabled) | After WiFi/polling | Yes (Timer) |
| **TIMER_WAKEUP** | ✓ | ✗ | ✗ (USB powered) | ✗ | Immediate | ✗ (Stay awake) |
| **KEY_BUTTON** | ✗ | ✓ | ✗ (Never) | ✗ | Immediate | Yes (Timer) |
| **KEY_BUTTON** | ✓ | ✗ | ✗ | ✗ | Immediate | ✗ (Stay awake) |
| **BOOT_BUTTON** | ✗ | ✓ | ✓ (Always) | ✓ | After init | Yes (120s timeout) |
| **BOOT_BUTTON** | ✓ | ✗ | ✓ (Always) | ✓ | After init | ✗ (Stay awake) |
| **POWER_ON** | Any | Any | ✓ (Always) | ✓ | After init | Depends on USB |

## Battery Low Mode Behavior

### Activation Conditions
```c
if (battery_percent <= 20 && !is_charging && !usb_connected) {
    // ACTIVATE LOW BATTERY MODE
    saved_rotate_interval = current_interval;
    display_manager_set_rotate_interval(21600);  // 6 hours
    battery_low_mode_active = true;
}
```

### Deactivation Conditions
```c
if (battery_percent >= 80 || is_charging || usb_connected || battery_percent == -1) {
    // DEACTIVATE LOW BATTERY MODE
    display_manager_set_rotate_interval(saved_rotate_interval);
    battery_low_mode_active = false;
}
```

### Impact
- **Normal Mode**: rotate_interval = 300s (5 min) - configurable
- **Low Battery Mode**: rotate_interval = 21600s (6 hours) - fixed
- **Battery drain reduction**: ~72x fewer wakeups per day

## Telegram Notification Scenarios

### Scenario 1: TIMER_WAKEUP + All Notifications ON + No Updates
```
1. check_telegram_on_timer_wakeup = true  → WiFi ON
2. telegram_bot_check_updates()           → No updates
3. notify_on_timer_wakeup = true          → Send "Wake Up" + battery
4. notify_on_display_update = true        → Send thumbnail after display
5. WiFi OFF
6. Display update
7. notify_on_sleep = true                 → WiFi ON, send "Going to Sleep"
8. WiFi OFF
9. Deep Sleep
```

### Scenario 2: TIMER_WAKEUP + All Notifications ON + Updates Found
```
1. check_telegram_on_timer_wakeup = true  → WiFi ON
2. telegram_bot_check_updates()           → 3 updates found
3. POLLING MODE (60s timeout)
   ├─ Process commands (/display, /config, photos)
   ├─ display_was_updated_by_telegram = true (if display command)
   └─ Send "Polling Complete" notification
4. notify_on_display_update = true
   ├─ IF display NOT updated during polling
   └─ Send thumbnail
5. WiFi OFF
6. Display update (if not done during polling)
7. notify_on_sleep = true                 → WiFi ON, send sleep notification
8. WiFi OFF
9. Deep Sleep
```

### Scenario 3: TIMER_WAKEUP + All Notifications OFF
```
1. check_telegram_on_timer_wakeup = false → WiFi OFF (never initialized)
2. Display update immediately
3. Deep Sleep immediately
```

### Scenario 4: KEY_BUTTON (Manual Rotation)
```
1. WiFi = OFF (never initialized)
2. All Telegram settings = ignored
3. Display update immediately
4. Deep Sleep immediately
```

### Scenario 5: BOOT_BUTTON + USB Connected
```
1. WiFi ON (always)
2. HTTP Server ON
3. Telegram Polling ON (background task)
4. notify_on_timer_wakeup = true → Send "Wake Up" (BOOT wakeup)
5. Stay awake indefinitely
6. No sleep (USB powered)
```

## WiFi State Transitions

```
State Machine:

[OFF] ──(check_telegram OR BOOT/POWER_ON)──→ [INITIALIZING]
         │
         ├─(NVS credentials)──→ [CONNECTING]
         ├─(SD credentials)───→ [CONNECTING]
         └─(No credentials)───→ [AP_MODE]
                                      │
[CONNECTING] ─(Success)──→ [CONNECTED]
             └(Failed)───→ [OFF]
                                      │
[CONNECTED] ─(Telegram done)──→ [DISCONNECTING] ──→ [OFF]
            └(Stay awake)─────→ [CONNECTED] (loop)

[AP_MODE] ─(User config)──→ [CONNECTING]
          └(Never config)──→ [AP_MODE] (loop)
```

## Image History Management

### Storage
- **Location**: NVS Flash
- **Keys**: 
  - `img_hist_cnt` (int32_t) - Number of displayed images
  - `img_history` (blob) - MD5 hashes array
- **Format**: Array of 16-byte MD5 hashes
- **Capacity**: 1000 images (16 KB NVS)

### Algorithm
```c
1. Get all images from enabled albums
2. For each image:
   - Calculate MD5(image_path)
   - Check if hash exists in displayed_image_hashes[]
   - If NOT found → Add to undisplayed_list[]
3. If undisplayed_count == 0:
   - Send Telegram notification (if configured)
   - Clear history (free memory + erase NVS)
   - Use all images again
4. Select random from undisplayed_list[]
5. Display image
6. Add MD5 hash to history
7. Save to NVS every 10 images (reduce flash wear)
```

### Reset Conditions
- All images displayed (automatic)
- Manual command: `/clearhistory` (Telegram)
- API call: `display_manager_clear_history()` (Web interface)

## Power Consumption Estimates

| Mode | WiFi | Display Updates/Day | Est. Battery Life* |
|------|------|---------------------|-------------------|
| **USB Powered** | Always ON | Continuous | Unlimited |
| **Battery + Telegram OFF** | OFF | 288 (5min) | ~7 days |
| **Battery + Telegram ON (no updates)** | ON briefly | 288 (5min) | ~5 days |
| **Battery + Telegram ON (polling)** | ON extended | 288 (5min) | ~3 days |
| **Battery Low Mode** | OFF/Brief | 4 (6h) | ~30 days |

*Estimated with 5000mAh battery, actual varies by usage

## Command Reference - Telegram Bot

### Display Commands
- `/display <album/image.bmp>` - Show specific image
- `/txt <text>` - Show random image with text overlay
- Send photo with caption "show" - Display immediately

### Album Management
- `/albums` - List all albums
- `/images <album>` - List images in album
- `/createalbum <name>` - Create new album
- `/deletealbum <name>` - Delete album
- `/enablealbum <name>` - Enable album
- `/disablealbum <name>` - Disable album

### Configuration
- `/config` - Show current settings
- `/setinterval <seconds>` - Set rotation interval (10-86400)
- `/autorotate <on/off>` - Enable/disable auto-rotation
- `/brightness <-2.0 to 2.0>` - Adjust brightness (f-stops)
- `/contrast <0.5 to 2.0>` - Adjust contrast

### Telegram Settings
- `/telegramsettings` - Show all Telegram settings
- `/telegramchecktimer <on/off>` - Check updates on auto-rotate
- `/telegramnotifytimer <on/off>` - Notify on auto-rotate
- `/telegramnotifysleep <on/off>` - Notify before sleep
- `/telegramnotifydisplay <on/off>` - Send thumbnail on display update

### Image Features
- `/combine` - Show portrait combine status
- `/combine <on/off>` - Enable/disable portrait combine
- `/history` - View image history count
- `/clearhistory` - Reset displayed images

### System
- `/status` - System status, IPs, memory
- `/battery` - Battery status
- `/sleep` - Enter sleep mode immediately

## API Endpoints - HTTP Server

### Display
- `POST /api/display` - Display specific image
- `POST /api/display-image` - Upload and display JPG directly
- `GET /api/image?name=<path>` - Serve image file

### Albums
- `GET /api/albums` - List all albums
- `POST /api/albums` - Create album
- `DELETE /api/albums?name=<name>` - Delete album
- `PUT /api/albums/enabled?name=<name>` - Enable/disable album
- `GET /api/images?album=<name>` - List images in album

### Configuration
- `GET /api/config` - Get configuration
- `POST /api/config` - Update configuration
- `GET /api/battery` - Battery status
- `POST /api/sleep` - Enter sleep mode

### Telegram
- `GET /api/telegram/config` - Get Telegram settings
- `POST /api/telegram/config` - Set Telegram token
- `GET /api/telegram/chatid` - Get configured chat ID
- `POST /api/telegram/chatid` - Set chat ID

### Image Processing
- `GET /api/settings/processing` - Get processing settings
- `POST /api/settings/processing` - Update processing settings
- `GET /api/settings/palette` - Get color palette
- `POST /api/settings/palette` - Update color palette
- `DELETE /api/settings/palette` - Reset to defaults

### Advanced
- `GET /api/portrait-combine` - Get portrait combine status
- `POST /api/portrait-combine` - Enable/disable portrait combine
- `POST /api/calibration/display` - Display calibration pattern
- `GET /api/version` - Firmware version info

## File Structure

```
/sdcard/
├── images/                          # Default album
│   ├── image1.bmp                   # Display file (800x480, 6-color)
│   ├── image1.jpg                   # Thumbnail (for Telegram)
│   ├── image2.bmp
│   └── image2.jpg
├── portrait/                        # Portrait images (auto-created)
│   ├── portrait1.bmp
│   ├── portrait2.bmp
│   └── combined_12.bmp              # Auto-combined portrait pair
├── Album1/                          # User-created album
│   └── ...
├── Album2/
│   └── ...
├── wifi.txt                         # WiFi credentials (optional)
│   # Line 1: SSID
│   # Line 2: Password
└── wifi.bak                         # Backup after successful connection

NVS Flash Storage:
├── photoframe/                      # Namespace
│   ├── wifi_ssid                    # WiFi SSID
│   ├── wifi_password                # WiFi password
│   ├── rotate_interval              # Rotation interval (seconds)
│   ├── auto_rotate                  # Auto-rotate enabled (bool)
│   ├── deep_sleep                   # Deep sleep enabled (bool)
│   ├── tg_token                     # Telegram bot token
│   ├── tg_chat_id                   # Telegram chat ID
│   ├── tg_check_timer               # Check Telegram on timer wakeup
│   ├── tg_notify_timer              # Notify on timer wakeup
│   ├── tg_notify_sleep              # Notify before sleep
│   ├── tg_notify_display            # Notify on display update
│   ├── bat_low_mode                 # Battery low mode active
│   ├── bat_saved_interval           # Saved interval before low mode
│   ├── img_hist_cnt                 # Image history count
│   └── img_history                  # Image history MD5 hashes (blob)
```

## Troubleshooting

### WiFi won't connect
1. Check credentials in NVS: Web interface → Settings
2. Try SD card method: Create `/sdcard/wifi.txt` with SSID and password
3. Factory reset WiFi: Hold BOOT button for 10s while awake
4. Use AP mode: Connect to "PhotoFrame" network, configure via 192.168.4.1

### Telegram not working
1. Verify bot token: Web interface → Telegram Settings
2. Set chat ID: Send `/mychatid` to bot, copy ID to web interface
3. Check settings: `/telegramsettings` via Telegram
4. Enable check on timer: `/telegramchecktimer on`

### Battery drains too fast
1. Disable Telegram check on timer: `/telegramchecktimer off`
2. Disable display notifications: `/telegramnotifydisplay off`
3. Increase rotation interval: `/setinterval 3600` (1 hour)
4. Battery low mode activates automatically at ≤20%

### Images repeat
1. Check history count: `/history`
2. Clear history: `/clearhistory`
3. History auto-resets when all images shown

### Display not updating
1. Check auto-rotate: `/config` → should show "Auto-Rotate: ON"
2. Enable auto-rotate: `/autorotate on`
3. Check interval: `/config` → interval should be reasonable (300-3600s)
4. Manual trigger: Press KEY button or send `/display <image>`

