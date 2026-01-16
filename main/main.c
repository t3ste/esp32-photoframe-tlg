#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "album_manager.h"
#include "axp_prot.h"
#include "color_palette.h"
#include "config.h"
#include "config_manager.h"
#include "display_manager.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_server.h"
#include "i2c_bsp.h"
#include "image_processor.h"
#include "mdns_service.h"
#include "nvs_flash.h"
#include "power_manager.h"
#include "processing_settings.h"
#include "sdmmc_cmd.h"
#include "telegram_bot.h"
#include "utils.h"
#include "wifi_manager.h"
#include "wifi_provisioning.h"

// BAT CHECK
#include "battery_manager.h"
// #include "audio_manager.h"

// idf.py fullclean
// idf.py set-target esp32s3
// idf.py build
// idf.py -p PORT erase-flash 
// idf.py -p PORT flash
// opt: idf.py -p PORT flash monitor
// opt: idf.py -p PORT flash monitor --no-reset

static const char *TAG = "main";

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
static esp_err_t init_sd_card(void);
static void telegram_check_task(void *arg);
static void button_task(void *arg);
// void test_timer_wakeup_flow(void);

// ============================================================================
// TELEGRAM STATE (v1.9.0_tlg)
// ============================================================================
static TaskHandle_t telegram_task_handle = NULL;
bool telegram_polling_active = false;
static bool display_was_updated_by_telegram = false;  // Flag for Telegram display updates
static bool pending_display_update = false;           // Flag for delayed display update

// Helper function to connect to WiFi with timeout
// If suppress_reconnect is true, wifi_manager_stop() is called on timeout instead of leaving WiFi
// active
static bool connect_to_wifi_with_timeout(int timeout_seconds, bool suppress_reconnect)
{
    char wifi_ssid[WIFI_SSID_MAX_LEN] = {0};
    char wifi_password[WIFI_PASS_MAX_LEN] = {0};
    ESP_ERROR_CHECK(wifi_manager_load_credentials(wifi_ssid, wifi_password));
    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", wifi_ssid);
    wifi_manager_connect(wifi_ssid, wifi_password);

    // Wait for WiFi connection (with timeout)
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    int retry_count = 0;
    while (!wifi_manager_is_connected() && retry_count < timeout_seconds) {
        if (retry_count % 10 == 0 && retry_count > 0) {
            ESP_LOGI(TAG, "WiFi connecting... (%d seconds elapsed)", retry_count);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        retry_count++;
    }

    if (wifi_manager_is_connected()) {
        ESP_LOGI(TAG, "WiFi connected after %d seconds", retry_count);
        return true;
    } else {
        ESP_LOGE(TAG, "WiFi connection timeout after %d seconds", timeout_seconds);

        // Stop WiFi completely to prevent reconnect attempts
        if (suppress_reconnect) {
            ESP_LOGI(TAG, "Stopping WiFi to prevent reconnect attempts...");
            wifi_manager_stop();
            telegram_bot_reset_display_flag();  // Reset on WiFi disconnect
        }

        return false;
    }
}

// ============================================================================
// SD CARD INITIALIZATION
// ============================================================================
static esp_err_t init_sd_card(void)
{
    ESP_LOGI(TAG, "Initializing SD card");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, .max_files = 5, .allocation_unit_size = 16 * 1024};

    sdmmc_card_t *card;
    const char *mount_point = SDCARD_MOUNT_POINT;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    // Configure SD card pins for ESP32-S3 PhotoPainter
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = GPIO_NUM_39;  // SDMMC_CLK
    slot_config.cmd = GPIO_NUM_41;  // SDMMC_CMD
    slot_config.d0 = GPIO_NUM_40;   // SDMMC_D0
    slot_config.d1 = GPIO_NUM_1;    // SDMMC_D1
    slot_config.d2 = GPIO_NUM_2;    // SDMMC_D2
    slot_config.d3 = GPIO_NUM_38;   // SDMMC_D3

    // Retry SD card initialization up to 3 times with delays
    esp_err_t ret = ESP_FAIL;
    const int retries = 3;

    for (int retry = 0; retry < retries; retry++) {
        if (retry > 0) {
            ESP_LOGW(TAG, "SD card init failed, retrying... (attempt %d/%d)", retry + 1, retries);
            vTaskDelay(pdMS_TO_TICKS(500));  // Wait 500ms before retry
        }

        ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
        if (ret == ESP_OK) {
            break;  // Success
        }
    }

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem after %d attempts", retries);
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card after %d attempts: (%s)", retries,
                     esp_err_to_name(ret));
        }
        return ret;
    }

    sdmmc_card_print_info(stdout, card);

    // Poll sdcard status until it's ready
    ESP_LOGI(TAG, "Waiting for SD card to be ready...");
    while (sdmmc_get_status(card) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "SD card ready");

    // Create image directory if it doesn't exist
    struct stat st;
    if (stat(IMAGE_DIRECTORY, &st) != 0) {
        ESP_LOGI(TAG, "Creating image directory: %s", IMAGE_DIRECTORY);
        mkdir(IMAGE_DIRECTORY, 0775);
    }

    ESP_LOGI(TAG, "SD card initialized successfully");
    return ESP_OK;
}

// ============================================================================
// TELEGRAM CHECK TASK (v1.9.0_tlg) - 60s Timeout und Display-Update NACH Polling
// ============================================================================
// Load Telegram settings
// bool check_telegram = telegram_bot_get_check_on_timer_wakeup();

