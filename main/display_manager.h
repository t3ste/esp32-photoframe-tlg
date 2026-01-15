#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t display_manager_init(void);
esp_err_t display_manager_show_image(const char *filename);
esp_err_t display_manager_clear(void);
bool display_manager_is_busy(void);

void display_manager_set_rotate_interval(int seconds);
int display_manager_get_rotate_interval(void);
void display_manager_set_auto_rotate(bool enabled);
bool display_manager_get_auto_rotate(void);
void display_manager_handle_wakeup(void);

// v1.9.0_tlg Telegram START
esp_err_t display_manager_get_current_image(char *buffer, size_t buffer_size);

// Image history management
int display_manager_get_history_count(void);
void display_manager_clear_history(void);

esp_err_t display_manager_show_image_with_text(const char *filename, const char *text);
// Text overlay management
esp_err_t display_manager_save_overlay_text(const char *bmp_path, const char *text);
// v1.9.0_tlg Telegram END

/**
 * @brief Get the next image that would be displayed (without displaying it)
 */
esp_err_t display_manager_get_next_image(char *imagepath, size_t pathsize);

#endif
