#include "battery_manager.h"

#include "axp_prot.h"
#include "esp_log.h"
#include "telegram_bot.h"
// #include "audio_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "battery_manager";

// ============================================================================
// GLOBAL STATE
// ============================================================================

static battery_state_t last_battery_state = BATTERY_STATE_NORMAL;
static bool low_battery_warning_sent = false;
static bool critical_battery_warning_sent = false;

// ============================================================================
// PRIVATE FUNCTIONS
// ============================================================================

/**
 * @brief Determine battery state from percentage
 */
static battery_state_t get_battery_state_from_percent(uint8_t percent)
{
    if (percent <= BATTERY_CRITICAL_THRESHOLD) {
        return BATTERY_STATE_CRITICAL;
    } else if (percent <= BATTERY_LOW_THRESHOLD) {
        return BATTERY_STATE_LOW;
    } else {
        return BATTERY_STATE_NORMAL;
    }
}

// ============================================================================
// PUBLIC FUNCTIONS
// ============================================================================

esp_err_t battery_manager_init(void)
{
    ESP_LOGI(TAG, "Battery manager initialized");

    // Initial battery check
    uint8_t percent = axp_get_battery_percent();
    last_battery_state = get_battery_state_from_percent(percent);

    ESP_LOGI(TAG, "Initial battery level: %d%% (State: %s)", percent,
             last_battery_state == BATTERY_STATE_CRITICAL ? "CRITICAL"
             : last_battery_state == BATTERY_STATE_LOW    ? "LOW"
                                                          : "NORMAL");

    return ESP_OK;
}

battery_state_t battery_manager_check(bool play_sound)
{
    uint8_t percent = axp_get_battery_percent();
    float voltage = axp_get_battery_voltage();
    battery_state_t current_state = get_battery_state_from_percent(percent);

    ESP_LOGD(TAG, "Battery check: %d%% (%.2fV) - State: %d", percent, voltage, current_state);

    // ========================================================================
    // STATE CHANGE DETECTION
    // ========================================================================

    if (current_state != last_battery_state) {
        ESP_LOGI(TAG, "Battery state changed: %d -> %d", last_battery_state, current_state);

        // State transition logic
        if (current_state == BATTERY_STATE_CRITICAL) {
            // Entered CRITICAL state
            ESP_LOGW(TAG, "CRITICAL battery level: %d%%", percent);

            // ❌ AUDIO TEMPORARILY DISABLED - Needs BoxAudioCodec framework
            /*
if (play_sound) {
    audio_manager_play_critical_battery_warning();
}
            */

            critical_battery_warning_sent = false;  // Reset for Telegram

        } else if (current_state == BATTERY_STATE_LOW) {
            // Entered LOW state
            if (last_battery_state == BATTERY_STATE_CRITICAL) {
                // Recovery from CRITICAL
                ESP_LOGI(TAG, " Battery recovered from CRITICAL to LOW: %d%%", percent);

                // ❌ AUDIO TEMPORARILY DISABLED - Needs BoxAudioCodec framework
                /*
if (play_sound) {
    audio_manager_play_battery_recovered();
}
                */

            } else {
                // Entered LOW from NORMAL
                ESP_LOGW(TAG, "LOW battery level: %d%%", percent);
                // ❌ AUDIO TEMPORARILY DISABLED - Needs BoxAudioCodec framework
                /*
if (play_sound) {
    audio_manager_play_low_battery_warning();
}
                */
            }

            low_battery_warning_sent = false;  // Reset for Telegram

        } else {
            // Entered NORMAL state (recovery)
            ESP_LOGI(TAG, " Battery recovered to NORMAL: %d%%", percent);

            // ❌ AUDIO TEMPORARILY DISABLED - Needs BoxAudioCodec framework
            /*
if (play_sound && (last_battery_state == BATTERY_STATE_LOW ||
                   last_battery_state == BATTERY_STATE_CRITICAL)) {
    audio_manager_play_battery_recovered();
}
            */

            // Reset all warning flags
            low_battery_warning_sent = false;
            critical_battery_warning_sent = false;
        }

        last_battery_state = current_state;
    }

    return current_state;
}

esp_err_t battery_manager_send_telegram_warning(int64_t chat_id, uint8_t percent, float voltage)
{
    if (chat_id == 0) {
        ESP_LOGW(TAG, "No chat ID configured, skipping Telegram warning");
        return ESP_ERR_INVALID_ARG;
    }

    battery_state_t state = get_battery_state_from_percent(percent);

    // Check if we should send notification (avoid spam)
    if (state == BATTERY_STATE_CRITICAL && critical_battery_warning_sent) {
        ESP_LOGD(TAG, "Critical warning already sent, skipping");
        return ESP_OK;
    }

    if (state == BATTERY_STATE_LOW && low_battery_warning_sent) {
        ESP_LOGD(TAG, "Low warning already sent, skipping");
        return ESP_OK;
    }

    // Prepare message
    char message[256];

    if (state == BATTERY_STATE_CRITICAL) {
        snprintf(message, sizeof(message),
                 "🔴 CRITICAL BATTERY WARNING\\n"
                 "Battery: %d%% (%.2fV)\\n"
                 "System may shut down soon!",
                 percent, voltage);

        critical_battery_warning_sent = true;

    } else if (state == BATTERY_STATE_LOW) {
        snprintf(message, sizeof(message),
                 "🟡 Low Battery Warning\\n"
                 "Battery: %d%% (%.2fV)\\n"
                 "Please charge soon.",
                 percent, voltage);

        low_battery_warning_sent = true;

    } else {
        // Normal state - no warning needed
        return ESP_OK;
    }

    // Send Telegram message
    ESP_LOGI(TAG, "Sending battery warning to Telegram...");
    esp_err_t ret = telegram_bot_send_message(chat_id, message);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, " Battery warning sent");
    } else {
        ESP_LOGW(TAG, "✗ Failed to send battery warning: %s", esp_err_to_name(ret));
    }

    return ret;
}

uint8_t battery_manager_get_percent(void)
{
    return axp_get_battery_percent();
}

float battery_manager_get_voltage(void)
{
    return axp_get_battery_voltage();
}

battery_state_t battery_manager_get_state(void)
{
    return last_battery_state;
}

void battery_manager_reset_warning_flags(void)
{
    ESP_LOGI(TAG, "Resetting battery warning flags");
    low_battery_warning_sent = false;
    critical_battery_warning_sent = false;
}