static void telegram_check_task(void *arg)
{
    // Wait for system ready
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "Starting Telegram update check...");

    // Reset flags
    display_was_updated_by_telegram = false;
    pending_display_update = false;

    // Send battery notification if < 20% on wakeup
    if (telegram_bot_get_notify_on_timer_wakeup()) {
        int battery_percent = axp_get_battery_percent();
        bool is_charging = axp_is_charging();
        bool usb_connected = axp_is_usb_connected();

        if (battery_percent < 0 && axp_is_usb_connected()) {
            ESP_LOGI(TAG, "USB powered, no battery detected - skipping battery warning");
        }

        // Only warn if: Battery present AND low AND not charging AND no USB
        if (battery_percent >= 0 &&  // battery connected
            battery_percent <= BATTERY_LOW_THRESHOLD && !is_charging &&
            !usb_connected) {  // no USB

            int64_t chat_id = telegram_bot_get_last_chat_id();
            if (chat_id != 0) {
                ESP_LOGW(TAG, "⚠️ Battery low on wakeup (%d%%), sending notification...",
                         battery_percent);
                char message[256];
                snprintf(message, sizeof(message),
                         "⚠️ <b>Low Battery Alert</b>\n"
                         "Battery: %d%%\n"
                         "Status: Not charging\n"
                         "Please connect charger soon.",
                         battery_percent);
                telegram_bot_send_message(chat_id, message);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }

    UBaseType_t stack_high_water = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "Initial stack free: %u bytes", stack_high_water * 4);

    bool had_updates = telegram_bot_check_updates();

    stack_high_water = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "Stack free after check: %u bytes", stack_high_water * 4);

    if (had_updates) {
        ESP_LOGI(TAG, "Updates detected, entering polling mode");
        telegram_polling_active = true;
        power_manager_reset_sleep_timer();

        // Polling timeout: 60 seconds (reset on updates)
        int64_t last_update_time = esp_timer_get_time();
        const int64_t polling_timeout_us = 60 * 1000000LL;  // 60 seconds

        uint32_t poll_interval_ms = TELEGRAM_POLL_INTERVAL_MS;

        // Tracking for notification
        int update_count = 0;
        int64_t polling_start_time = esp_timer_get_time();

        while (telegram_polling_active) {
            // Check if we should stop
            if (!telegram_polling_active) {
                ESP_LOGI(TAG, "Polling stopped by external signal");
                break;
            }

            // Check if 60s passed since last update
            int64_t now = esp_timer_get_time();
            int64_t time_since_last_update = now - last_update_time;

            if (time_since_last_update >= polling_timeout_us) {
                ESP_LOGI(TAG, "Polling timeout: 60s since last update");
                telegram_polling_active = false;
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));

            bool updates = telegram_bot_check_updates();
            if (updates) {
                update_count++;  // Count updates
                last_update_time = esp_timer_get_time();
                power_manager_reset_sleep_timer();
                ESP_LOGI(TAG, "New updates (#%d) - timers reset", update_count);
            }

            // Monitor stack during polling
            stack_high_water = uxTaskGetStackHighWaterMark(NULL);
            if (stack_high_water < 512) {  // < 2 KB free
                ESP_LOGW(TAG, "Stack running low! Free: %u bytes", stack_high_water * 4);
            }
        }

        ESP_LOGI(TAG, "Telegram polling complete");

        // Send polling-end notification
        int64_t chat_id = telegram_bot_get_last_chat_id();
        if (chat_id != 0 && wifi_manager_is_connected()) {
            int64_t polling_duration_us = esp_timer_get_time() - polling_start_time;
            int polling_duration_sec = (int) (polling_duration_us / 1000000LL);

            char msg[256];
            if (display_was_updated_by_telegram) {
                snprintf(msg, sizeof(msg),
                         "📊 Polling Complete\n"
                         "⏱ Duration: %d seconds\n"
                         "📥 Updates processed: %d\n"
                         "✓ Display already updated",
                         polling_duration_sec, update_count);
            } else {
                snprintf(msg, sizeof(msg),
                         "📊 Polling Complete\n"
                         "⏱ Duration: %d seconds\n"
                         "📥 Updates processed: %d\n"
                         "🖼 Display update starting now...",
                         polling_duration_sec, update_count);
            }

            ESP_LOGI(TAG, "Sending polling complete notification...");
            if (telegram_bot_send_message(chat_id, msg) == ESP_OK) {
                ESP_LOGI(TAG, " Notification sent");
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }

        // Display update AFTER notification
        if (pending_display_update && !display_was_updated_by_telegram) {
            ESP_LOGI(TAG, "Executing postponed display update...");
            display_manager_handle_wakeup();
            pending_display_update = false;

        } else if (display_was_updated_by_telegram) {
            ESP_LOGI(TAG, "Display already updated by Telegram command");
        }

        // Stop Telegram polling BEFORE WiFi is stopped
        if (telegram_task_handle != NULL) {
            ESP_LOGI(TAG, "Stopping Telegram polling task...");
            telegram_polling_active = false;  // Signal task to stop

            // Wait until task is finished
            int timeout = 0;
            while (telegram_task_handle != NULL && timeout < 50) {
                vTaskDelay(pdMS_TO_TICKS(100));
                timeout++;
            }

            if (telegram_task_handle != NULL) {
                ESP_LOGW(TAG, "Force-deleting Telegram task");
                vTaskDelete(telegram_task_handle);
                telegram_task_handle = NULL;
            }
        }

        // WiFi stop?
        /*
if (wifi_manager_is_connected()) {
    ESP_LOGI(TAG, "Disconnecting WiFi...");
    vTaskDelay(pdMS_TO_TICKS(2000)); // Waiting for e.g. Telegram notification to complete
    //wifi_manager_disconnect();
                wifi_manager_stop();
                vTaskDelay(pdMS_TO_TICKS(1000));
}
        */
        ESP_LOGI(TAG, "System will enter deep sleep in %ds", AUTO_SLEEP_TIMEOUT_SEC);
        // ESP_LOGI(TAG, "System will enter deep sleep in 120s");

    } else {
        ESP_LOGI(TAG, "No initial Telegram updates found");
    }

    ESP_LOGI(TAG, "Telegram check complete");
    telegram_task_handle = NULL;
    vTaskDelete(NULL);
}

