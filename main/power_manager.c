#include "power_manager.h"

#include "axp_prot.h"
#include "config.h"
#include "config_manager.h"   // <-- new from v1.9.0
#include "display_manager.h"  // <-- old
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_server.h"
#include "nvs.h"
#include "telegram_bot.h"  // <-- v1.9.0_tlg Telegram
#include "utils.h"

static const char *TAG = "power_manager";

static TaskHandle_t sleep_timer_task_handle = NULL;
static TaskHandle_t rotation_timer_task_handle = NULL;
static int64_t next_sleep_time = 0;     // Use absolute time for sleep timer
static bool deep_sleep_enabled = true;  // Enabled by default, can be disabled for HA integration
static esp_sleep_wakeup_cause_t last_wakeup_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
static int64_t next_rotation_time = 0;  // Use absolute time for rotation
static uint64_t ext1_wakeup_pin_mask = 0;

// Battery Low Mode State
static bool battery_low_mode_active = false;
static int32_t saved_rotate_interval = 0;  // Saved original interval

/**
 * @brief Check and manage battery low mode
 *
 * When battery < 20% and not charging:
 * - Set rotation interval to 21600s (6 hours)
 * - Save original interval to NVS
 *
 * When battery > 80% or charging:
 * - Restore original rotation interval
 * - Clear battery low mode
 */
/**
 * @brief Check and manage battery low mode
 *
 * When battery ≤ 20% and not charging:
 * - Set rotation interval to 21600s (6 hours)
 * - Save original interval to NVS
 *
 * When battery ≥ 80% OR charging OR USB connected OR no battery:
 * - Restore original rotation interval
 * - Clear battery low mode
 */
static void check_battery_low_mode(void)
{
    int battery_percent = axp_get_battery_percent();
    bool is_charging = axp_is_charging();
    bool usb_connected = axp_is_usb_connected();

    // ═══════════════════════════════════════════════════════════════
    // ACTIVATE Low Battery Mode: ≤20% AND NOT charging AND battery present
    // ═══════════════════════════════════════════════════════════════
    if (battery_percent >= 0 &&  // check for battery connection
        battery_percent <= BATTERY_LOW_THRESHOLD && !is_charging && !usb_connected) {
        if (!battery_low_mode_active) {
            // Get current rotation interval
            int current_interval = config_manager_get_rotate_interval();

            // Only activate if current interval is lower than low mode interval
            if (current_interval < BATTERY_LOW_MODE_INTERVAL_SEC) {
                ESP_LOGW(TAG, "🔋 Battery Low Mode ACTIVATED (≤%d%%, not charging)",
                         BATTERY_LOW_THRESHOLD);
                ESP_LOGI(TAG, "Saving current interval: %d seconds", current_interval);

                // Save current interval to NVS
                nvs_handle_t nvs_handle;
                if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
                    nvs_set_i32(nvs_handle, NVS_BAT_SAVED_INTERVAL_KEY, current_interval);
                    nvs_set_u8(nvs_handle, NVS_BAT_LOW_MODE_KEY, 1);
                    nvs_commit(nvs_handle);
                    nvs_close(nvs_handle);
                }

                saved_rotate_interval = current_interval;

                // Set long interval to save battery
                display_manager_set_rotate_interval(BATTERY_LOW_MODE_INTERVAL_SEC);
                ESP_LOGI(TAG, "Rotation interval changed: %d → %d seconds (battery saving)",
                         current_interval, BATTERY_LOW_MODE_INTERVAL_SEC);

                battery_low_mode_active = true;
            } else {
                ESP_LOGD(TAG, "Battery low, but interval already ≥ %d seconds, skipping mode",
                         BATTERY_LOW_MODE_INTERVAL_SEC);
            }
        }
    }
    // ═══════════════════════════════════════════════════════════════
    // DEACTIVATE Low Battery Mode: ≥80% OR charging OR USB OR no battery
    // ═══════════════════════════════════════════════════════════════
    else if (battery_low_mode_active &&
             (battery_percent >= BATTERY_RECOVER_THRESHOLD || is_charging || usb_connected ||
              battery_percent < 0)) {  // at -1% (no battery)

        ESP_LOGW(TAG, "Battery Low Mode DEACTIVATED (≥%d%% or charging or USB or no battery)",
                 BATTERY_RECOVER_THRESHOLD);

        // Restore saved interval from NVS
        nvs_handle_t nvs_handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
            int32_t saved_interval = IMAGE_ROTATE_INTERVAL_SEC;  // Fallback
            if (nvs_get_i32(nvs_handle, NVS_BAT_SAVED_INTERVAL_KEY, &saved_interval) == ESP_OK) {
                saved_rotate_interval = saved_interval;
            }
            nvs_close(nvs_handle);
        }

        if (saved_rotate_interval > 0) {
            display_manager_set_rotate_interval(saved_rotate_interval);
            ESP_LOGI(TAG, "Rotation interval restored: %d seconds", saved_rotate_interval);
        } else {
            ESP_LOGW(TAG, "No saved interval found, keeping current: %d seconds",
                     config_manager_get_rotate_interval());
        }

        // Clear low mode flag in NVS
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
            nvs_erase_key(nvs_handle, NVS_BAT_LOW_MODE_KEY);
            nvs_erase_key(nvs_handle, NVS_BAT_SAVED_INTERVAL_KEY);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
        }

        battery_low_mode_active = false;
        saved_rotate_interval = 0;
    }
}

