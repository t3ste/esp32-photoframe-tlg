#include "image_processor.h"

#include <dirent.h>  // for opendir/readdir
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>  // for stat/mkdir
#include <time.h>      // for time() timestamp
#include <unistd.h>    // for unlink()

#include "color_palette.h"
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "jpeg_decoder.h"
#include "nvs.h"        // for NVS-Access
#include "nvs_flash.h"  // for NVS-Access

static const char *TAG = "image_processor";

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

// Theoretical palette - used for BMP output (firmware compatibility)
static const rgb_t palette[7] = {
    {0, 0, 0},        // Black
    {255, 255, 255},  // White
    {255, 255, 0},    // Yellow
    {255, 0, 0},      // Red
    {0, 0, 0},        // Reserved
    {0, 0, 255},      // Blue
    {0, 255, 0}       // Green
};

// Measured palette - loaded from NVS or defaults
static rgb_t palette_measured[7] = {
    {2, 2, 2},        // Black (default)
    {190, 190, 190},  // White (default)
    {205, 202, 0},    // Yellow (default)
    {135, 19, 0},     // Red (default)
    {0, 0, 0},        // Reserved
    {5, 64, 158},     // Blue (default)
    {39, 102, 60}     // Green (default)
};

static void load_calibrated_palette(void)
{
    color_palette_t cal_palette;
    if (color_palette_load(&cal_palette) == ESP_OK) {
        palette_measured[0] =
            (rgb_t) {cal_palette.black.r, cal_palette.black.g, cal_palette.black.b};
        palette_measured[1] =
            (rgb_t) {cal_palette.white.r, cal_palette.white.g, cal_palette.white.b};
        palette_measured[2] =
            (rgb_t) {cal_palette.yellow.r, cal_palette.yellow.g, cal_palette.yellow.b};
        palette_measured[3] = (rgb_t) {cal_palette.red.r, cal_palette.red.g, cal_palette.red.b};
        palette_measured[5] = (rgb_t) {cal_palette.blue.r, cal_palette.blue.g, cal_palette.blue.b};
        palette_measured[6] =
            (rgb_t) {cal_palette.green.r, cal_palette.green.g, cal_palette.green.b};
        ESP_LOGI(TAG, "Loaded calibrated color palette from NVS");
    } else {
        ESP_LOGI(TAG, "Using default color palette");
    }
}

static int find_closest_color(uint8_t r, uint8_t g, uint8_t b, const rgb_t *pal)
{
    int min_dist = INT_MAX;
    int closest = 1;

    for (int i = 0; i < 7; i++) {
        if (i == 4)  // Skip reserved index!
            continue;

        int dr = r - pal[i].r;
        int dg = g - pal[i].g;
        int db = b - pal[i].b;
        int dist = dr * dr + dg * dg + db * db;

        if (dist < min_dist) {
            min_dist = dist;
            closest = i;
        }
    }

    return closest;
}

static void apply_floyd_steinberg_dither(uint8_t *image, int width, int height,
                                         const rgb_t *dither_palette)
{
    // Use two scanlines for error diffusion (current and next row)
    // This reduces memory from ~4.6MB to ~10KB for 800x480
    int *curr_errors = (int *) heap_caps_calloc(width * 3, sizeof(int), MALLOC_CAP_SPIRAM);
    int *next_errors = (int *) heap_caps_calloc(width * 3, sizeof(int), MALLOC_CAP_SPIRAM);

    if (!curr_errors || !next_errors) {
        ESP_LOGE(TAG, "Failed to allocate error buffers");
        if (curr_errors)
            free(curr_errors);
        if (next_errors)
            free(next_errors);
        return;
    }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int img_idx = (y * width + x) * 3;
            int err_idx = x * 3;

            int old_r = image[img_idx] + curr_errors[err_idx];
            int old_g = image[img_idx + 1] + curr_errors[err_idx + 1];
            int old_b = image[img_idx + 2] + curr_errors[err_idx + 2];

            old_r = (old_r < 0) ? 0 : (old_r > 255) ? 255 : old_r;
            old_g = (old_g < 0) ? 0 : (old_g > 255) ? 255 : old_g;
            old_b = (old_b < 0) ? 0 : (old_b > 255) ? 255 : old_b;

            // Find closest color using specified dither palette
            int color_idx = find_closest_color(old_r, old_g, old_b, dither_palette);

            // Output using theoretical palette (for BMP/firmware compatibility)
            image[img_idx] = palette[color_idx].r;
            image[img_idx + 1] = palette[color_idx].g;
            image[img_idx + 2] = palette[color_idx].b;

            // Calculate error using specified dither palette (for error diffusion)
            int err_r = old_r - dither_palette[color_idx].r;
            int err_g = old_g - dither_palette[color_idx].g;
            int err_b = old_b - dither_palette[color_idx].b;

            // Distribute error to neighboring pixels
            if (x + 1 < width) {
                // Right pixel (current row)
                curr_errors[(x + 1) * 3] += err_r * 7 / 16;
                curr_errors[(x + 1) * 3 + 1] += err_g * 7 / 16;
                curr_errors[(x + 1) * 3 + 2] += err_b * 7 / 16;
            }

            if (y + 1 < height) {
                // Bottom-left pixel (next row)
                if (x > 0) {
                    next_errors[(x - 1) * 3] += err_r * 3 / 16;
                    next_errors[(x - 1) * 3 + 1] += err_g * 3 / 16;
                    next_errors[(x - 1) * 3 + 2] += err_b * 3 / 16;
                }

                // Bottom pixel (next row)
                next_errors[x * 3] += err_r * 5 / 16;
                next_errors[x * 3 + 1] += err_g * 5 / 16;
                next_errors[x * 3 + 2] += err_b * 5 / 16;

                // Bottom-right pixel (next row)
                if (x + 1 < width) {
                    next_errors[(x + 1) * 3] += err_r * 1 / 16;
                    next_errors[(x + 1) * 3 + 1] += err_g * 1 / 16;
                    next_errors[(x + 1) * 3 + 2] += err_b * 1 / 16;
                }
            }
        }

        // Swap error buffers for next row
        int *temp = curr_errors;
        curr_errors = next_errors;
        next_errors = temp;
        memset(next_errors, 0, width * 3 * sizeof(int));
    }

    free(curr_errors);
    free(next_errors);
}

// ===== Forward Declarations =====
static int find_closest_color(uint8_t r, uint8_t g, uint8_t b, const rgb_t *pal);
static void apply_floyd_steinberg_dither(uint8_t *image, int width, int height,
                                         const rgb_t *dither_palette);
static uint8_t *resize_image(uint8_t *src, int src_w, int src_h, int dst_w, int dst_h);
static esp_err_t write_bmp_file(const char *filename, uint8_t *rgb_data, int width,
                                int height);

// ===== Portrait-Combination - configuration =====
static bool portrait_combine_enabled = true;  // Default: true
#define PORTRAIT_DIR_NAME "portrait"
#define NVS_PORTRAIT_COMBINE_KEY "portrait_comb"