// ============================================================================
// BUTTON TASK - Handles BOOT and KEY button presses
// ============================================================================

static void button_task(void *arg)
{
    bool last_boot_state = gpio_get_level(BOOT_BUTTON_GPIO);
    bool last_key_state = gpio_get_level(KEY_BUTTON_GPIO);
    bool current_boot_state, current_key_state;
    uint32_t boot_press_time = 0;
    uint32_t key_press_time = 0;

    while (1) {
        current_boot_state = gpio_get_level(BOOT_BUTTON_GPIO);
        current_key_state = gpio_get_level(KEY_BUTTON_GPIO);

#if ENABLE_WIFI_BOOT_BUTTON_RELOAD
        // ═══════════════════════════════════════════════════════════════════
        // WIFI RESET: BOOT Button (2s hold) = Load WiFi from SD Card
        // ═══════════════════════════════════════════════════════════════════
        // Note: This requires ENABLE_WIFI_BOOT_BUTTON_RELOAD = 1 in config.h
        // Only works when system is awake and WiFi is NOT connected
        // Tracking for Button-Combination
        bool boot_held = (current_boot_state == 0);
        bool key_held = (current_key_state == 0);

        if (boot_held && !key_held && !wifi_manager_is_connected() &&
            power_manager_is_awake()) {  // Nur im Wach-Zustand

            // Debounce: Wait 2 seconds, then check if still pressed
            uint32_t hold_start = xTaskGetTickCount();
            bool still_held = true;

            while ((xTaskGetTickCount() - hold_start) * portTICK_PERIOD_MS < 2000) {
                if (gpio_get_level(BOOT_BUTTON_GPIO) != 0) {
                    still_held = false;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            if (still_held) {
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
                ESP_LOGI(TAG, "║ BOOT Button (2s): WiFi Reset from SD Card               ║");
                ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
                ESP_LOGI(TAG, "");

                char wifi_ssid[WIFI_SSID_MAX_LEN] = {0};
                char wifi_pass[WIFI_PASS_MAX_LEN] = {0};

                // Try loading from SD card
                esp_err_t sd_err = wifi_manager_load_credentials_from_sd(wifi_ssid, wifi_pass);

                if (sd_err == ESP_OK) {
                    ESP_LOGI(TAG, "Connecting to WiFi: %s", wifi_ssid);
                    esp_err_t wifi_ret = wifi_manager_connect(wifi_ssid, wifi_pass);

                    if (wifi_ret == ESP_OK) {
                        ESP_LOGI(TAG, " WiFi connected - saving to NVS");
                        wifi_manager_save_credentials(wifi_ssid, wifi_pass);

                        // Rename file to .bak
                        if (rename(WIFI_CREDENTIALS_FILE, WIFI_CREDENTIALS_BACKUP) == 0) {
                            ESP_LOGI(TAG, " Credentials file backed up");
                        } else {
                            ESP_LOGW(TAG, "Failed to rename credentials file");
                        }

                        // Restart mDNS
                        mdns_service_init();

                        ESP_LOGI(TAG, " WiFi reset complete");
                    } else {
                        ESP_LOGE(TAG, "✗ WiFi connection failed");
                    }
                } else {
                    ESP_LOGE(TAG, "✗ No credentials file found on SD card");
                    ESP_LOGI(TAG, "Expected file: %s", WIFI_CREDENTIALS_FILE);
                }

                // Wait until button released
                while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }

                continue;
            }
        }
#endif

        // ═══════════════════════════════════════════════════════════════════
        // BOOT BUTTON: Short press = Reset sleep timer
        // ═══════════════════════════════════════════════════════════════════
        if (current_boot_state == 0 && last_boot_state == 1) {
            boot_press_time = xTaskGetTickCount();
        } else if (current_boot_state == 1 && last_boot_state == 0) {
            uint32_t duration = (xTaskGetTickCount() - boot_press_time) * portTICK_PERIOD_MS;
            if (duration > 50 && duration < 3000) {
                ESP_LOGI(TAG, "Boot button pressed, resetting sleep timer");
                power_manager_reset_sleep_timer();
            }
        }

        // ═══════════════════════════════════════════════════════════════════
        // KEY BUTTON: Short press = Trigger image rotation
        // ═══════════════════════════════════════════════════════════════════
        if (current_key_state == 0 && last_key_state == 1) {
            key_press_time = xTaskGetTickCount();
        } else if (current_key_state == 1 && last_key_state == 0) {
            uint32_t duration = (xTaskGetTickCount() - key_press_time) * portTICK_PERIOD_MS;
            if (duration > 50 && duration < 3000) {
                ESP_LOGI(TAG, "Key button pressed, triggering rotation");
                power_manager_reset_sleep_timer();
                display_manager_handle_wakeup();
            }
        }

        last_boot_state = current_boot_state;
        last_key_state = current_key_state;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ============================================================================
// HELPER FUNCTION: Set display updated flag (called from display_manager.c)
// ============================================================================
void main_set_display_updated_by_telegram(bool updated)
{
    display_was_updated_by_telegram = updated;
}

// ============================================================================
// MAIN APPLICATION ENTRY POINT
// ============================================================================
void app_main(void)
{
    // ═══════════════════════════════════════════════════════════════════════
    // STARTUP: Check reset reason and log system info
    // ═══════════════════════════════════════════════════════════════════════
    esp_log_level_set("*", ESP_LOG_DEBUG); // enable or disable logging

    esp_reset_reason_t reset_reason = esp_reset_reason();
    const char *reset_reason_str;

    switch (reset_reason) {
    case ESP_RST_POWERON:
        reset_reason_str = "Power-on reset";
        break;
    case ESP_RST_SW:
        reset_reason_str = "Software reset";
        break;
    case ESP_RST_PANIC:
        reset_reason_str = "Exception/panic";
        break;
    case ESP_RST_INT_WDT:
        reset_reason_str = "Interrupt watchdog";
        break;
    case ESP_RST_TASK_WDT:
        reset_reason_str = "Task watchdog";
        break;
    case ESP_RST_WDT:
        reset_reason_str = "Other watchdog";
        break;
    case ESP_RST_DEEPSLEEP:
        reset_reason_str = "Deep sleep wake";
        break;
    case ESP_RST_BROWNOUT:
        reset_reason_str = "Brownout reset";
        break;
    default:
        reset_reason_str = "Unknown";
        break;
    }

    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "PhotoFrame starting... (Reset reason: %s)", reset_reason_str);
    ESP_LOGI(TAG, "=================================================");

    // ═══════════════════════════════════════════════════════════════════════
    // DEEP SLEEP WAKEUP: Extra delay for hardware stabilization
    // ═══════════════════════════════════════════════════════════════════════
    if (reset_reason == ESP_RST_DEEPSLEEP) {
        ESP_LOGI(TAG, "Deep sleep wakeup - waiting for hardware stabilization...");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Wait 1 second
    }

    // Log initial memory state
    ESP_LOGI(TAG, "Free heap: %lu bytes, Largest free block: %lu bytes", esp_get_free_heap_size(),
             heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    // ═══════════════════════════════════════════════════════════════════════
    // I2C INITIALIZATION (required for AXP2101 communication)
    // ═══════════════════════════════════════════════════════════════════════
    ESP_LOGI(TAG, "Initializing I2C bus...");
    i2c_master_Init();

    // Additional delay after I2C init, especially for Deep Sleep wakeup
    if (reset_reason == ESP_RST_DEEPSLEEP) {
        ESP_LOGI(TAG, "Waiting for I2C bus to stabilize...");
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // ═══════════════════════════════════════════════════════════════════════
    // AXP2101 POWER MANAGEMENT INITIALIZATION
    // ═══════════════════════════════════════════════════════════════════════
    ESP_LOGI(TAG, "Initializing AXP2101 power management...");
    axp_i2c_prot_init();
    axp_cmd_init();
    ESP_LOGI(TAG, "AXP2101 initialized");

    // Wait for power rails to stabilize after AXP2101 initialization
    if (reset_reason == ESP_RST_DEEPSLEEP) {
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // ═══════════════════════════════════════════════════════════════════════
    // NVS FLASH INITIALIZATION
    // ═══════════════════════════════════════════════════════════════════════
    ESP_LOGI(TAG, "Initializing NVS flash...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ═══════════════════════════════════════════════════════════════════════
    // SD CARD INITIALIZATION
    // ═══════════════════════════════════════════════════════════════════════
    ret = init_sd_card();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card initialization failed!");
        // System can continue without SD card for web interface
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Config Manager
    // ═══════════════════════════════════════════════════════════════════════
    ESP_ERROR_CHECK(config_manager_init());

    // ═══════════════════════════════════════════════════════════════════════
    // BUTTON GPIO CONFIGURATION
    // ═══════════════════════════════════════════════════════════════════════
    ESP_LOGI(TAG, "Initializing button GPIOs...");
    gpio_config_t io_conf = {.pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO) | (1ULL << KEY_BUTTON_GPIO),
                             .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE,
                             .pull_down_en = GPIO_PULLDOWN_DISABLE,
                             .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&io_conf);

    // ═══════════════════════════════════════════════════════════════════════
    // COMPONENT INITIALIZATION - CORRECT ORDER
    // ═══════════════════════════════════════════════════════════════════════
    ESP_LOGI(TAG, "Initializing processing settings...");
    processing_settings_init();  // ESP_ERROR_CHECK(processing_settings_init());

    ESP_LOGI(TAG, "Initializing color palette...");
    color_palette_init();  // ESP_ERROR_CHECK(color_palette_init());

    ESP_LOGI(TAG, "Initializing image processor...");
    ESP_ERROR_CHECK(image_processor_init());

    // Portrait Combine Feature (optional - default ist bereits true in image_processor.c)
    ESP_LOGI(TAG, "Configuring portrait combine feature...");
    image_processor_set_portrait_combine_enabled(true);

    ESP_LOGI(TAG, "Initializing display manager...");
    ESP_ERROR_CHECK(display_manager_init());

    ESP_LOGI(TAG, "Initializing album manager...");
    ESP_ERROR_CHECK(album_manager_init());

    ESP_LOGI(TAG, "Initializing power manager...");
    ESP_ERROR_CHECK(power_manager_init());

    // ═══════════════════════════════════════════════════════════════════════
    // CHECK WAKEUP SOURCE AND HANDLE ACCORDINGLY
    // ═══════════════════════════════════════════════════════════════════════

    // ───────────────────────────────────────────────────────────────────────
    // TIMER WAKEUP: Auto-rotate from deep sleep
    // ───────────────────────────────────────────────────────────────────────
    // if (true) {
    if (power_manager_is_timer_wakeup()) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "═════════════════════════════════════════");
        ESP_LOGI(TAG, "  TIMER WAKEUP: Auto-Rotate Cycle");
        ESP_LOGI(TAG, "═════════════════════════════════════════");
        ESP_LOGI(TAG, "");

        // Reset flags
        display_was_updated_by_telegram = false;
        pending_display_update = false;
        telegram_bot_reset_display_flag();  // Reset display flag for next #show

        // Check rotation mode
        rotation_mode_t rotation_mode = config_manager_get_rotation_mode();

        if (rotation_mode == ROTATION_MODE_URL) {
            // URL mode - need WiFi to fetch image from URL
            ESP_LOGI(TAG, "URL rotation mode - initializing WiFi");
            ESP_ERROR_CHECK(wifi_manager_init());

            // if (connect_to_wifi_with_timeout(60)) {
            if (connect_to_wifi_with_timeout(60, true)) {  // Stop WiFi on timeout
                const char *image_url = config_manager_get_image_url();
                ESP_LOGI(TAG, "Downloading from: %s", image_url);
                char saved_bmp_path[512];

                if (fetch_and_save_image_from_url(image_url, saved_bmp_path,
                                                  sizeof(saved_bmp_path)) == ESP_OK) {
                    ESP_LOGI(TAG, "Successfully downloaded and saved image, displaying...");
                    display_manager_show_image(saved_bmp_path);
                    ESP_LOGI(TAG, "URL image display complete");
                } else {
                    ESP_LOGE(TAG,
                             "Failed to download image from URL, falling back to SD card rotation");
                    display_manager_handle_wakeup();
                }
            } else {
                ESP_LOGE(TAG, "WiFi connection timeout, falling back to SD card rotation");
                display_manager_handle_wakeup();
            }

            // Go back to sleep after URL mode
            ESP_LOGI(TAG, "URL rotation complete, entering deep sleep");
            power_manager_enter_sleep();
            // Won't reach here
        }

        // SD card mode - continue with Telegram check logic...

        // Initialize Telegram
        ESP_ERROR_CHECK(telegram_bot_init());
        bool check_telegram = telegram_bot_get_check_on_timer_wakeup();
        bool send_wakeup_notification = telegram_bot_get_notify_on_timer_wakeup();

        // Start WiFi if Telegram check is enabled
        if (check_telegram && telegram_bot_has_token()) {
            ESP_LOGI(TAG, "Telegram check enabled - starting WiFi");
            ESP_ERROR_CHECK(wifi_manager_init());

            char wifi_ssid[WIFI_SSID_MAX_LEN] = {0};
            char wifi_pass[WIFI_PASS_MAX_LEN] = {0};

            if (wifi_manager_load_credentials(wifi_ssid, wifi_pass) == ESP_OK) {
                // if (connect_to_wifi_with_timeout(30)) {  // 30 s Timeout
                if (connect_to_wifi_with_timeout(30, true)) {  // Stop WiFi on timeout
                    char ip_str[16];
                    wifi_manager_get_ip(ip_str, sizeof(ip_str));
                    ESP_LOGI(TAG, " WiFi connected: %s", ip_str);
                    vTaskDelay(pdMS_TO_TICKS(5000));

                    /*
    if (wifi_manager_connect(wifi_ssid, wifi_pass) == ESP_OK) {
        ESP_LOGI(TAG, "WiFi connected, waiting for network...");

        // Wait for WiFi connection
        int retry = 0;
        while (!wifi_manager_is_connected() && retry < 100) {
            vTaskDelay(pdMS_TO_TICKS(100));
            retry++;
        }

        if (wifi_manager_is_connected()) {
            char ip_str[16];
            wifi_manager_get_ip(ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, " WiFi connected: %s", ip_str);
            vTaskDelay(pdMS_TO_TICKS(5000)); //1000
                    */
                    // Telegram Check
                    ESP_LOGI(TAG, "Checking Telegram updates...");
                    bool telegram_updates_found = telegram_bot_check_updates();

                    if (telegram_updates_found) {
                        // Updates found - start polling, display update later
                        ESP_LOGI(TAG, " Telegram updates detected");
                        ESP_LOGI(TAG, "Starting polling (60s timeout)...");
                        pending_display_update = true;

                        // Polling task handles display update
                        xTaskCreate(telegram_check_task, "telegram_poll", 32768, NULL, 5,
                                    &telegram_task_handle);

                        // Wait until task is finished
                        while (telegram_task_handle != NULL) {
                            vTaskDelay(pdMS_TO_TICKS(1000));
                        }
                    } else {
                        // No updates- Wake-up Notification & Display now
                        ESP_LOGI(TAG, "No Telegram updates found...");

                        if (send_wakeup_notification) {
                            int64_t chat_id = telegram_bot_get_last_chat_id();
                            if (chat_id != 0) {
                                ESP_LOGI(TAG, "Sending wake-up notification...");
                                telegram_bot_notify_wakeup(chat_id);
                                vTaskDelay(pdMS_TO_TICKS(2000));
                            }
                        }

                        // Get next image BEFORE Telegram notification
                        char selected_image[512] = {0};
                        bool image_preselected = false;

                        if (telegram_bot_get_notify_on_display_update()) {
                            // Wähle Bild EINMAL aus
                            if (display_manager_get_next_image(selected_image,
                                                               sizeof(selected_image)) == ESP_OK) {
                                image_preselected = true;

                                int64_t chat_id = telegram_bot_get_last_chat_id();
                                if (chat_id != 0 && telegram_bot_has_token()) {
                                    ESP_LOGI(TAG,
                                             "Sending display update notification with "
                                             "pre-selected image...");
                                    telegram_bot_notify_display_update(chat_id, selected_image);

                                    // Wait for thumbnail upload
                                    ESP_LOGI(TAG, "Waiting for thumbnail upload...");
                                    int timeout = 0;
                                    while (telegram_bot_is_sending_photo() && timeout < 100) {
                                        vTaskDelay(pdMS_TO_TICKS(100));
                                        timeout++;
                                    }

                                    if (timeout >= 100) {
                                        ESP_LOGW(TAG, "Thumbnail upload timeout!");
                                    } else {
                                        ESP_LOGI(TAG, "Thumbnail upload completed");
                                    }
                                }
                            }
                        }

                        // WiFi trennen
                        ESP_LOGI(TAG, "Disconnecting WiFi...");
                        wifi_manager_stop();
                        telegram_bot_reset_display_flag();  // Reset on WiFi disconnect
                        vTaskDelay(pdMS_TO_TICKS(500));

                        // Display update with preselected image
                        ESP_LOGI(TAG, "Performing display update...");
                        if (image_preselected) {
                            ESP_LOGI(TAG, "Using pre-selected image: %s", selected_image);
                            display_manager_show_image(selected_image);
                        } else {
                            // Fallback: normal auto-rotation
                            display_manager_handle_wakeup();
                        }
                    }

                } else {
                    ESP_LOGW(TAG, "WiFi connection timeout - display update without Telegram");
                    display_manager_handle_wakeup();
                }
                /*
} else {
    ESP_LOGW(TAG, "Failed to connect to WiFi");
    display_manager_handle_wakeup();
}
                */
            } else {
                ESP_LOGW(TAG, "No WiFi credentials found");
                display_manager_handle_wakeup();
            }
        } else {
            // Telegram disabled - display update immediately
            ESP_LOGI(TAG, "Telegram check disabled - display update only");
            display_manager_handle_wakeup();
        }

        // Back to Deep Sleep
        ESP_LOGI(TAG, "Auto-rotate complete, entering deep sleep");
        power_manager_enter_sleep();
        // Code never reaches this point!
    }

    // ───────────────────────────────────────────────────────────────────────
    // KEY BUTTON WAKEUP: Manual image rotation
    // ───────────────────────────────────────────────────────────────────────
    else if (power_manager_is_key_button_wakeup()) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "═════════════════════════════════════════");
        ESP_LOGI(TAG, "  KEY BUTTON WAKEUP: Manual Rotation");
        ESP_LOGI(TAG, "═════════════════════════════════════════");
        ESP_LOGI(TAG, "");

        // Reset display flag for next #show
        telegram_bot_reset_display_flag();

        // Simple image change without Telegram/WiFi
        display_manager_handle_wakeup();

        // Back to deep sleep
        ESP_LOGI(TAG, "Manual rotation complete, entering deep sleep");
        power_manager_enter_sleep();
        // Code never reaches this point!
    }

    // ───────────────────────────────────────────────────────────────────────
    // BOOT BUTTON WAKEUP oder POWER-ON: Normale Initialisierung
    // ───────────────────────────────────────────────────────────────────────
    else if (power_manager_is_boot_button_wakeup()) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "═════════════════════════════════════════");
        ESP_LOGI(TAG, "  BOOT BUTTON WAKEUP: Web Interface Mode");
        ESP_LOGI(TAG, "═════════════════════════════════════════");
        ESP_LOGI(TAG, "");
        // Reset display flag for next #show
        telegram_bot_reset_display_flag();
        // Continue below with normal initialization
    }

    // ═══════════════════════════════════════════════════════════════════════
    // WIFI INITIALIZATION WITH SD-CARD FALLBACK
    // ═══════════════════════════════════════════════════════════════════════
    // Priority:
    // 1. Try NVS (previously saved credentials)
    // 2. Try SD card (/sdcard/wifi.txt)
    // 3. Fall back to provisioning AP mode
    char wifi_ssid[WIFI_SSID_MAX_LEN] = {0};
    char wifi_pass[WIFI_PASS_MAX_LEN] = {0};
    bool credentials_loaded = false;
    bool credentials_from_sd = false;
    bool provisioning_mode = false;  // Track if we're in provisioning mode

    ESP_LOGI(TAG, "Initializing WiFi manager...");
    ESP_ERROR_CHECK(wifi_manager_init());
    // ───────────────────────────────────────────────────────────────────────
    // Step 1: Try loading credentials from NVS first
    // ───────────────────────────────────────────────────────────────────────
    esp_err_t cred_err = wifi_manager_load_credentials(wifi_ssid, wifi_pass);

    if (cred_err == ESP_OK) {
        ESP_LOGI(TAG, " WiFi credentials found in NVS: %s", wifi_ssid);
        credentials_loaded = true;
    } else {
        ESP_LOGI(TAG, "No WiFi credentials in NVS, checking SD card...");
        // ───────────────────────────────────────────────────────────────────
        // Step 2: Try loading from SD card (/sdcard/wifi.txt)
        // ───────────────────────────────────────────────────────────────────
        cred_err = wifi_manager_load_credentials_from_sd(wifi_ssid, wifi_pass);

        if (cred_err == ESP_OK) {
            ESP_LOGI(TAG, " WiFi credentials loaded from SD card");
            credentials_loaded = true;
            credentials_from_sd = true;
        } else {
            ESP_LOGW(TAG, "No WiFi credentials found on SD card");
        }
    }
    // ───────────────────────────────────────────────────────────────────────
    // Step 3: Connect to WiFi if credentials are available
    // ───────────────────────────────────────────────────────────────────────
    /*
if (credentials_loaded) {
    ESP_LOGI(TAG, "Connecting to WiFi: %s", wifi_ssid);
    esp_err_t wifi_ret = wifi_manager_connect(wifi_ssid, wifi_pass);

    if (wifi_ret == ESP_OK) {
        ESP_LOGI(TAG, " WiFi connected successfully");

    */
    if (credentials_loaded) {
        ESP_LOGI(TAG, "Attempting WiFi connection: %s", wifi_ssid);
        // Use timeout-based connection to prevent indefinite blocking
        if (connect_to_wifi_with_timeout(30, false)) {  // Keep WiFi for web interface
            ESP_LOGI(TAG, " WiFi connected successfully");

            // ─────────────────────────────────────────────────────────────
            // If credentials came from SD card, save to NVS and rename file
            // ─────────────────────────────────────────────────────────────
            if (credentials_from_sd) {
                ESP_LOGI(TAG, "Saving WiFi credentials to NVS...");
                wifi_manager_save_credentials(wifi_ssid, wifi_pass);

                // Rename wifi.txt to wifi.bak (backup)
                ESP_LOGI(TAG, "Renaming %s to %s", WIFI_CREDENTIALS_FILE, WIFI_CREDENTIALS_BACKUP);

                if (rename(WIFI_CREDENTIALS_FILE, WIFI_CREDENTIALS_BACKUP) == 0) {
                    ESP_LOGI(TAG, " WiFi credentials file backed up");
                } else {
                    ESP_LOGW(TAG, "Failed to rename credentials file");
                    ESP_LOGW(TAG, "(File may not exist or backup already exists)");
                }
            }

            // Start mDNS service
            ESP_LOGI(TAG, "Starting mDNS service...");
            mdns_service_init();

        } else {
            ESP_LOGE(TAG, "✗ Failed to connect to WiFi");

            // If credentials were from SD card and connection failed, don't save them
            if (credentials_from_sd) {
                ESP_LOGW(TAG, "Not saving invalid credentials from SD card");
            }
        }
    } else {
        // ───────────────────────────────────────────────────────────────────
        // Step 4: No credentials found - start provisioning AP
        // ───────────────────────────────────────────────────────────────────
        ESP_LOGW(TAG, "No WiFi credentials available");
        ESP_LOGI(TAG, "Starting provisioning AP mode...");

        wifi_provisioning_start_ap();
        provisioning_mode = true;  // Mark that we're in provisioning mode

        ESP_LOGI(TAG, "Connect to WiFi: %s (Password: %s)", DEFAULT_WIFI_SSID,
                 DEFAULT_WIFI_PASSWORD);
        ESP_LOGI(TAG, "Then open: http://192.168.4.1 to configure");
    }

    // ═══════════════════════════════════════════════════════════════════════
    // WIFI RESET: Check for 10-second BOOT button hold
    // ═══════════════════════════════════════════════════════════════════════
    // This allows forcing WiFi provisioning mode by holding BOOT for 10s
    ESP_LOGI(TAG, "Hold BOOT button for 10s to reset WiFi settings...");

    bool boot_held = true;
    uint32_t hold_start = xTaskGetTickCount();

    while (boot_held && (xTaskGetTickCount() - hold_start) * portTICK_PERIOD_MS < 10000) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) != 0) {
            boot_held = false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (boot_held) {
        ESP_LOGW(TAG, "BOOT button held for 10s - RESETTING WIFI CREDENTIALS");

        // Erase WiFi credentials from NVS
        nvs_handle_t nvs_handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
            nvs_erase_key(nvs_handle, NVS_WIFI_SSID_KEY);
            nvs_erase_key(nvs_handle, NVS_WIFI_PASS_KEY);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
        }

        ESP_LOGI(TAG, "Restarting to enter provisioning mode...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    // ═══════════════════════════════════════════════════════════════════════
    // HTTP SERVER INITIALIZATION - Only if not in provisioning mode
    // ═══════════════════════════════════════════════════════════════════════
    if (!provisioning_mode) {
        ESP_LOGI(TAG, "Starting HTTP server...");
        ESP_ERROR_CHECK(http_server_init());


		// ═══════════════════════════════════════════════════════════════════════
		// TELEGRAM BOT INITIALIZATION (v1.9.0_tlg)
		// ═══════════════════════════════════════════════════════════════════════
		ESP_LOGI(TAG, "Initializing Telegram bot...");
		ESP_ERROR_CHECK(telegram_bot_init());

		if (true) {  // enable POWER-ON NOTIFICATION
			// ============================================================================
			// POWER-ON NOTIFICATION (Battery mode only)
			// ============================================================================

			if (reset_reason == ESP_RST_POWERON) {
				ESP_LOGI(TAG, "");
				ESP_LOGI(TAG, "==========================================");
				ESP_LOGI(TAG, "POWER-ON DETECTED - System Started");
				ESP_LOGI(TAG, "==========================================");
				ESP_LOGI(TAG, "");

				// Nur Benachrichtigung wenn Telegram aktiv UND kein USB
				if (telegram_bot_has_token() && !axp_is_usb_connected() &&
					wifi_manager_is_connected()) {
					int64_t chat_id = telegram_bot_get_last_chat_id();
					if (chat_id != 0) {
						ESP_LOGI(TAG, "Sending Power-On notification to Telegram...");

						// Hole System-Info
						int battery_percent = axp_get_battery_percent();
						bool is_charging = axp_is_charging();
						int rotate_interval = display_manager_get_rotate_interval();
						bool auto_rotate = display_manager_get_auto_rotate();
						bool deep_sleep = power_manager_get_deep_sleep_enabled();

						// Baue Nachricht
						char message[512];
						snprintf(message, sizeof(message),
								 "🟢 <b>System Started</b>\n"
								 "\n"
								 "<b>Power Status</b>\n"
								 "Battery: %d%%\n"
								 "Charging: %s\n"
								 "Power Source: Battery\n"
								 "\n"
								 "<b>Configuration</b>\n"
								 "Auto-Rotate: %s\n"
								 "Rotation Interval: %d seconds\n"
								 "Deep Sleep: %s\n"
								 "\n"
								 "<b>System Behavior</b>\n"
								 "Auto-Sleep Timer: %d seconds\n"
								 "After timeout, system will enter deep sleep.\n"
								 "Next wakeup in %d seconds for image rotation.",
								 battery_percent, is_charging ? "Yes" : "No",
								 auto_rotate ? "Enabled" : "Disabled", rotate_interval,
								 deep_sleep ? "Enabled" : "Disabled", AUTO_SLEEP_TIMEOUT_SEC,
								 rotate_interval);

						// Sende Nachricht
						esp_err_t err = telegram_bot_send_message(chat_id, message);
						if (err == ESP_OK) {
							ESP_LOGI(TAG, "Power-On notification sent successfully");
						} else {
							ESP_LOGW(TAG, "Failed to send Power-On notification");
						}

						// Warte damit Nachricht gesendet wird
						vTaskDelay(pdMS_TO_TICKS(2000));
					}
				} else {
					if (!telegram_bot_has_token()) {
						ESP_LOGI(TAG, "Telegram not configured - skipping Power-On notification");
					} else if (axp_is_usb_connected()) {
						ESP_LOGI(TAG, "USB connected - skipping Power-On notification");
					} else if (!wifi_manager_is_connected()) {
						ESP_LOGI(TAG, "WiFi not connected - skipping Power-On notification");
					}
				}
			}
		}

		if (telegram_bot_has_token()) {
			// Send wake-up notification for BOOT button wakeup
			if (power_manager_is_boot_button_wakeup()) {
				int64_t chat_id = telegram_bot_get_last_chat_id();
				if (chat_id != 0) {
					ESP_LOGI(TAG, "Sending BOOT wakeup notification to Telegram...");
					telegram_bot_notify_wakeup(chat_id);
				}
			}

			// Start telegram polling task
			xTaskCreate(telegram_check_task, "telegram_check", 32768, NULL, 5, NULL);
		} else {
			ESP_LOGI(TAG, "No Telegram token configured");
		}

		// ═══════════════════════════════════════════════════════════════════════
		// LOG NETWORK INFORMATION
		// ═══════════════════════════════════════════════════════════════════════
		if (wifi_manager_is_connected()) {
			char ip_str[16];
			wifi_manager_get_ip(ip_str, sizeof(ip_str));
			vTaskDelay(pdMS_TO_TICKS(2000));

			ESP_LOGI(TAG, "===========================================");
			ESP_LOGI(TAG, "Web interface available at: http://%s", ip_str);
			ESP_LOGI(TAG, "Or use: http://photoframe.local");
			ESP_LOGI(TAG, "===========================================");
		}

		// ═══════════════════════════════════════════════════════════════════════
		// START BUTTON TASK
		// ═══════════════════════════════════════════════════════════════════════
		xTaskCreate(button_task, "button_task", 12288, NULL, 5, NULL);

		// ═══════════════════════════════════════════════════════════════════════
		// MARK SYSTEM AS READY
		// ═══════════════════════════════════════════════════════════════════════
		http_server_set_ready();
		ESP_LOGI(TAG, "PhotoFrame started successfully");

    } else {
        ESP_LOGI(TAG, "Provisioning mode active - HTTP server will start after provisioning");
    }
}