static void rotation_timer_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Check battery low mode every cycle
        // check_battery_low_mode();

        // Run active rotation when:
        // 1. USB is connected (device stays awake), OR
        // 2. Deep sleep is disabled (device stays awake on battery)
        bool should_use_active_rotation = axp_is_usb_connected() || !deep_sleep_enabled;

        if (!should_use_active_rotation) {
            next_rotation_time =
                0;  // Reset when on battery with deep sleep enabled (uses sleep-based rotation)
            continue;
        }

        // Handle active rotation when device stays awake and auto-rotate enabled
        if (config_manager_get_auto_rotate()) {
            int64_t now = esp_timer_get_time();  // Get absolute time in microseconds

            if (next_rotation_time == 0) {
                // Initialize next rotation time
                int rotate_interval = config_manager_get_rotate_interval();
                next_rotation_time = now + (rotate_interval * 1000000LL);

                const char *reason = axp_is_usb_connected() ? "USB powered" : "deep sleep disabled";
                ESP_LOGI(TAG, "Active rotation scheduled in %d seconds (%s)", rotate_interval,
                         reason);
            } else if (now >= next_rotation_time) {
                // Time to rotate
                const char *reason = axp_is_usb_connected() ? "USB powered" : "deep sleep disabled";
                ESP_LOGI(TAG, "Active rotation triggered (%s)", reason);

                // Check rotation mode
                rotation_mode_t rotation_mode = config_manager_get_rotation_mode();
                if (rotation_mode == ROTATION_MODE_URL) {
                    // URL mode - fetch image from URL
                    const char *image_url = config_manager_get_image_url();
                    ESP_LOGI(TAG, "URL rotation mode, downloading from: %s", image_url);

                    char saved_bmp_path[512];
                    if (fetch_and_save_image_from_url(image_url, saved_bmp_path,
                                                      sizeof(saved_bmp_path)) == ESP_OK) {
                        ESP_LOGI(TAG, "Successfully downloaded and saved image, displaying...");
                        display_manager_show_image(saved_bmp_path);
                    } else {
                        ESP_LOGE(
                            TAG,
                            "Failed to download image from URL, falling back to SD card rotation");
                        display_manager_handle_wakeup();
                    }
                } else {
                    // SD card mode - use normal SD card rotation
                    display_manager_handle_wakeup();
                }

                // Schedule next rotation (might have changed due to battery mode)
                int rotate_interval = config_manager_get_rotate_interval();
                next_rotation_time = now + (rotate_interval * 1000000LL);
                ESP_LOGI(TAG, "Next rotation scheduled in %d seconds", rotate_interval);
            }
        } else {
            next_rotation_time = 0;  // Reset if auto-rotate disabled
        }
    }
}
static void sleep_timer_task(void *arg)
{
    int64_t last_blink_time = 0;
    int64_t last_log_time = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Skip auto-sleep when USB is connected
        if (axp_is_usb_connected()) {
            // Reset timer so it doesn't trigger immediately when USB is unplugged
            next_sleep_time = 0;
            continue;
        }

        // Handle auto-sleep timer when on battery (only if deep sleep is enabled)
        if (deep_sleep_enabled) {
            int64_t now = esp_timer_get_time();

            if (next_sleep_time == 0) {
                // Initialize sleep timer
                next_sleep_time = now + (AUTO_SLEEP_TIMEOUT_SEC * 1000000LL);
                last_blink_time = now;
                last_log_time = now;
                ESP_LOGI(TAG, "Auto-sleep timer started, will sleep in %d seconds",
                         AUTO_SLEEP_TIMEOUT_SEC);
            }

            int64_t remaining_us = next_sleep_time - now;
            int32_t remaining_sec = (int32_t) (remaining_us / 1000000LL);

            if (remaining_sec > 0) {
                // Visual indicator: blink GREEN LED every 10 seconds
                if ((now - last_blink_time) >= 10000000LL) {
                    gpio_set_level(LED_GREEN_GPIO, 0);  // Turn on (active-low)
                    vTaskDelay(pdMS_TO_TICKS(200));
                    gpio_set_level(LED_GREEN_GPIO, 1);  // Turn off
                    last_blink_time = now;
                }

                // Log countdown every 30 seconds
                if ((now - last_log_time) >= 30000000LL) {
                    ESP_LOGI(TAG, "Auto-sleep countdown: %ld seconds remaining", remaining_sec);
                    last_log_time = now;
                }
            } else {
                // Time to sleep
                ESP_LOGI(TAG, "Sleep timeout reached, entering deep sleep");
                power_manager_enter_sleep();
            }
        } else {
            // Deep sleep disabled - reset timer to prevent it from triggering
            next_sleep_time = 0;
        }
    }
}