// ===== Portrait-Combination - NVS Getter/Setter =====
esp_err_t image_processor_set_portrait_combine_enabled(bool enabled)
{
    portrait_combine_enabled = enabled;
    ESP_LOGI(TAG, "Portrait combine: %s", enabled ? "ENABLED" : "DISABLED");

    // Save to NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_PORTRAIT_COMBINE_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Portrait combine feature %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

bool image_processor_get_portrait_combine_enabled(void)
{
    return portrait_combine_enabled;
}

// ===== Portrait-Combination - Helper-Functions =====

/**
 * @brief Detect if image is portrait format by reading JPEG dimensions
 * @param jpg_buffer JPEG data buffer
 * @param jpg_size Size of JPEG data
 * @param width Output: image width
 * @param height Output: image height
 * @return ESP_OK on success
 */
static esp_err_t detect_jpeg_dimensions(const uint8_t *jpg_buffer, size_t jpg_size, int *width,
                                        int *height)
{
    esp_jpeg_image_cfg_t jpeg_cfg = {.indata = (uint8_t *) jpg_buffer,
                                     .indata_size = jpg_size,
                                     .outbuf = NULL,
                                     .outbuf_size = 0,
                                     .out_format = JPEG_IMAGE_FORMAT_RGB888,
                                     .out_scale = JPEG_IMAGE_SCALE_0,
                                     .flags = {.swap_color_bytes = 0}};

    esp_jpeg_image_output_t outimg;
    esp_err_t ret = esp_jpeg_get_image_info(&jpeg_cfg, &outimg);
    if (ret != ESP_OK) {
        return ret;
    }

    *width = outimg.width;
    *height = outimg.height;
    return ESP_OK;
}

/**
 * @brief Read BMP file header to get dimensions
 * @param bmp_path Path to BMP file
 * @param width Output: BMP width
 * @param height Output: BMP height
 * @return ESP_OK on success
 */
static esp_err_t read_bmp_dimensions(const char *bmp_path, int *width, int *height)
{
    FILE *fp = fopen(bmp_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "fopen failed: %s", bmp_path);
        return ESP_FAIL;
    }

    // BMP header: offset 18-21 = width, 22-25 = height (little-endian)
    uint8_t header[54];
    if (fread(header, 1, 54, fp) != 54) {
        ESP_LOGE(TAG, "fread failed: %s", bmp_path);
        fclose(fp);
        return ESP_FAIL;
    }
    fclose(fp);

    // Check BMP signature
    if (header[0] != 'B' || header[1] != 'M') {
        ESP_LOGE(TAG, "Invalid BMP: %s (sig: %c%c)", bmp_path, header[0], header[1]);
        return ESP_FAIL;
    }

    *width = header[18] | (header[19] << 8) | (header[20] << 16) | (header[21] << 24);
    *height = header[22] | (header[23] << 8) | (header[24] << 16) | (header[25] << 24);
    ESP_LOGI(TAG, "BMP OK: %dx%d - %s", *width, *height, bmp_path);
    return ESP_OK;
}

/**
 * @brief Find another portrait BMP in the portrait directory
 * @param portrait_dir Portrait directory path
 * @param exclude_file Filename to exclude (current file)
 * @param found_path Output buffer for found file path
 * @param found_path_size Size of output buffer
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no match
 */
static esp_err_t find_portrait_bmp(const char *portrait_dir, const char *exclude_file,
                                   char *found_path, size_t found_path_size)
{
    ESP_LOGI(TAG, "Searching for portrait in: %s (excluding: %s)", portrait_dir,
             exclude_file ? exclude_file : "none");

    DIR *dir = opendir(portrait_dir);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", portrait_dir);
        return ESP_ERR_NOT_FOUND;
    }

    int file_count = 0;
    int bmp_count = 0;
    int portrait_count = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        file_count++;

        if (entry->d_type != DT_REG) {
            ESP_LOGD(TAG, "Skipping non-file: %s", entry->d_name);
            continue;
        }

        // Skip excluded file
        if (exclude_file && strcmp(entry->d_name, exclude_file) == 0) {
            ESP_LOGI(TAG, "Skipping excluded file: %s", entry->d_name);
            continue;
        }

        // Check .bmp extension
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || (strcasecmp(ext, ".bmp") != 0)) {
            ESP_LOGD(TAG, "Skipping non-BMP: %s", entry->d_name);
            continue;
        }

        bmp_count++;
        ESP_LOGI(TAG, "Checking BMP file: %s", entry->d_name);

        // Build full path
        snprintf(found_path, found_path_size, "%s/%s", portrait_dir, entry->d_name);

        // Check file exists and has size
        struct stat st;
        if (stat(found_path, &st) != 0) {
            ESP_LOGW(TAG, "stat() failed for: %s", found_path);
            continue;
        }

        // Skip empty marker files (used for duplicate detection after combination)
        if (st.st_size == 0) {
            ESP_LOGD(TAG, "Skipping empty marker file: %s (already combined)", entry->d_name);
            continue;
        }

        ESP_LOGI(TAG, "File size: %ld bytes", st.st_size);

        // Verify it's portrait format
        int width, height;
        esp_err_t ret = read_bmp_dimensions(found_path, &width, &height);

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "read_bmp_dimensions() failed for %s: %s", entry->d_name,
                     esp_err_to_name(ret));
            continue;
        }

        ESP_LOGI(TAG, "BMP dimensions: %dx%d (w×h)", width, height);

        // if (height > width) {
        // if (height > width || (width == 800 && height == 480)) {
        if (height > width && width <= 800 && height <= 480) {
            portrait_count++;
            closedir(dir);
            ESP_LOGI(TAG, " Found portrait BMP: %s (%dx%d)", entry->d_name, width, height);
            return ESP_OK;
        } else if (width == DISPLAY_WIDTH && height == DISPLAY_HEIGHT) {
            // rotated  portrait (now landscape)
            portrait_count++;
            closedir(dir);
            ESP_LOGI(TAG, "Found rotated portrait: %s (%dx%d)", entry->d_name, width, height);
            return ESP_OK;
        }
    }

    closedir(dir);

    ESP_LOGW(TAG, "Search summary: %d files, %d BMPs, %d portraits", file_count, bmp_count,
             portrait_count);
    ESP_LOGW(TAG, "No suitable portrait BMP found");

    return ESP_ERR_NOT_FOUND;
}

/**
 * @brief Read BMP file into RGB buffer
 * @param bmp_path Path to BMP file
 * @param rgb_buffer Output buffer (must be pre-allocated: width * height * 3)
 * @param width Expected width
 * @param height Expected height
 * @return ESP_OK on success
 */
static esp_err_t read_bmp_to_rgb(const char *bmp_path, uint8_t *rgb_buffer, int width, int height)
{
    FILE *fp = fopen(bmp_path, "rb");
    if (!fp) {
        return ESP_FAIL;
    }

    // Skip BMP header (54 bytes)
    fseek(fp, 54, SEEK_SET);

    // BMP stores rows bottom-to-top, BGR format, with padding
    int row_size = ((width * 3 + 3) / 4) * 4;
    uint8_t *row_buffer = malloc(row_size);
    if (!row_buffer) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    // Read rows bottom-to-top, convert BGR->RGB
    for (int y = height - 1; y >= 0; y--) {
        if (fread(row_buffer, 1, row_size, fp) != row_size) {
            free(row_buffer);
            fclose(fp);
            return ESP_FAIL;
        }

        for (int x = 0; x < width; x++) {
            int dst_idx = (y * width + x) * 3;
            rgb_buffer[dst_idx] = row_buffer[x * 3 + 2];      // R
            rgb_buffer[dst_idx + 1] = row_buffer[x * 3 + 1];  // G
            rgb_buffer[dst_idx + 2] = row_buffer[x * 3];      // B
        }
    }

    free(row_buffer);
    fclose(fp);
    return ESP_OK;
}

/**
 * @brief Convert JPG buffer to 24-bit RGB BMP WITHOUT dithering (for portrait combine)
 * @param jpg_buffer JPEG data buffer
 * @param jpg_size Size of JPEG data
 * @param bmp_path Output BMP path (24-bit RGB)
 * @param rotate_portrait true to rotate 90°, false to keep portrait
 * @return ESP_OK on success
 */
