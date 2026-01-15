#ifndef CONFIG_H
#define CONFIG_H

#include <driver/gpio.h>
// Debug-Build vs. Release-Build
/*
#ifdef DEBUG_BUILD
    #define LOG_LEVEL ESP_LOG_DEBUG
#else
    #define LOG_LEVEL ESP_LOG_NONE  // Productive: No Logging
#endif
*/
// Compile:
// # Debug-Build (with Logging)
// idf.py -D DEBUG_BUILD=1 build flash
//
// # Release-Build (no Logging)
// idf.py build flash

// Uncomment to debug deep sleep wake
// #define DEBUG_DEEP_SLEEP_WAKE

typedef enum { ROTATION_MODE_SDCARD = 1, ROTATION_MODE_URL = 0 } rotation_mode_t;

#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define PWR_BUTTON_GPIO GPIO_NUM_5
#define KEY_BUTTON_GPIO GPIO_NUM_4

#define LED_RED_GPIO GPIO_NUM_45
#define LED_GREEN_GPIO GPIO_NUM_42

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 64
#define IMAGE_URL_MAX_LEN 256
#define ROTATION_MODE_MAX_LEN 16

#define DEFAULT_WIFI_SSID "PhotoFrame"
#define DEFAULT_WIFI_PASSWORD "photoframe123"
#define DEFAULT_IMAGE_URL "https://loremflickr.com/800/480"

#define SDCARD_MOUNT_POINT "/sdcard"
#define IMAGE_DIRECTORY "/sdcard/images"
#define DEFAULT_ALBUM_NAME "Default"

#define DISPLAY_WIDTH 800
#define DISPLAY_HEIGHT 480

#ifdef DEBUG_DEEP_SLEEP_WAKE
#define AUTO_SLEEP_TIMEOUT_SEC 20
#else
#define AUTO_SLEEP_TIMEOUT_SEC 120
#endif

#define IMAGE_ROTATE_INTERVAL_SEC 3600

#define NVS_NAMESPACE "photoframe"
#define NVS_WIFI_SSID_KEY "wifi_ssid"
#define NVS_WIFI_PASS_KEY "wifi_pass"
#define NVS_ROTATE_INTERVAL_KEY "rotate_int"
#define NVS_AUTO_ROTATE_KEY "auto_rotate"
#define NVS_BRIGHTNESS_KEY "brightness"
#define NVS_CONTRAST_KEY "contrast"
#define NVS_DEEP_SLEEP_KEY "deep_sleep"
#define NVS_ENABLED_ALBUMS_KEY "enabled_albums"
#define NVS_IMAGE_URL_KEY "image_url"
#define NVS_ROTATION_MODE_KEY "rotation_mode"
#define NVS_SAVE_DOWNLOADED_KEY "save_dl"

// v1.9.0_tlg Telegram START

// Telegram Buffer Sizes
#define TELEGRAM_MAX_LINE_MESSAGE_LENGTH 128
#define TELEGRAM_MAX_ALBUM_PATH_LENGTH 256  // 128
#define TELEGRAM_MAX_CAPTION_LENGTH 256
#define TELEGRAM_MAX_FILENAME_LENGTH 512  // base name // file_path
#define TELEGRAM_MAX_PATH_LENGTH 1024     // 512
#define TELEGRAM_MAX_URL_LENGTH 2048      // 512
#define TELEGRAM_MAX_MESSAGE_LENGTH_XS 256
#define TELEGRAM_MAX_MESSAGE_LENGTH_S 512
#define TELEGRAM_MAX_MESSAGE_LENGTH_M 1024
#define TELEGRAM_MAX_MESSAGE_LENGTH_L 2048
#define TELEGRAM_HTTP_TIMEOUT_MS 30000
#define TELEGRAM_SHORT_TIMEOUT_MS 10000
// #define TELEGRAM_MAX_THUMBNAIL_SIZE     (512 * 1024)  // 512 KB
#define TELEGRAM_MAX_DOWN_JPG_SIZE 2048  // 4096 → 2048

/*
telegram_check_task: 32768 Bytes (32 KB) - main.c
sleep_timer_task: 4096 Bytes (4 KB) - power_manager.c
rotation_timer_task: 4096 Bytes (4 KB) - power_manager.c
send_photo_task: 32768 Bytes (32 KB) - telegram_bot.c
*/

// Telegram polling interval in milliseconds
#define TELEGRAM_POLL_INTERVAL_MS 10000

// Telegram configuration keys
#define NVS_TELEGRAM_CHECK_ON_TIMER_KEY "tg_check_timer"    // Check Telegram on timer wakeup
#define NVS_TELEGRAM_NOTIFY_ON_TIMER_KEY "tg_notify_timer"  // Send notification on timer wakeup

#define NVS_TELEGRAM_CHAT_ID_KEY "tg_chat_id"
// v1.9.0_tlg Telegram END

// v1.9.0_tlg Battery Warning System START
#define BATTERY_CRITICAL_THRESHOLD 10
#define BATTERY_LOW_THRESHOLD 20
#define BATTERY_RECOVER_THRESHOLD 80
#define BATTERY_LOW_MODE_INTERVAL_SEC 21600  // 6 hours when battery < 20%
// NVS Keys
#define NVS_BAT_SAVED_INTERVAL_KEY "bat_saved_int"
#define NVS_BAT_LOW_MODE_KEY "bat_low_mode"
// Optional: Uncomment and set GPIO if you have a buzzer
// #define BUZZER_GPIO GPIO_NUM_21
// v1.9.0_tlg Battery Warning System END

// ============================================================================
// WIFI SD-CARD CREDENTIALS on SD card
// ============================================================================

// WiFi credentials file on SD card
#define WIFI_CREDENTIALS_FILE "/sdcard/wifi.txt"
#define WIFI_CREDENTIALS_BACKUP "/sdcard/wifi.bak"

/// sdcard/wifi.txt:
/*
My SSiD name
My_Pw123
*/

// Enable BOOT button to reload WiFi credentials from SD card (even if NVS has credentials)
// WARNING: This allows WiFi reset without flash erase!
// Set to 1 to enable, 0 to disable (default)
#define ENABLE_WIFI_BOOT_BUTTON_RELOAD 0

#endif