static void power_manager_enable_auto_light_sleep(void)
{
    // Configure automatic light sleep with CPU frequency scaling
    // This allows the ESP32 to automatically enter light sleep when idle
    // and scale CPU frequency down to save power while maintaining WiFi connectivity
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,         // Maximum CPU frequency (160MHz for ESP32-S3)
        .min_freq_mhz = 40,          // Minimum CPU frequency (40MHz when idle)
        .light_sleep_enable = true,  // Enable automatic light sleep
    };

    esp_err_t pm_ret = esp_pm_configure(&pm_config);
    if (pm_ret == ESP_OK) {
        ESP_LOGI(TAG, "Automatic light sleep enabled (CPU: 160MHz -> 40MHz)");
    } else {
        ESP_LOGW(TAG, "Failed to configure power management: %s", esp_err_to_name(pm_ret));
    }
}

static void power_manager_disable_auto_light_sleep(void)
{
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,  // Maximum CPU frequency (160MHz for ESP32-S3)
        .min_freq_mhz = 40,   // Minimum CPU frequency (40MHz when idle)
        .light_sleep_enable = false,
    };

    esp_err_t pm_ret = esp_pm_configure(&pm_config);
    if (pm_ret == ESP_OK) {
        ESP_LOGI(TAG, "Automatic light sleep disabled");
    } else {
        ESP_LOGW(TAG, "Failed to configure power management: %s", esp_err_to_name(pm_ret));
    }

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
}