static esp_err_t convert_buffer_to_rgb_bmp(uint8_t *jpg_buffer, size_t jpg_size,
                                           const char *bmp_path, bool rotate_portrait)
{
    ESP_LOGI(TAG, "Converting %zu bytes JPG to 24-bit RGB BMP (no dithering)", jpg_size);

    // Get JPEG info
    esp_jpeg_image_cfg_t jpeg_cfg = {.indata = jpg_buffer,
                                     .indata_size = jpg_size,
                                     .outbuf = NULL,
                                     .outbuf_size = 0,
                                     .out_format = JPEG_IMAGE_FORMAT_RGB888,
                                     .out_scale = JPEG_IMAGE_SCALE_0,
                                     .flags = {.swap_color_bytes = 0}};

    esp_jpeg_image_output_t out_img;
    esp_err_t ret = esp_jpeg_get_image_info(&jpeg_cfg, &out_img);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get JPEG info: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "JPEG info: %dx%d", out_img.width, out_img.height);

    // Allocate RGB buffer
    uint8_t *rgb_buffer = (uint8_t *) heap_caps_malloc(out_img.output_len, MALLOC_CAP_SPIRAM);
    if (!rgb_buffer) {
        ESP_LOGE(TAG, "Failed to allocate RGB buffer: %zu bytes", out_img.output_len);
        return ESP_ERR_NO_MEM;
    }

    // Decode JPEG
    jpeg_cfg.outbuf = rgb_buffer;
    jpeg_cfg.outbuf_size = out_img.output_len;
    ret = esp_jpeg_decode(&jpeg_cfg, &out_img);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(ret));
        free(rgb_buffer);
        return ret;
    }

    ESP_LOGI(TAG, "Successfully decoded JPEG: %dx%d", out_img.width, out_img.height);

    uint8_t *final_image = rgb_buffer;
    int final_width = out_img.width;
    int final_height = out_img.height;

    // Resize to 400×480 (half display width for portrait combine)
    int target_width = DISPLAY_WIDTH / 2;  // 400
    int target_height = DISPLAY_HEIGHT;    // 480

    if (final_width != target_width || final_height != target_height) {
        ESP_LOGI(TAG, "Resizing %dx%d -> %dx%d", final_width, final_height, target_width,
                 target_height);
        uint8_t *resized =
            resize_image(final_image, final_width, final_height, target_width, target_height);
        if (!resized) {
            ESP_LOGE(TAG, "Failed to resize image");
            free(rgb_buffer);
            return ESP_FAIL;
        }
        free(rgb_buffer);
        final_image = resized;
        final_width = target_width;
        final_height = target_height;
    }

    // Rotate if requested (not used for portrait combine)
    if (rotate_portrait) {
        ESP_LOGI(TAG, "Rotating 90° clockwise");
        size_t rotated_size = final_width * final_height * 3;
        uint8_t *rotated = (uint8_t *) heap_caps_malloc(rotated_size, MALLOC_CAP_SPIRAM);
        if (!rotated) {
            ESP_LOGE(TAG, "Failed to allocate rotation buffer");
            free(final_image);
            return ESP_ERR_NO_MEM;
        }

        // Rotate 90° clockwise
        for (int y = 0; y < final_height; y++) {
            for (int x = 0; x < final_width; x++) {
                int src_idx = (y * final_width + x) * 3;
                int dst_x = final_height - 1 - y;
                int dst_y = x;
                int dst_idx = (dst_y * final_height + dst_x) * 3;
                rotated[dst_idx] = final_image[src_idx];
                rotated[dst_idx + 1] = final_image[src_idx + 1];
                rotated[dst_idx + 2] = final_image[src_idx + 2];
            }
        }

        free(final_image);
        final_image = rotated;

        // Swap dimensions
        int temp = final_width;
        final_width = final_height;
        final_height = temp;
    }

    // Write 24-bit RGB BMP (NO DITHERING!)
    ESP_LOGI(TAG, "Writing 24-bit RGB BMP: %s (%dx%d)", bmp_path, final_width, final_height);
    ret = write_bmp_file(bmp_path, final_image, final_width, final_height);

    free(final_image);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Successfully saved 24-bit RGB BMP (no dithering): %s", bmp_path);
    } else {
        ESP_LOGE(TAG, "Failed to write BMP file");
    }

    return ret;
}


/**
 * @brief Combine two portrait BMPs side-by-side into DISPLAY_WIDTH×DISPLAY_HEIGHT BMP
 */
static esp_err_t combine_portrait_bmps(const char *bmp1_path, const char *bmp2_path,
                                       const char *output_path)
{
    ESP_LOGI(TAG, "Combining portraits: %s + %s -> %s", bmp1_path, bmp2_path, output_path);

    // Read dimensions of both images
    int w1, h1, w2, h2;
    if (read_bmp_dimensions(bmp1_path, &w1, &h1) != ESP_OK ||
        read_bmp_dimensions(bmp2_path, &w2, &h2) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read BMP dimensions");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BMP1: %dx%d, BMP2: %dx%d", w1, h1, w2, h2);

    // Read both BMPs into memory
    uint8_t *rgb1 = heap_caps_malloc(w1 * h1 * 3, MALLOC_CAP_SPIRAM);
    uint8_t *rgb2 = heap_caps_malloc(w2 * h2 * 3, MALLOC_CAP_SPIRAM);

    if (!rgb1 || !rgb2) {
        ESP_LOGE(TAG, "Failed to allocate temp buffers");
        if (rgb1)
            free(rgb1);
        if (rgb2)
            free(rgb2);
        return ESP_ERR_NO_MEM;
    }

    if (read_bmp_to_rgb(bmp1_path, rgb1, w1, h1) != ESP_OK ||
        read_bmp_to_rgb(bmp2_path, rgb2, w2, h2) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read BMP data");
        free(rgb1);
        free(rgb2);
        return ESP_FAIL;
    }

    // Resize both portraits to half display width (400×480)
    ESP_LOGI(TAG, "Resizing both portraits to fit %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    int half_width = DISPLAY_WIDTH / 2;   // 400
    int display_height = DISPLAY_HEIGHT;  // 480

    uint8_t *resized1 = resize_image(rgb1, w1, h1, half_width, display_height);
    uint8_t *resized2 = resize_image(rgb2, w2, h2, half_width, display_height);

    free(rgb1);
    free(rgb2);

    if (!resized1 || !resized2) {
        ESP_LOGE(TAG, "Failed to resize portraits");
        if (resized1)
            free(resized1);
        if (resized2)
            free(resized2);
        return ESP_FAIL;
    }

    // Allocate final combined buffer (800×480)
    uint8_t *combined_rgb = heap_caps_malloc(DISPLAY_WIDTH * DISPLAY_HEIGHT * 3, MALLOC_CAP_SPIRAM);
    if (!combined_rgb) {
        ESP_LOGE(TAG, "Failed to allocate final buffer");
        free(resized1);
        free(resized2);
        return ESP_ERR_NO_MEM;
    }

    // Combine: left half = resized1, right half = resized2
    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
        // Left half (0-399)
        memcpy(&combined_rgb[y * DISPLAY_WIDTH * 3], &resized1[y * half_width * 3], half_width * 3);

        // Right half (400-799)
        memcpy(&combined_rgb[(y * DISPLAY_WIDTH + half_width) * 3], &resized2[y * half_width * 3],
               half_width * 3);
    }

    free(resized1);
    free(resized2);

    // *** CRITICAL: Apply dithering to combined image ***
    ESP_LOGI(TAG, "Applying dithering to combined portrait...");
    apply_floyd_steinberg_dither(combined_rgb, DISPLAY_WIDTH, DISPLAY_HEIGHT, palette_measured);

    // Write final 800×480 BMP
    // esp_err_t ret = write_bmp_file(output_path, combined_rgb, DISPLAY_HEIGHT);
    esp_err_t ret = write_bmp_file(output_path, combined_rgb, DISPLAY_WIDTH, DISPLAY_HEIGHT);

    // Thumbnail handling (existing code)
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Combined BMP saved: %s", output_path);

        // Create thumbnail path
        char thumb_path[256];
        strncpy(thumb_path, output_path, sizeof(thumb_path) - 1);
        thumb_path[sizeof(thumb_path) - 1] = '\0';

        char *ext = strstr(thumb_path, ".bmp");
        if (ext) {
            strcpy(ext, "_thumb.jpg");
        } else {
            strncat(thumb_path, "_thumb.jpg", sizeof(thumb_path) - strlen(thumb_path) - 1);
        }

        // Find existing thumbnail from first portrait
        const char *filename1 = strrchr(bmp1_path, '/');
        if (filename1) {
            filename1++;
        } else {
            filename1 = bmp1_path;
        }

        char basename1[64];
        strncpy(basename1, filename1, sizeof(basename1) - 1);
        basename1[sizeof(basename1) - 1] = '\0';

        char *bmp_ext = strstr(basename1, ".bmp");
        if (bmp_ext) {
            *bmp_ext = '\0';
        }

        // Build path to existing thumbnail
        const char *album_start = strstr(output_path, "images/") + 7;
        const char *album_end = strrchr(output_path, '/');
        int album_len = album_end - album_start;

        char source_thumb[256];
        snprintf(source_thumb, sizeof(source_thumb), "/sdcard/images/%.*s/%s.jpg", album_len,
                 album_start, basename1);

        ESP_LOGI(TAG, "Looking for source thumbnail: %s", source_thumb);

        struct stat st;
        if (stat(source_thumb, &st) == 0) {
            FILE *src = fopen(source_thumb, "rb");
            FILE *dst = fopen(thumb_path, "wb");

            if (src && dst) {
                uint8_t buffer[1024];
                size_t bytes;
                while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
                    fwrite(buffer, 1, bytes, dst);
                }
                fclose(src);
                fclose(dst);
                ESP_LOGI(TAG, "Thumbnail created: %s", thumb_path);

                unlink(source_thumb);
            } else {
                ESP_LOGW(TAG, "Could not open files for thumbnail copy");
                if (src)
                    fclose(src);
                if (dst)
                    fclose(dst);
            }
        } else {
            ESP_LOGW(TAG, "Source thumbnail not found: %s", source_thumb);
        }
    }

    free(combined_rgb);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Combined portrait BMP: %dx%d (Display-compatible with dithering)",
                 DISPLAY_WIDTH, DISPLAY_HEIGHT);
    }

    return ret;
}

