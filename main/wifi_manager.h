#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
esp_err_t wifi_manager_disconnect(void);
bool wifi_manager_is_connected(void);
esp_err_t wifi_manager_get_ip(char *ip_str, size_t len);
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);
esp_err_t wifi_manager_load_credentials(char *ssid, char *password);
EventGroupHandle_t wifi_manager_get_event_group(void);

// ============================================================================
// WIFI SD-CARD CREDENTIALS on SD card
// ============================================================================
/**
 * @brief Try to load WiFi credentials from SD card file
 *
 * Reads credentials from /sdcard/wifi.txt (format: SSID on line 1, PASSWORD on line 2)
 * If successful and connection works, renames file to /sdcard/wifi.bak
 *
 * @param ssid Output buffer for SSID (min WIFI_SSID_MAX_LEN)
 * @param password Output buffer for password (min WIFI_PASS_MAX_LEN)
 * @return ESP_OK if file exists and credentials loaded
 *         ESP_ERR_NOT_FOUND if file doesn't exist
 *         ESP_FAIL on read error
 */
esp_err_t wifi_manager_load_credentials_from_sd(char *ssid, char *password);

/**
 * @brief Check if WiFi credentials exist in NVS
 *
 * @return true if both SSID and password are stored in NVS
 */
bool wifi_manager_has_stored_credentials(void);

// WiFI shut down A
esp_err_t wifi_manager_stop(void);

// WiFI shut down B
/**
 * @brief Stop WiFi immediately without triggering reconnect
 *
 * Disables auto-reconnect and event handlers before stopping WiFi
 * to prevent "connect to AP fail" messages.
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_stop_immediate(void);

#endif