// new file for v1.9.0_tlg
#ifndef TELEGRAM_BOT_H
#define TELEGRAM_BOT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define TELEGRAM_TOKEN_MAX_LEN 64
#define TELEGRAM_API_BASE "api.telegram.org"

/*
telegram_bot_check_updates()
  ↓
[Parse JSON: photo message detected]
  ↓
process_photo_message(cJSON *message)
  ↓
[Extract largest photo file_id]
  ↓
process_single_photo(file_id, ...)
  ↓
┌─────────────────────────────────────┐
│ 1. getFile API Call                 │
│    → Get file_path from Telegram    │
└─────────────────────────────────────┘
  ↓
┌─────────────────────────────────────┐
│ 2. download_to_ram(file_path)       │
│    → HTTP GET → ram_buffer          │
└─────────────────────────────────────┘
  ↓
  ├─ SUCCESS ✅
  │   ↓
  │ ┌───────────────────────────────────────────────────┐
  │ │ 3. image_processor_convert_buffer_to_bmp_         │
  │ │    with_portrait_combine(ram_buffer, ...)         │
  │ └───────────────────────────────────────────────────┘
  │   ↓
  │ ┌───────────────────────────────────┐
  │ │ detect_jpeg_dimensions(buffer)    │
  │ │   → is_portrait?                  │
  │ └───────────────────────────────────┘
  │   ↓
  │   ├─ NO (Landscape) → image_processor_convert_buffer_to_bmp()
  │   │                      ↓
  │   │                    esp_jpeg_decode(buffer)
  │   │                      ↓
  │   │                    resize_image()
  │   │                      ↓
  │   │                    apply_floyd_steinberg_dither()
  │   │                      ↓
  │   │                    write_bmp_file()
  │   │
  │   └─ YES (Portrait) → Save to portrait/ dir
  │                         ↓
  │                       find_portrait_bmp()
  │                         ↓
  │                         ├─ NO pair → Wait for next image
  │                         └─ YES pair → combine_portrait_bmps()
  │   ↓
  │ free(ram_buffer) ✅
  │   ↓
  │ [DONE - optimized path]
  │
  └─ FAIL ❌
      ↓
    ┌─────────────────────────────────────────────┐
    │ FALLBACK: download_and_save_telegram_file() │
    │           → Save to SD: temp.jpg            │
    └─────────────────────────────────────────────┘
      ↓
    ┌──────────────────────────────────────────────────┐
    │ image_processor_convert_jpg_to_bmp_              │
    │ with_portrait_combine(temp.jpg, ...)             │
    └──────────────────────────────────────────────────┘
      ↓
    [fopen() → fread() → esp_jpeg_decode() → ...]
      ↓
    unlink(temp.jpg)
      ↓
    [DONE - fallback path]
*/

/**
 * @brief Check if a photo send operation is in progress
 * @return true if photo is being sent, false otherwise
 */
bool telegram_bot_is_sending_photo(void);

esp_err_t telegram_bot_init(void);
esp_err_t telegram_bot_set_token(const char *token);
esp_err_t telegram_bot_get_token(char *token, size_t len);
bool telegram_bot_has_token(void);
// esp_err_t telegram_bot_check_updates(void);
bool telegram_bot_check_updates(void);

// Nachricht an Chat senden
esp_err_t telegram_bot_send_message(int64_t chat_id, const char *text);

void telegram_bot_stop_polling(void);

// Send photo to chat
esp_err_t telegram_bot_send_photo(int64_t chat_id, const char *photo_path, const char *caption);
// esp_err_t telegram_bot_send_photo_async(int64_t chat_id, const char *photo_path, const char
// *caption);

// Notify when display is updated (sends message + thumbnail)
esp_err_t telegram_bot_notify_display_update(int64_t chat_id, const char *image_path);

// Get last chat ID (for notifications)
int64_t telegram_bot_get_last_chat_id(void);

// Send wake-up notification with battery status
esp_err_t telegram_bot_notify_wakeup(int64_t chat_id);

// Settings for timer wakeup behavior
bool telegram_bot_get_check_on_timer_wakeup(void);
esp_err_t telegram_bot_set_check_on_timer_wakeup(bool enabled);
bool telegram_bot_get_notify_on_timer_wakeup(void);
esp_err_t telegram_bot_set_notify_on_timer_wakeup(bool enabled);

// Set/Get configured chat ID
esp_err_t telegram_bot_set_chat_id(int64_t chat_id);
esp_err_t telegram_bot_get_chat_id(int64_t *chat_id);
bool telegram_bot_has_chat_id(void);

// Send sleep notification with battery status
esp_err_t telegram_bot_notify_sleep(int64_t chat_id);
bool telegram_bot_get_notify_on_sleep(void);
esp_err_t telegram_bot_set_notify_on_sleep(bool enabled);

// Display update notification settings
bool telegram_bot_get_notify_on_display_update(void);
esp_err_t telegram_bot_set_notify_on_display_update(bool enabled);

#endif  // TELEGRAM_BOT_H