/**
 * @brief Combine two portrait BMPs side-by-side into DISPLAY_WIDTH×DISPLAY_HEIGHT BMP
 */
esp_err_t image_processor_init(void)
{
    load_calibrated_palette();
    ESP_LOGI(TAG, "Image processor initialized");
    return ESP_OK;
}

void image_processor_reload_palette(void)
{
    load_calibrated_palette();
    ESP_LOGI(TAG, "Calibrated palette reloaded");
}

static uint8_t *resize_image(uint8_t *src, int src_w, int src_h, int dst_w, int dst_h)
{
    uint8_t *dst = (uint8_t *) heap_caps_malloc(dst_w * dst_h * 3, MALLOC_CAP_SPIRAM);
    if (!dst) {
        ESP_LOGE(TAG, "Failed to allocate resize buffer");
        return NULL;
    }

    // Cover mode: scale to fill entire display, crop excess
    // Use max scale to ensure image covers the entire display
    float scale_x = (float) dst_w / src_w;
    float scale_y = (float) dst_h / src_h;
    float scale = fmaxf(scale_x, scale_y);

    int scaled_w = (int) (src_w * scale);
    int scaled_h = (int) (src_h * scale);

    // Calculate crop offsets in scaled space to center the image
    int offset_x = (scaled_w - dst_w) / 2;
    int offset_y = (scaled_h - dst_h) / 2;

    ESP_LOGI(TAG, "Cover mode resize: %dx%d -> scale %.2f -> %dx%d, offset (%d,%d)", src_w, src_h,
             scale, scaled_w, scaled_h, offset_x, offset_y);

    for (int y = 0; y < dst_h; y++) {
        for (int x = 0; x < dst_w; x++) {
            // Map destination pixel to scaled space, then back to source
            float scaled_x = x + offset_x;
            float scaled_y = y + offset_y;

            // Map from scaled space to source space
            float src_x_f = scaled_x / scale;
            float src_y_f = scaled_y / scale;

            int src_x = (int) src_x_f;
            int src_y = (int) src_y_f;

            // Clamp to source bounds
            if (src_x >= src_w)
                src_x = src_w - 1;
            if (src_y >= src_h)
                src_y = src_h - 1;
            if (src_x < 0)
                src_x = 0;
            if (src_y < 0)
                src_y = 0;

            int dst_idx = (y * dst_w + x) * 3;
            int src_idx = (src_y * src_w + src_x) * 3;

            dst[dst_idx] = src[src_idx];
            dst[dst_idx + 1] = src[src_idx + 1];
            dst[dst_idx + 2] = src[src_idx + 2];
        }
    }

    return dst;
}

static esp_err_t write_bmp_file(const char *filename, uint8_t *rgb_data, int width, int height)
{
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filename);
        return ESP_FAIL;
    }

    int row_size = ((width * 3 + 3) / 4) * 4;
    int image_size = row_size * height;
    int file_size = 54 + image_size;

    uint8_t bmp_header[54] = {'B',
                              'M',
                              file_size & 0xFF,
                              (file_size >> 8) & 0xFF,
                              (file_size >> 16) & 0xFF,
                              (file_size >> 24) & 0xFF,
                              0,
                              0,
                              0,
                              0,
                              54,
                              0,
                              0,
                              0,
                              40,
                              0,
                              0,
                              0,
                              width & 0xFF,
                              (width >> 8) & 0xFF,
                              (width >> 16) & 0xFF,
                              (width >> 24) & 0xFF,
                              height & 0xFF,
                              (height >> 8) & 0xFF,
                              (height >> 16) & 0xFF,
                              (height >> 24) & 0xFF,
                              1,
                              0,
                              24,
                              0,
                              0,
                              0,
                              0,
                              0,
                              image_size & 0xFF,
                              (image_size >> 8) & 0xFF,
                              (image_size >> 16) & 0xFF,
                              (image_size >> 24) & 0xFF,
                              0x13,
                              0x0B,
                              0,
                              0,
                              0x13,
                              0x0B,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0};

    fwrite(bmp_header, 1, 54, fp);

    uint8_t *row_buffer = (uint8_t *) malloc(row_size);
    if (!row_buffer) {
        fclose(fp);
        return ESP_FAIL;
    }

    for (int y = height - 1; y >= 0; y--) {
        memset(row_buffer, 0, row_size);
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 3;
            row_buffer[x * 3] = rgb_data[idx + 2];
            row_buffer[x * 3 + 1] = rgb_data[idx + 1];
            row_buffer[x * 3 + 2] = rgb_data[idx];
        }
        fwrite(row_buffer, 1, row_size, fp);
    }

    free(row_buffer);
    fclose(fp);

    return ESP_OK;
}

/**
 * @brief Convert JPG from RAM buffer directly to BMP without SD intermediate file
 *
 * This optimized version avoids writing JPG to SD and reading it back,
 * reducing peak memory usage and improving speed.
 *
 * @param jpg_buffer Pointer to JPG data in RAM (will NOT be freed by this function)
 * @param jpg_size Size of JPG data in bytes
 * @param bmp_path Output BMP file path
 * @param use_stock_mode true = use theoretical palette, false = use measured palette
 * @return ESP_OK on success, error code otherwise
 */
