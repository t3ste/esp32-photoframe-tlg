set(SOURCES 
    "album_manager.c"
    "color_palette.c"
    "config_manager.c"
    "display_manager.c"
    "dns_server.c"
    "http_server.c"
    "image_processor.c"
    "main.c"
    "mdns_service.c"
    "power_manager.c"
    "processing_settings.c"
    "utils.c"
    "wifi_manager.c"
    "wifi_provisioning.c"
	"telegram_bot.c"          # <- Telegram v1.9.0_tlg
	"api_handlers.c"          # <- Telegram v1.9.0_tlg
	"battery_manager.c"  	  # BAT CHECK
)

set(INCLUDE_DIRS ".")

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_sdmmc nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_http_client fatfs espressif__cjson espressif__esp_jpeg ListLib axpPower i2c_bsp espressif__mdns
    PRIV_REQUIRES epaper_port epaper_src sdcard_bsp esp_timer esp_app_format esp_http_client mbedtls
    EMBED_FILES "webapp/index.html" "webapp/style.css" "webapp/app.js" "webapp/image-processor.js" "webapp/favicon.svg" "webapp/provision.html" "webapp/measurement_sample.jpg" "resources/calibration.bmp"
)
#v1.9.0_tlg idf.py -p <PORT> flash monitor --no-reset
#v1.9.0_tlg REQUIRES esp_http_client <-- For Telegram HTTPS Requests
#v1.9.0_tlg PRIV_REQUIRES mbedtls <-- For Telegram CA Root Certificates f�r HTTPS
#v1.9.0_tlg set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/components/esp-tls)

add_custom_target(show_features ALL
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "========================================================="
    COMMAND ${CMAKE_COMMAND} -E echo " Extended version 1.9.0 with Telegram features of"
    COMMAND ${CMAKE_COMMAND} -E echo " https://aitjcize.github.io/esp32-photoframe/"	
    COMMAND ${CMAKE_COMMAND} -E echo " ESP32-S3 PhotoFrame Build finished"
    COMMAND ${CMAKE_COMMAND} -E echo "========================================================="
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "======= FEATURE STATUS [Y] tested / [ ] untested  ======="
    COMMAND ${CMAKE_COMMAND} -E echo "Implemented (Telegram / Display / Power):"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] Telegram bot REST-API cmds + /help"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] Wake-up Telegram notify (battery, preview, pre-sleep)"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] Optional wake-up telegram command window"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] History mgmt (show/delete) >10 img ons SDs needed!"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] Text overlay (caption or message /txt)"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] Fetch image from chat (BMP + thumbnail)"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] displayed image Preview + filename to Telegram"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] WiFi is stopped before the display update (saves energy)"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] Fallback: gradual increase of init timings"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] Telegram token/chat ID via Web-UI (NVS)"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] Optional Portrait pairing to landscape BMP (auto combine)"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] Telegram ACK on download image"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] Telegram instant show uploaded image as #show command"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] Initial Read WiFi credentials file from SD card"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] Build Option to disable logging"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] Duplicate check without mapping file (file unique ID)"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] Option to disable Serial Logging"
    COMMAND ${CMAKE_COMMAND} -E echo "  [Y] local script for Batch and Combine portrait mode"
    COMMAND ${CMAKE_COMMAND} -E echo "  [ ] WiFi reset via BOOT press (10s) when device active"
    COMMAND ${CMAKE_COMMAND} -E echo "  [ ] Read WiFi credentials file from SD card via BOOT press (2s)"
    COMMAND ${CMAKE_COMMAND} -E echo "  [ ] Random non-repeating image rotation (md5 up to 1000)"
    COMMAND ${CMAKE_COMMAND} -E echo "  [ ] Buffer Download with SD-Card Fallback"
    COMMAND ${CMAKE_COMMAND} -E echo "  [ ] Battery <20%: text overlay"
    COMMAND ${CMAKE_COMMAND} -E echo "  [ ] Battery <20%: notify tlg message + auto interval increase"
    COMMAND ${CMAKE_COMMAND} -E echo "  [ ] Battery <20%: notify tlg message + auto interval increase"
    COMMAND ${CMAKE_COMMAND} -E echo "  [ ] URL rotation mode from v1.9.0"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Planned Features / ToDo:"
    COMMAND ${CMAKE_COMMAND} -E echo "	WEBPAGE:"
    COMMAND ${CMAKE_COMMAND} -E echo "  - Show, Edit Settings on Webpage"
    COMMAND ${CMAKE_COMMAND} -E echo "  - Batch and combine portrait mode on webpage Image Upload if usb connected"	
    COMMAND ${CMAKE_COMMAND} -E echo "  - Set rotation url via webpage"
    COMMAND ${CMAKE_COMMAND} -E echo "	TELEGRAM:"
    COMMAND ${CMAKE_COMMAND} -E echo "  - set telegram token and chat id via txt on sd card"
    COMMAND ${CMAKE_COMMAND} -E echo "  - Allow only one show / display command on wake up session"
    COMMAND ${CMAKE_COMMAND} -E echo "  - Handle portrait on show / display command on wake up session"
    COMMAND ${CMAKE_COMMAND} -E echo "  - Improved text overlay (layout, style, charset)"
    COMMAND ${CMAKE_COMMAND} -E echo "  - Text overlay mgmt on webpage (add/delete/change)"
    COMMAND ${CMAKE_COMMAND} -E echo "  - Better Telegram img filenames (user+date)"
    COMMAND ${CMAKE_COMMAND} -E echo "  - Trigger next random img via telegram command"
    COMMAND ${CMAKE_COMMAND} -E echo "  - Analyze the behavior of multi (batch) uploads of multiple images in a single message."
    COMMAND ${CMAKE_COMMAND} -E echo "	OTHERS:"
    COMMAND ${CMAKE_COMMAND} -E echo "  - Options for HTTP image download"
    COMMAND ${CMAKE_COMMAND} -E echo "  - Split very long rotation intervals"
    COMMAND ${CMAKE_COMMAND} -E echo "  - Duplicate check with mapping file (without file unique ID)?"
    COMMAND ${CMAKE_COMMAND} -E echo "  - Optional logging on SD-Card with webpage log viewer"
    COMMAND ${CMAKE_COMMAND} -E echo "  - No Image Upload: disbale Enable pre-shared-key ciphersuites"
    COMMAND ${CMAKE_COMMAND} -E echo "  - No Image Upload: disbale mbedTLS allocation from SPIRAM"
    COMMAND ${CMAKE_COMMAND} -E echo "  - recognize momocrome images to use a different conversion algorithm"
    COMMAND ${CMAKE_COMMAND} -E echo "	BIG ONES:"
    COMMAND ${CMAKE_COMMAND} -E echo "  - OTA firmware update (USB-powered), if possible"
    COMMAND ${CMAKE_COMMAND} -E echo "  - 4 preset modes (USB/battery, full/eco/custom)"
    COMMAND ${CMAKE_COMMAND} -E echo "  - RTC wake/alarms, RTC battery status check"
    COMMAND ${CMAKE_COMMAND} -E echo "  - show image on date and time"
    COMMAND ${CMAKE_COMMAND} -E echo "  - Optional BLE wake-up"
    COMMAND ${CMAKE_COMMAND} -E echo "  - Optional speaker & temp/humidity features"
    COMMAND ${CMAKE_COMMAND} -E echo "  - Optional Weather kiosk mode (if usb connected)"
    COMMAND ${CMAKE_COMMAND} -E echo "  - Streaming decode, if possible"
    COMMAND ${CMAKE_COMMAND} -E echo "================================================"
)

add_dependencies(show_features app)
