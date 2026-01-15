#ifndef BATTERY_MANAGER_H
#define BATTERY_MANAGER_H
#include <stdbool.h>
#include <stdint.h>

#include "config.h"  // for BATTERY THRESHOLDS
#include "esp_err.h"
// ============================================================================
// BATTERY THRESHOLDS
// ============================================================================
// BATTERY THRESHOLDS - SET TO 100 FOR TESTING!
// #define BATTERY_CRITICAL_THRESHOLD  100  // ⚠️ TEST MODE!
// #define BATTERY_LOW_THRESHOLD       100  // ⚠️ TEST MODE!

// #define BATTERY_CRITICAL_THRESHOLD  10  // 10% or below = CRITICAL
// #define BATTERY_LOW_THRESHOLD       20  // 20% or below = LOW
// ============================================================================
// BATTERY STATE ENUM
// ============================================================================

typedef enum {
    BATTERY_STATE_NORMAL = 0,   // Above 20%
    BATTERY_STATE_LOW = 1,      // 10-20%
    BATTERY_STATE_CRITICAL = 2  // Below 10%
} battery_state_t;

// ============================================================================
// PUBLIC FUNCTIONS
// ============================================================================

/**
 * @brief Initialize battery manager
 *
 * Initializes the battery manager and performs initial battery check.
 *
 * @return ESP_OK on success
 */
esp_err_t battery_manager_init(void);

/**
 * @brief Check battery level and handle state changes
 *
 * Checks current battery level and triggers audio/visual warnings
 * when battery state changes (NORMAL -> LOW -> CRITICAL).
 *
 * NOTE: Audio is currently disabled (needs BoxAudioCodec implementation)
 *       Battery warnings are sent via Telegram instead.
 *
 * @param play_sound IGNORED (audio disabled) - use false
 * @return Current battery state
 */
battery_state_t battery_manager_check(bool play_sound);

/**
 * @brief Send battery warning via Telegram
 *
 * Sends a Telegram notification when battery is LOW or CRITICAL.
 * Includes spam protection to avoid sending duplicate warnings.
 *
 * @param chat_id Telegram chat ID to send message to
 * @param percent Current battery percentage (0-100)
 * @param voltage Current battery voltage in Volts
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t battery_manager_send_telegram_warning(int64_t chat_id, uint8_t percent, float voltage);

/**
 * @brief Get current battery percentage
 *
 * @return Battery percentage (0-100)
 */
uint8_t battery_manager_get_percent(void);

/**
 * @brief Get current battery voltage
 *
 * @return Battery voltage in Volts
 */
float battery_manager_get_voltage(void);

/**
 * @brief Get current battery state
 *
 * @return Current battery state (NORMAL, LOW, or CRITICAL)
 */
battery_state_t battery_manager_get_state(void);

/**
 * @brief Reset battery warning flags
 *
 * Resets the internal flags that prevent duplicate Telegram warnings.
 * Call this after battery has been charged or after a system restart.
 */
void battery_manager_reset_warning_flags(void);

#endif  // BATTERY_MANAGER_H