// Convert JPG from RAM buffer directly to BMP (optimized, no SD intermediate)
// rotate_portrait: true = rotate portrait, false = keep Portrait not rotated
// Do NOT rotate portrait if intended for Combine!
esp_err_t image_processor_convert_buffer_to_bmp(uint8_t *jpg_buffer, size_t jpg_size,
                                                const char *bmp_path, bool use_stock_mode,
                                                bool rotate_portrait)
{
    ESP_LOGI(TAG, "Converting %zu bytes JPG buffer to BMP (mode: %s, rotate: %s)", jpg_size,
             use_stock_mode ? "stock" : "enhanced", rotate_portrait ? "yes" : "no");

    // First, get image info at full scale
    esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata = jpg_buffer,
        .indata_size = jpg_size,
        .outbuf = NULL,
        .outbuf_size = 0,
        .out_format = JPEG_IMAGE_FORMAT_RGB888,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags =
            {
                .swap_color_bytes = 0,
            },
    };

    esp_jpeg_image_output_t outimg;
    esp_err_t ret = esp_jpeg_get_image_info(&jpeg_cfg, &outimg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get JPEG info: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "JPEG info: %dx%d, output size: %zu bytes", outimg.width, outimg.height,
             outimg.output_len);

    // Determine optimal JPEG decode scale
    esp_jpeg_image_scale_t decode_scale = JPEG_IMAGE_SCALE_0;
    int scaled_width = outimg.width;
    int scaled_height = outimg.height;

    if (outimg.width >= DISPLAY_WIDTH * 2 || outimg.height >= DISPLAY_HEIGHT * 2) {
        decode_scale = JPEG_IMAGE_SCALE_1_2;
        scaled_width = outimg.width / 2;
        scaled_height = outimg.height / 2;
        ESP_LOGI(TAG, "Using 1/2 JPEG decode scale: %dx%d -> %dx%d", outimg.width, outimg.height,
                 scaled_width, scaled_height);
    }

    if (outimg.width >= DISPLAY_WIDTH * 4 || outimg.height >= DISPLAY_HEIGHT * 4) {
        decode_scale = JPEG_IMAGE_SCALE_1_4;
        scaled_width = outimg.width / 4;
        scaled_height = outimg.height / 4;
        ESP_LOGI(TAG, "Using 1/4 JPEG decode scale: %dx%d -> %dx%d", outimg.width, outimg.height,
                 scaled_width, scaled_height);
    }

    // Check size limits
    if (outimg.width >= DISPLAY_WIDTH * 8 || outimg.height >= DISPLAY_HEIGHT * 8) {
        ESP_LOGE(TAG, "Image too large: %dx%d (max: %dx%d)", outimg.width, outimg.height,
                 DISPLAY_WIDTH * 8, DISPLAY_HEIGHT * 8);
        return ESP_ERR_INVALID_SIZE;
    }

    // Get scaled image info if needed
    if (decode_scale != JPEG_IMAGE_SCALE_0) {
        jpeg_cfg.out_scale = decode_scale;
        ret = esp_jpeg_get_image_info(&jpeg_cfg, &outimg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get scaled JPEG info: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "Scaled JPEG output: %dx%d, size: %zu bytes", outimg.width, outimg.height,
                 outimg.output_len);
    }

    // Final safety check
    const size_t MAX_DECODED_SIZE = 4 * 1024 * 1024;  // 4MB max
    if (outimg.output_len > MAX_DECODED_SIZE) {
        ESP_LOGE(TAG, "Decoded image size too large: %zu bytes (max: %zu bytes)", outimg.output_len,
                 MAX_DECODED_SIZE);
        return ESP_ERR_NO_MEM;
    }

    // Allocate output buffer for decoded RGB
    uint8_t *rgb_buffer = (uint8_t *) heap_caps_malloc(outimg.output_len, MALLOC_CAP_SPIRAM);
    if (!rgb_buffer) {
        ESP_LOGE(TAG, "Failed to allocate RGB buffer: %zu bytes", outimg.output_len);
        return ESP_ERR_NO_MEM;
    }

    // Decode JPEG
    jpeg_cfg.outbuf = rgb_buffer;
    jpeg_cfg.outbuf_size = outimg.output_len;
    ret = esp_jpeg_decode(&jpeg_cfg, &outimg);

    // CRITICAL: jpg_buffer can now be freed by caller!
    // We only need rgb_buffer from here on

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(ret));
        free(rgb_buffer);
        return ret;
    }

    ESP_LOGI(TAG, "Successfully decoded JPEG: %dx%d", outimg.width, outimg.height);

    // Continue with existing resize/rotate/dither logic
    uint8_t *resized = NULL;
    uint8_t *rotated = NULL;
    uint8_t *final_image = rgb_buffer;
    int final_width = outimg.width;
    int final_height = outimg.height;

    bool is_portrait = (outimg.height > outimg.width);
    ESP_LOGI(TAG, "Image orientation: %dx%d (portrait: %d)", outimg.width, outimg.height,
             is_portrait);

    // STEP 1: Resize for portrait (only if will be rotated)
    int target_width, target_height;

    if (is_portrait && rotate_portrait) {
        // Portrait that WILL be rotated: resize to fit display width after rotation
        target_width = (outimg.width * DISPLAY_WIDTH) / outimg.height;
        target_height = DISPLAY_WIDTH;
        ESP_LOGI(TAG, "Portrait resize (will rotate): %dx%d -> %dx%d", final_width, final_height,
                 target_width, target_height);
    } else if (is_portrait && !rotate_portrait) {
        // Portrait that will NOT be rotated: keep as-is or resize to fit portrait display
        target_width = outimg.width;
        target_height = outimg.height;
        ESP_LOGI(TAG, "Portrait resize (no rotation): keeping %dx%d", target_width, target_height);
    } else {
        // Landscape: resize to display size
        target_width = DISPLAY_WIDTH;
        target_height = DISPLAY_HEIGHT;
        ESP_LOGI(TAG, "Landscape resize: %dx%d -> %dx%d", final_width, final_height, target_width,
                 target_height);
    }

    // Only resize if needed
    if (final_width != target_width || final_height != target_height) {
        resized = resize_image(final_image, final_width, final_height, target_width, target_height);
        if (!resized) {
            ESP_LOGE(TAG, "Failed to resize image");
            free(rgb_buffer);
            return ESP_ERR_NO_MEM;
        }
        free(rgb_buffer);
        rgb_buffer = NULL;
        final_image = resized;
        final_width = target_width;
        final_height = target_height;
        ESP_LOGI(TAG, "Resize complete: %dx%d", final_width, final_height);
    }

    // STEP 2: Rotate if portrait AND rotation enabled
    if (is_portrait && rotate_portrait) {
        size_t rotated_size = final_width * final_height * 3;
        rotated = (uint8_t *) heap_caps_malloc(rotated_size, MALLOC_CAP_SPIRAM);
        if (!rotated) {
            ESP_LOGE(TAG, "Failed to allocate rotation buffer");
            if (resized)
                free(resized);
            else if (rgb_buffer)
                free(rgb_buffer);
            return ESP_ERR_NO_MEM;
        }

        // Rotate 90° clockwise
        for (int y = 0; y < final_height; y++) {
            for (int x = 0; x < final_width; x++) {
                int src_idx = (y * final_width + x) * 3;
                int dst_x = final_height - 1 - y;
                int dst_y = x;
                int dst_idx = (dst_y * final_height + dst_x) * 3;
                rotated[dst_idx] = final_image[src_idx];
                rotated[dst_idx + 1] = final_image[src_idx + 1];
                rotated[dst_idx + 2] = final_image[src_idx + 2];
            }
        }

        if (resized)
            free(resized);
        else if (rgb_buffer)
            free(rgb_buffer);
        rgb_buffer = NULL;
        resized = NULL;
        final_image = rotated;

        // Swap dimensions
        int temp = final_width;
        final_width = final_height;
        final_height = temp;
        ESP_LOGI(TAG, "After rotation: %dx%d", final_width, final_height);

    } else if (is_portrait && !rotate_portrait) {
        ESP_LOGI(TAG, "Portrait kept unrotated: %dx%d", final_width, final_height);
    }

    // STEP 3: Final resize ONLY for landscape or rotated portraits
    bool is_unrotated_portrait = (is_portrait && !rotate_portrait);

    if ((final_width != DISPLAY_WIDTH || final_height != DISPLAY_HEIGHT) &&
        !is_unrotated_portrait) {
        ESP_LOGI(TAG, "Final resize: %dx%d -> %dx%d", final_width, final_height, DISPLAY_WIDTH,
                 DISPLAY_HEIGHT);

        uint8_t *final_resized =
            resize_image(final_image, final_width, final_height, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        if (!final_resized) {
            ESP_LOGE(TAG, "Final resize failed");
            if (rotated)
                free(rotated);
            else if (resized)
                free(resized);
            else if (rgb_buffer)
                free(rgb_buffer);
            return ESP_ERR_NO_MEM;
        }

        if (rotated)
            free(rotated);
        else if (resized)
            free(resized);
        else if (rgb_buffer)
            free(rgb_buffer);

        final_image = final_resized;
        final_width = DISPLAY_WIDTH;
        final_height = DISPLAY_HEIGHT;

    } else if (is_unrotated_portrait) {
        ESP_LOGI(TAG, "Skipping final resize for unrotated portrait: %dx%d", final_width,
                 final_height);
    }

    // Apply dithering
    const rgb_t *dither_palette = use_stock_mode ? palette : palette_measured;
    ESP_LOGI(TAG, "Applying dithering with %s palette",
             use_stock_mode ? "theoretical" : "measured");
    apply_floyd_steinberg_dither(final_image, final_width, final_height, dither_palette);

    // Write BMP file
    ESP_LOGI(TAG, "Writing BMP file: %s", bmp_path);
    ret = write_bmp_file(bmp_path, final_image, final_width, final_height);

    // Cleanup
    free(final_image);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Successfully converted buffer to BMP: %s", bmp_path);
    } else {
        ESP_LOGE(TAG, "Failed to write BMP file");
    }

    return ret;
}