esp_err_t power_manager_init(void)
{
    // Load deep sleep enabled setting from NVS
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        uint8_t enabled = 1;  // Default to enabled
        nvs_get_u8(nvs_handle, NVS_DEEP_SLEEP_KEY, &enabled);
        deep_sleep_enabled = (enabled != 0);

        // Load battery low mode state from NVS
        uint8_t low_mode_active = 0;
        if (nvs_get_u8(nvs_handle, NVS_BAT_LOW_MODE_KEY, &low_mode_active) == ESP_OK) {
            battery_low_mode_active = (low_mode_active != 0);

            // Load saved interval
            int32_t saved_interval = 0;
            if (nvs_get_i32(nvs_handle, NVS_BAT_SAVED_INTERVAL_KEY, &saved_interval) == ESP_OK) {
                saved_rotate_interval = saved_interval;
            }

            if (battery_low_mode_active) {
                ESP_LOGI(TAG, "Battery Low Mode was active before reboot, saved interval: %d",
                         saved_rotate_interval);
            }
        }
        nvs_close(nvs_handle);
    }

    // Prüfe Battery Low Mode direkt nach Init
    check_battery_low_mode();

    ESP_LOGI(TAG, "Deep sleep %s", deep_sleep_enabled ? "enabled" : "disabled");

    // Get wakeup causes bitmap (new API in ESP-IDF v6.0)
    uint32_t wakeup_causes = esp_sleep_get_wakeup_causes();
    ext1_wakeup_pin_mask = 0;

    // Convert bitmap to single cause for backward compatibility
    if (wakeup_causes & (1 << ESP_SLEEP_WAKEUP_TIMER)) {
        last_wakeup_cause = ESP_SLEEP_WAKEUP_TIMER;
        ESP_LOGI(TAG, "Wakeup caused by timer (auto-rotate)");
    } else if (wakeup_causes & (1 << ESP_SLEEP_WAKEUP_EXT1)) {
        last_wakeup_cause = ESP_SLEEP_WAKEUP_EXT1;
        // ESP32-S3 only supports EXT1, check which GPIO triggered it
        ext1_wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();

        if (ext1_wakeup_pin_mask & (1ULL << BOOT_BUTTON_GPIO)) {
            ESP_LOGI(TAG, "Wakeup caused by BOOT button (GPIO %d)", BOOT_BUTTON_GPIO);
        } else if (ext1_wakeup_pin_mask & (1ULL << KEY_BUTTON_GPIO)) {
            ESP_LOGI(TAG, "Wakeup caused by KEY button (GPIO %d)", KEY_BUTTON_GPIO);
        } else {
            ESP_LOGI(TAG, "Wakeup caused by EXT1 (unknown GPIO: 0x%llx)", ext1_wakeup_pin_mask);
        }
    } else {
        last_wakeup_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
        ESP_LOGI(TAG, "Not a deep sleep wakeup");
    }

    // Configure button GPIOs as input with pull-ups
    // NOTE: PWR_BUTTON_GPIO (GPIO 5) is NOT configured here - it's AXP2101 SYS_OUT pin
    gpio_config_t io_conf = {.intr_type = GPIO_INTR_DISABLE,
                             .mode = GPIO_MODE_INPUT,
                             .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO) | (1ULL << KEY_BUTTON_GPIO),
                             .pull_down_en = GPIO_PULLDOWN_DISABLE,
                             .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io_conf);

    // Hold GPIO state during deep sleep to prevent floating
    // This prevents false EXT1 wake-ups when timer fires
    gpio_hold_en(BOOT_BUTTON_GPIO);
    gpio_hold_en(KEY_BUTTON_GPIO);
    gpio_deep_sleep_hold_en();

    // Configure LED GPIOs as output and turn them off
    gpio_config_t led_conf = {.intr_type = GPIO_INTR_DISABLE,
                              .mode = GPIO_MODE_OUTPUT,
                              .pin_bit_mask = (1ULL << LED_RED_GPIO) | (1ULL << LED_GREEN_GPIO),
                              .pull_down_en = GPIO_PULLDOWN_DISABLE,
                              .pull_up_en = GPIO_PULLUP_DISABLE};
    gpio_config(&led_conf);

    // Turn on red LED only if deep sleep is enabled (to indicate battery mode)
    // If deep sleep is disabled, keep LED off to save battery
    gpio_set_level(LED_RED_GPIO, deep_sleep_enabled ? 0 : 1);  // active-low
    gpio_set_level(LED_GREEN_GPIO, 1);                         // Turn off green LED (active-low)

    xTaskCreate(sleep_timer_task, "sleep_timer", 4096, NULL, 5, &sleep_timer_task_handle);
    // xTaskCreate(rotation_timer_task, "rotation_timer", 4096, NULL, 5,
    // &rotation_timer_task_handle);
    xTaskCreate(rotation_timer_task, "rotation_timer", 16384, NULL, 5, &rotation_timer_task_handle);

    power_manager_enable_auto_light_sleep();

    ESP_LOGI(TAG, "Power manager initialized");
    return ESP_OK;
}

