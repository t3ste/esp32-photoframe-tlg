#ifndef IMAGE_PROCESSOR_H
#define IMAGE_PROCESSOR_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t image_processor_init(void);
// esp_err_t image_processor_convert_jpg_to_bmp(const char *jpg_path, const char *bmp_path, bool
// use_stock_mode);
void image_processor_reload_palette(void);

// ===== Portrait-Combination =====
/**
 * @brief Enable/disable automatic portrait combination feature
 * @param enabled true to enable, false to disable
 * @return ESP_OK on success
 */
esp_err_t image_processor_set_portrait_combine_enabled(bool enabled);

/**
 * @brief Get current portrait combination feature state
 * @return true if enabled, false if disabled
 */
bool image_processor_get_portrait_combine_enabled(void);

/**
 * @brief Convert JPG to BMP with automatic portrait detection and combination
 *
 * This function:
 * 1. Detects if JPG is portrait format (height > width)
 * 2. If portrait: saves BMP to /sdcard/images/album/portrait/ (rotation disabled)
 * 3. If portrait: searches for another portrait BMP
 * 4. If second portrait found: combines both side-by-side into landscape BMP
 * 5. If landscape: converts normally
 *
 * @param jpg_path Input JPG file path
 * @param bmp_path Output BMP file path (for landscape) or base path
 * @param use_stock_mode Use stock dithering palette
 * @param album_name Album name for portrait directory (e.g., "Default")
 * @return ESP_OK on success, error code on failure
 */
esp_err_t image_processor_convert_jpg_to_bmp_with_portrait_combine(const char *jpg_path,
                                                                   const char *bmp_path,
                                                                   bool use_stock_mode,
                                                                   const char *album_name);
/**
 * @brief Convert JPG image to BMP format (from file)
 */
esp_err_t image_processor_convert_jpg_to_bmp(const char *jpg_path, const char *bmp_path,
                                             bool use_stock_mode);

/**
 * @brief Convert JPG image to BMP format (from RAM buffer) - OPTIMIZED VERSION
 *
 * This function converts a JPG image from a RAM buffer directly to BMP without
 * writing an intermediate file to SD card. The jpg_buffer will NOT be freed by
 * this function - caller must free it after calling.
 *
 * @param jpg_buffer Pointer to JPG data in RAM (caller owns, caller must free)
 * @param jpg_size Size of JPG data in bytes
 * @param bmp_path Output BMP file path on SD card
 * @param use_stock_mode true = use theoretical palette, false = use measured palette
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t image_processor_convert_buffer_to_bmp(uint8_t *jpg_buffer, size_t jpg_size,
                                                const char *bmp_path, bool use_stock_mode,
                                                bool rotate_portrait);

/**
 * @brief Convert JPG to BMP with portrait combine (from RAM buffer) - OPTIMIZED
 *
 * @param jpg_buffer Pointer to JPG data in RAM (caller must free after call)
 * @param jpg_size Size of JPG data in bytes
 * @param bmp_path Output BMP file path
 * @param use_stock_mode true = theoretical palette, false = measured palette
 * @param album_name Album name for portrait directory
 * @return ESP_OK on success
 */
esp_err_t image_processor_convert_buffer_to_bmp_with_portrait_combine(uint8_t *jpg_buffer,
                                                                      size_t jpg_size,
                                                                      const char *bmp_path,
                                                                      bool use_stock_mode,
                                                                      const char *album_name);

#endif