esp_err_t image_processor_convert_jpg_to_bmp(const char *jpg_path, const char *bmp_path,
                                             bool use_stock_mode)
{
    ESP_LOGI(TAG, "Converting %s to %s (mode: %s)", jpg_path, bmp_path,
             use_stock_mode ? "stock" : "enhanced");

    FILE *fp = fopen(jpg_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open JPG file: %s", jpg_path);
        return ESP_FAIL;
    }

    fseek(fp, 0, SEEK_END);
    long jpg_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *jpg_buffer = (uint8_t *) heap_caps_malloc(jpg_size, MALLOC_CAP_SPIRAM);
    if (!jpg_buffer) {
        ESP_LOGE(TAG, "Failed to allocate JPG buffer");
        fclose(fp);
        return ESP_FAIL;
    }

    fread(jpg_buffer, 1, jpg_size, fp);
    fclose(fp);

    // First, get image info at full scale to determine if we need to scale during decode
    esp_jpeg_image_cfg_t jpeg_cfg = {.indata = jpg_buffer,
                                     .indata_size = jpg_size,
                                     .outbuf = NULL,
                                     .outbuf_size = 0,
                                     .out_format = JPEG_IMAGE_FORMAT_RGB888,
                                     .out_scale = JPEG_IMAGE_SCALE_0,
                                     .flags = {
                                         .swap_color_bytes = 0,
                                     }};

    esp_jpeg_image_output_t outimg;
    esp_err_t ret = esp_jpeg_get_image_info(&jpeg_cfg, &outimg);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get JPEG info: %s", esp_err_to_name(ret));
        free(jpg_buffer);
        return ret;
    }

    ESP_LOGI(TAG, "JPEG info: %dx%d, output size: %zu bytes", outimg.width, outimg.height,
             outimg.output_len);

    // Determine optimal JPEG decode scale to reduce memory usage
    // Scale down large images during decode to avoid memory allocation failures
    esp_jpeg_image_scale_t decode_scale = JPEG_IMAGE_SCALE_0;
    int scaled_width = outimg.width;
    int scaled_height = outimg.height;

    // If image is much larger than display, use JPEG decoder's built-in scaling
    // This reduces memory usage significantly (e.g., 1/2 scale = 1/4 memory)
    if (outimg.width > DISPLAY_WIDTH * 2 || outimg.height > DISPLAY_HEIGHT * 2) {
        decode_scale = JPEG_IMAGE_SCALE_1_2;  // 1:2 scale
        scaled_width = outimg.width / 2;
        scaled_height = outimg.height / 2;
        ESP_LOGI(TAG, "Image is large, using 1:2 JPEG decode scale: %dx%d -> %dx%d", outimg.width,
                 outimg.height, scaled_width, scaled_height);
    }

    if (outimg.width > DISPLAY_WIDTH * 4 || outimg.height > DISPLAY_HEIGHT * 4) {
        decode_scale = JPEG_IMAGE_SCALE_1_4;  // 1:4 scale
        scaled_width = outimg.width / 4;
        scaled_height = outimg.height / 4;
        ESP_LOGI(TAG, "Image is very large, using 1:4 JPEG decode scale: %dx%d -> %dx%d",
                 outimg.width, outimg.height, scaled_width, scaled_height);
    }

    // Check if image is still too large even after maximum scaling
    // Maximum supported: 1:8 scale would be ~6400x3840 original -> 800x480 scaled
    if (outimg.width > DISPLAY_WIDTH * 8 || outimg.height > DISPLAY_HEIGHT * 8) {
        ESP_LOGE(TAG, "Image is too large: %dx%d (max supported: %dx%d)", outimg.width,
                 outimg.height, DISPLAY_WIDTH * 8, DISPLAY_HEIGHT * 8);
        free(jpg_buffer);
        return ESP_ERR_INVALID_SIZE;
    }

    // Get scaled image info
    if (decode_scale != JPEG_IMAGE_SCALE_0) {
        jpeg_cfg.out_scale = decode_scale;
        ret = esp_jpeg_get_image_info(&jpeg_cfg, &outimg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get scaled JPEG info: %s", esp_err_to_name(ret));
            free(jpg_buffer);
            return ret;
        }
        ESP_LOGI(TAG, "Scaled JPEG output: %dx%d, size: %zu bytes", outimg.width, outimg.height,
                 outimg.output_len);
    }

    // Final safety check: ensure decoded size won't exceed available memory
    // Typical SPIRAM: 8MB, need headroom for processing
    const size_t MAX_DECODED_SIZE = 4 * 1024 * 1024;  // 4MB max for decoded image
    if (outimg.output_len > MAX_DECODED_SIZE) {
        ESP_LOGE(TAG, "Decoded image size too large: %zu bytes (max: %zu bytes)", outimg.output_len,
                 MAX_DECODED_SIZE);
        free(jpg_buffer);
        return ESP_ERR_NO_MEM;
    }

    // Allocate output buffer for scaled image
    uint8_t *rgb_buffer = (uint8_t *) heap_caps_malloc(outimg.output_len, MALLOC_CAP_SPIRAM);
    if (!rgb_buffer) {
        ESP_LOGE(TAG, "Failed to allocate output buffer (%zu bytes)", outimg.output_len);
        free(jpg_buffer);
        return ESP_FAIL;
    }

    // Decode JPEG with scaling
    jpeg_cfg.outbuf = rgb_buffer;
    jpeg_cfg.outbuf_size = outimg.output_len;

    ret = esp_jpeg_decode(&jpeg_cfg, &outimg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(ret));
        free(rgb_buffer);
        free(jpg_buffer);
        return ret;
    }

    ESP_LOGI(TAG, "Successfully decoded JPEG: %dx%d", outimg.width, outimg.height);
    free(jpg_buffer);

    uint8_t *resized = NULL;
    uint8_t *rotated = NULL;
    uint8_t *final_image = rgb_buffer;
    int final_width = outimg.width;
    int final_height = outimg.height;

    // Check if image is portrait and needs rotation for display
    bool is_portrait = outimg.height > outimg.width;
    ESP_LOGI(TAG, "Image orientation check: %dx%d (width x height), is_portrait=%d", outimg.width,
             outimg.height, is_portrait);

    // STEP 1: Resize to appropriate target size immediately to free large decoded buffer
    int target_width, target_height;

    if (is_portrait) {
        // Portrait: resize to fit display width after rotation
        // Target height = display width (800), maintain aspect ratio
        target_width = (outimg.width * DISPLAY_WIDTH) / outimg.height;
        target_height = DISPLAY_WIDTH;
        ESP_LOGI(TAG, "Portrait image: resizing %dx%d -> %dx%d (will rotate after)", final_width,
                 final_height, target_width, target_height);
    } else {
        // Landscape: resize directly to display size
        target_width = DISPLAY_WIDTH;
        target_height = DISPLAY_HEIGHT;
        ESP_LOGI(TAG, "Landscape image: resizing %dx%d -> %dx%d", final_width, final_height,
                 target_width, target_height);
    }

    // Only resize if needed
    if (final_width != target_width || final_height != target_height) {
        resized = resize_image(final_image, final_width, final_height, target_width, target_height);
        if (!resized) {
            ESP_LOGE(TAG, "Failed to resize image from %dx%d to %dx%d", final_width, final_height,
                     target_width, target_height);
            free(rgb_buffer);
            return ESP_FAIL;
        }
        free(rgb_buffer);
        rgb_buffer = NULL;
        final_image = resized;
        final_width = target_width;
        final_height = target_height;
        ESP_LOGI(TAG, "Resize complete: %dx%d", final_width, final_height);
    }

    // STEP 2: Rotate portrait images (now working with smaller buffer)
    if (is_portrait) {
        size_t rotated_size = final_width * final_height * 3;
        ESP_LOGI(TAG, "Rotating portrait image, allocating %zu bytes", rotated_size);

        rotated = (uint8_t *) heap_caps_malloc(rotated_size, MALLOC_CAP_SPIRAM);
        if (!rotated) {
            ESP_LOGE(TAG, "Failed to allocate rotation buffer (%zu bytes)", rotated_size);
            if (resized)
                free(resized);
            else if (rgb_buffer)
                free(rgb_buffer);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Performing 90° clockwise rotation");

        // Rotate 90° clockwise: swap dimensions and rotate pixels
        for (int y = 0; y < final_height; y++) {
            for (int x = 0; x < final_width; x++) {
                int src_idx = (y * final_width + x) * 3;
                int dst_x = final_height - 1 - y;
                int dst_y = x;
                int dst_idx = (dst_y * final_height + dst_x) * 3;

                rotated[dst_idx] = final_image[src_idx];
                rotated[dst_idx + 1] = final_image[src_idx + 1];
                rotated[dst_idx + 2] = final_image[src_idx + 2];
            }
        }

        ESP_LOGI(TAG, "Rotation complete");
        if (resized)
            free(resized);
        else if (rgb_buffer)
            free(rgb_buffer);
        rgb_buffer = NULL;
        resized = NULL;
        final_image = rotated;

        // Swap dimensions after rotation
        int temp = final_width;
        final_width = final_height;
        final_height = temp;
        ESP_LOGI(TAG, "After rotation: %dx%d", final_width, final_height);
    }

    // STEP 3: Final resize if still needed (shouldn't happen normally)
    if (final_width != DISPLAY_WIDTH || final_height != DISPLAY_HEIGHT) {
        ESP_LOGE(TAG, "Unexpected dimensions %dx%d after processing, expected %dx%d", final_width,
                 final_height, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        uint8_t *final_resized =
            resize_image(final_image, final_width, final_height, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        if (!final_resized) {
            ESP_LOGE(TAG, "Final resize failed from %dx%d to %dx%d", final_width, final_height,
                     DISPLAY_WIDTH, DISPLAY_HEIGHT);
            if (rotated)
                free(rotated);
            else if (resized)
                free(resized);
            else if (rgb_buffer)
                free(rgb_buffer);
            return ESP_FAIL;
        }
        if (rotated) {
            free(rotated);
            rotated = NULL;
        } else if (resized) {
            free(resized);
            resized = NULL;
        } else if (rgb_buffer) {
            free(rgb_buffer);
            rgb_buffer = NULL;
        }
        final_image = final_resized;
        final_width = DISPLAY_WIDTH;
        final_height = DISPLAY_HEIGHT;
    }

    // Apply dithering based on processing mode
    // Stock mode: use theoretical palette (matches original Waveshare algorithm)
    // Enhanced mode: use measured palette (accurate error diffusion)
    const rgb_t *dither_palette = use_stock_mode ? palette : palette_measured;
    ESP_LOGI(TAG, "Applying Floyd-Steinberg dithering with %s palette",
             use_stock_mode ? "theoretical" : "measured");
    apply_floyd_steinberg_dither(final_image, final_width, final_height, dither_palette);

    // Write BMP file
    ESP_LOGI(TAG, "Writing BMP file");
    ret = write_bmp_file(bmp_path, final_image, final_width, final_height);

    // Cleanup - free final_image (which could be rotated, resized, final_resized, or rgb_buffer)
    free(final_image);

    // These should already be NULL if they were freed earlier, but check anyway
    if (rgb_buffer && rgb_buffer != final_image) {
        free(rgb_buffer);
    }
    if (resized && resized != final_image) {
        free(resized);
    }
    if (rotated && rotated != final_image) {
        free(rotated);
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Successfully converted %s to %s", jpg_path, bmp_path);
    } else {
        ESP_LOGE(TAG, "Failed to write BMP file");
    }

    return ret;
}

/**
 * @brief Convert JPG buffer to BMP with portrait combine support - OPTIMIZED
 *
 * This version works directly from RAM buffer without SD intermediate file.
 * Detects portrait images and handles combination logic.
 */
esp_err_t image_processor_convert_buffer_to_bmp_with_portrait_combine(uint8_t *jpg_buffer,
                                                                      size_t jpg_size,
                                                                      const char *bmp_path,
                                                                      bool use_stock_mode,
                                                                      const char *album_name)
{
    if (!jpg_buffer || !bmp_path || !album_name) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Converting buffer with portrait combine (feature %s)",
             portrait_combine_enabled ? "ENABLED" : "DISABLED");

    // Step 1: Detect JPEG dimensions
    int jpg_width, jpg_height;
    esp_err_t ret = detect_jpeg_dimensions(jpg_buffer, jpg_size, &jpg_width, &jpg_height);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to detect JPEG dimensions");
        return ret;
    }

    bool is_portrait = (jpg_height > jpg_width);
    ESP_LOGI(TAG, "JPEG dimensions: %dx%d (w×h) - %s", jpg_width, jpg_height,
             is_portrait ? "PORTRAIT" : "LANDSCAPE");

    // Step 2: Check if we should handle as portrait-combine
    if (!is_portrait) {
        // Landscape: Normal processing
        ESP_LOGI(TAG, "Landscape image - normal conversion");
        return image_processor_convert_buffer_to_bmp(jpg_buffer, jpg_size, bmp_path, use_stock_mode,
                                                     true);  // rotate=true (not used for landscape)
    }

    if (!portrait_combine_enabled) {
        // Portrait BUT combine disabled: Treat as landscape (rotate to landscape)
        ESP_LOGI(TAG, "Portrait image but combine DISABLED - converting to landscape");
        return image_processor_convert_buffer_to_bmp(jpg_buffer, jpg_size, bmp_path, use_stock_mode,
                                                     true);  // rotate=true
    }

    // Step 3: Portrait + combine enabled - save WITHOUT rotation to portrait directory
    ESP_LOGI(TAG, "Portrait image with combine ENABLED - saving unrotated to portrait directory");

    // Build portrait directory path
    char portrait_dir[TELEGRAM_MAX_PATH_LENGTH];
    snprintf(portrait_dir, sizeof(portrait_dir), "%s/%s/%s", IMAGE_DIRECTORY, album_name,
             PORTRAIT_DIR_NAME);

    // Ensure directory exists
    struct stat st;
    if (stat(portrait_dir, &st) != 0) {
        ESP_LOGI(TAG, "Creating portrait directory: %s", portrait_dir);
        mkdir(portrait_dir, 0755);
    }

    // Extract filename from bmp_path
    const char *filename = strrchr(bmp_path, '/');
    if (!filename) {
        filename = bmp_path;
    } else {
        filename++;  // Skip '/'
    }

    // Build portrait BMP path
    char portrait_bmp_path[TELEGRAM_MAX_PATH_LENGTH * 2];
    int written =
        snprintf(portrait_bmp_path, sizeof(portrait_bmp_path), "%s/%s", portrait_dir, filename);

    if (written >= sizeof(portrait_bmp_path)) {
        ESP_LOGE(TAG, "Portrait BMP path too long (%d chars)", written);
        return ESP_ERR_INVALID_SIZE;
    }

    // Convert WITHOUT rotation (keep portrait format)
    // ESP_LOGI(TAG, "Converting portrait buffer to BMP (unrotated): %s", portrait_bmp_path);
    // ret = image_processor_convert_buffer_to_bmp(jpg_buffer, jpg_size, portrait_bmp_path,
    // use_stock_mode, false);  // rotate=false!
    // Convert JPG buffer to portrait BMP (WITHOUT rotation - keep portrait format!)

    // Convert JPG buffer to 24-bit RGB BMP WITHOUT dithering (for later combination)
    ESP_LOGI(TAG, "Converting portrait buffer to 24-bit RGB BMP (no dithering): %s",
             portrait_bmp_path);
    ret =
        convert_buffer_to_rgb_bmp(jpg_buffer, jpg_size, portrait_bmp_path, false);  // rotate=false!

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to convert portrait buffer");
        return ret;
    }

    // Step 4: Search for second portrait BMP for combining
    char second_portrait[TELEGRAM_MAX_PATH_LENGTH];
    ret = find_portrait_bmp(portrait_dir, filename, second_portrait, sizeof(second_portrait));

    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "No second portrait found - keeping single portrait for later pairing");
        return ESP_OK;  // Not an error - waiting for pair
    }

    // Step 5: Found second portrait - combine them
    ESP_LOGI(TAG, "Found second portrait: %s - combining both", second_portrait);

    // Generate combined filename in main album directory
    char combined_path[TELEGRAM_MAX_PATH_LENGTH];
    snprintf(combined_path, sizeof(combined_path), "%s/%s/combined_%ld.bmp", IMAGE_DIRECTORY,
             album_name, (long) time(NULL));

    ret = combine_portrait_bmps(portrait_bmp_path, second_portrait, combined_path);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to combine portraits");
        return ret;
    }

    // Step 6: Delete portrait BMPs and create empty marker files for duplicate detection
    ESP_LOGI(TAG, "Deleting used portrait BMPs after successful combination");
    unlink(portrait_bmp_path);
    unlink(second_portrait);

    // Create empty marker files (0 bytes) for duplicate detection
    // These files prevent re-uploading the same portraits
    FILE *marker1 = fopen(portrait_bmp_path, "w");
    if (marker1) {
        fclose(marker1);
        ESP_LOGI(TAG, "Created marker: %s (0 bytes)", portrait_bmp_path);
    } else {
        ESP_LOGW(TAG, "Failed to create marker for: %s", portrait_bmp_path);
    }

    FILE *marker2 = fopen(second_portrait, "w");
    if (marker2) {
        fclose(marker2);
        ESP_LOGI(TAG, "Created marker: %s (0 bytes)", second_portrait);
    } else {
        ESP_LOGW(TAG, "Failed to create marker for: %s", second_portrait);
    }

    ESP_LOGI(TAG, "Portrait combination complete: %s", combined_path);
    ESP_LOGI(TAG, "Marker files created for duplicate detection (0 KB disk usage)");
    return ESP_OK;

    /*
        ESP_LOGI(TAG, "Deleting used portrait BMPs after successful combination");
    unlink(portrait_bmp_path);
    unlink(second_portrait);

    ESP_LOGI(TAG, " Portrait combination complete: %s", combined_path);
    return ESP_OK;
        */
}