void power_manager_enter_sleep(void)
{
    power_manager_disable_auto_light_sleep();

    ESP_LOGI(TAG, "Preparing to enter deep sleep mode");

    // Turn off LEDs before sleep to save power (active-low)
    gpio_set_level(LED_RED_GPIO, 1);
    gpio_set_level(LED_GREEN_GPIO, 1);

    // v1.9.0_tlg Telegram START
    // === Send Telegram notification before sleep - error if WiFi off! ===
    if (telegram_bot_get_notify_on_sleep()) {
        int64_t chat_id = telegram_bot_get_last_chat_id();
        if (chat_id != 0 && telegram_bot_has_token()) {
            ESP_LOGI(TAG, "Sending sleep notification to Telegram...");
            telegram_bot_notify_sleep(chat_id);

            // Give time for message to be sent
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    // === Telegram notification END ===
    // v1.9.0_tlg Telegram END

    // Check if auto-rotate is enabled
    if (config_manager_get_auto_rotate()) {
        // Use timer-based sleep for auto-rotate
        int rotate_interval = config_manager_get_rotate_interval();
        ESP_LOGI(TAG, "Auto-rotate enabled, setting timer wake-up for %d seconds", rotate_interval);
        esp_sleep_enable_timer_wakeup(rotate_interval * 1000000ULL);
    }

    // Enable boot button and key button wake-up (ESP32-S3 only supports EXT1)
    esp_sleep_enable_ext1_wakeup((1ULL << BOOT_BUTTON_GPIO) | (1ULL << KEY_BUTTON_GPIO),
                                 ESP_EXT1_WAKEUP_ANY_LOW);

    ESP_LOGI(TAG, "Configuring AXP2101 for deep sleep");
    axp_basic_sleep_start();

    ESP_LOGI(TAG, "Entering deep sleep now");
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_deep_sleep_start();
}

void power_manager_reset_sleep_timer(void)
{
    next_sleep_time = esp_timer_get_time() + (AUTO_SLEEP_TIMEOUT_SEC * 1000000LL);
}

void power_manager_reset_rotate_timer(void)
{
    int rotate_interval = config_manager_get_rotate_interval();
    next_rotation_time = esp_timer_get_time() + (rotate_interval * 1000000LL);
    ESP_LOGI(TAG, "Rotation timer reset, next rotation in %d seconds", rotate_interval);
}

bool power_manager_is_timer_wakeup(void)
{
    return last_wakeup_cause == ESP_SLEEP_WAKEUP_TIMER;
}

bool power_manager_is_ext1_wakeup(void)
{
    return last_wakeup_cause == ESP_SLEEP_WAKEUP_EXT1;
}

bool power_manager_is_boot_button_wakeup(void)
{
    return (last_wakeup_cause == ESP_SLEEP_WAKEUP_EXT1) &&
           (ext1_wakeup_pin_mask & (1ULL << BOOT_BUTTON_GPIO));
}

bool power_manager_is_key_button_wakeup(void)
{
    return (last_wakeup_cause == ESP_SLEEP_WAKEUP_EXT1) &&
           (ext1_wakeup_pin_mask & (1ULL << KEY_BUTTON_GPIO));
}

void power_manager_set_deep_sleep_enabled(bool enabled)
{
    deep_sleep_enabled = enabled;

    // Save to NVS
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_DEEP_SLEEP_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    // Update RED LED state: on when deep sleep enabled, off when disabled (to save battery)
    gpio_set_level(LED_RED_GPIO, enabled ? 0 : 1);  // active-low

    ESP_LOGI(TAG, "Deep sleep %s", enabled ? "enabled" : "disabled");
}

bool power_manager_get_deep_sleep_enabled(void)
{
    return deep_sleep_enabled;
}