/**
 * @brief Combine two portrait BMPs side-by-side into landscape BMP
 * @param bmp1_path First portrait BMP
 * @param bmp2_path Second portrait BMP
 * @param output_path Output landscape BMP path
 * @return ESP_OK on success
 */

// ===== Main conversion function with Portrait-Combination =====

esp_err_t image_processor_convert_jpg_to_bmp_with_portrait_combine(const char *jpg_path,
                                                                   const char *bmp_path,
                                                                   bool use_stock_mode,
                                                                   const char *album_name)
{
    if (!jpg_path || !bmp_path || !album_name) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Converting with portrait combine: %s (feature %s)", jpg_path,
             portrait_combine_enabled ? "enabled" : "disabled");

    // Step 1: Detect JPG dimensions WITHOUT full decode
    FILE *fp = fopen(jpg_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open JPG: %s", jpg_path);
        return ESP_FAIL;
    }

    fseek(fp, 0, SEEK_END);
    long jpg_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *jpg_buffer = heap_caps_malloc(jpg_size, MALLOC_CAP_SPIRAM);
    if (!jpg_buffer) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    fread(jpg_buffer, 1, jpg_size, fp);
    fclose(fp);

    int jpg_width, jpg_height;
    esp_err_t ret = detect_jpeg_dimensions(jpg_buffer, jpg_size, &jpg_width, &jpg_height);
    free(jpg_buffer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to detect JPEG dimensions");
        return ret;
    }

    bool is_portrait = (jpg_height > jpg_width);
    ESP_LOGI(TAG, "JPEG dimensions: %dx%d (w×h) - %s", jpg_width, jpg_height,
             is_portrait ? "PORTRAIT" : "LANDSCAPE");

    // Step 2: LANDSCAPE - normal conversion
    if (!is_portrait || !portrait_combine_enabled) {
        ESP_LOGI(TAG, "Processing as landscape (normal conversion)");
        return image_processor_convert_jpg_to_bmp(jpg_path, bmp_path, use_stock_mode);
    }

    // Step 3: PORTRAIT - save to portrait directory
    ESP_LOGI(TAG, "Portrait detected - saving to portrait directory");

    // Build portrait directory path: /sdcard/images/<album>/portrait/
    char portrait_dir[TELEGRAM_MAX_PATH_LENGTH];
    snprintf(portrait_dir, sizeof(portrait_dir), "%s/%s/%s", IMAGE_DIRECTORY, album_name,
             PORTRAIT_DIR_NAME);

    // Ensure portrait directory exists
    struct stat st;
    if (stat(portrait_dir, &st) != 0) {
        ESP_LOGI(TAG, "Creating portrait directory: %s", portrait_dir);
        mkdir(portrait_dir, 0755);
    }

    // Extract filename from bmp_path
    const char *filename = strrchr(bmp_path, '/');
    if (!filename) {
        filename = bmp_path;
    } else {
        filename++;  // Skip the '/'
    }

    // Build portrait BMP path
    // char portrait_bmp_path[TELEGRAM_MAX_PATH_LENGTH *2];
    // snprintf(portrait_bmp_path, sizeof(portrait_bmp_path), "%s/%s", portrait_dir, filename);
    char portrait_bmp_path[TELEGRAM_MAX_PATH_LENGTH * 2];
    int written =
        snprintf(portrait_bmp_path, sizeof(portrait_bmp_path), "%s/%s", portrait_dir, filename);

    // Prüfen ob Pfad zu lang war
    if (written >= sizeof(portrait_bmp_path)) {
        ESP_LOGE(TAG, "Portrait BMP path too long (%d chars)", written);
        return ESP_ERR_INVALID_SIZE;
    }

    // Convert JPG to BMP (portrait, no rotation)
    ESP_LOGI(TAG, "Converting portrait JPG to BMP: %s", portrait_bmp_path);
    ret = image_processor_convert_jpg_to_bmp(jpg_path, portrait_bmp_path, use_stock_mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to convert portrait JPG");
        return ret;
    }

    // Step 4: Search for second portrait BMP
    char second_portrait[TELEGRAM_MAX_PATH_LENGTH];
    ret = find_portrait_bmp(portrait_dir, filename, second_portrait, sizeof(second_portrait));

    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "No second portrait found - keeping single portrait for later pairing");
        return ESP_OK;  // Not an error - just waiting for pair
    }

    // Step 5: Combine two portraits
    ESP_LOGI(TAG, "Found second portrait - combining both");

    // Generate combined filename (use timestamp or first filename)
    char combined_path[TELEGRAM_MAX_PATH_LENGTH];
    snprintf(combined_path, sizeof(combined_path), "%s/%s/combined_%ld.bmp", IMAGE_DIRECTORY,
             album_name, (long) time(NULL));

    ret = combine_portrait_bmps(portrait_bmp_path, second_portrait, combined_path);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to combine portraits");
        return ret;
    }

    // Step 6: Delete both portrait BMPs and their source JPGs
    ESP_LOGI(TAG, "Deleting used portrait BMPs and source JPGs");

    // Delete BMP1
    unlink(portrait_bmp_path);

    // Delete BMP2
    unlink(second_portrait);

    // Delete source JPG1
    unlink(jpg_path);

    // Delete source JPG2 (derive from BMP path)
    char jpg2_path[TELEGRAM_MAX_PATH_LENGTH];
    strncpy(jpg2_path, second_portrait, sizeof(jpg2_path) - 1);
    char *ext = strrchr(jpg2_path, '.');
    if (ext) {
        snprintf(ext, 5, ".jpg");
        unlink(jpg2_path);  // Ignore errors if not exists
    }

    ESP_LOGI(TAG, "Portrait combination complete: %s", combined_path);
    return ESP_OK;
}
