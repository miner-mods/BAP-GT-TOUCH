/**
 * @file loading.c
 *
 * Simple loading screen UI for boot up phase
 */

/*********************
 *      INCLUDES
 *********************/
#include "loading.h"
#include "home.h"
#include "custom_fonts.h"
extern const lv_img_dsc_t mm_splash;
#include "esp_log.h"
#include "bap.h"
#include "miners.h"
#include "board_config.h"
#include "wifi.h"
#include "lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"

/*********************
 *      DEFINES
 *********************/
static const char *TAG = "loading";
#define LOADING_DURATION_MS 4000
#define LOADING_STEPS 100
#define LOADING_STEP_TIME_MS (LOADING_DURATION_MS / LOADING_STEPS)

/**********************
 *  STATIC VARIABLES
 **********************/
static lv_obj_t *screen;
static lv_obj_t *progress_bar;
static lv_obj_t *loading_label;
static lv_obj_t *logo_img;
static lv_timer_t *loading_timer;
static uint8_t progress_value = 0;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void loading_timer_cb(lv_timer_t *timer);
static void finish_loading(void);
static void bap_init_task(void *pvParameters);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void loading(void)
{
    screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // Miner-Mods boot logo
    logo_img = lv_img_create(screen);
    lv_img_set_src(logo_img, &mm_splash);
    lv_obj_align(logo_img, LV_ALIGN_TOP_MID, 0, 0);

    // Loading text
    loading_label = lv_label_create(screen);
    lv_label_set_text(loading_label, "Loading...");
    lv_obj_set_style_text_color(loading_label, COLOR_TEXT_SECONDARY, 0);
    lv_obj_align(loading_label, LV_ALIGN_BOTTOM_MID, 0, -8);

    // Create progress bar
    progress_bar = lv_bar_create(screen);
    lv_obj_set_size(progress_bar, 200, 20);
    lv_obj_align(progress_bar, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(progress_bar, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_bar, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(progress_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(progress_bar, COLOR_BORDER, LV_PART_MAIN);

    loading_timer = lv_timer_create(loading_timer_cb, LOADING_STEP_TIME_MS, NULL);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void loading_timer_cb(lv_timer_t *timer)
{
    progress_value++;

    lv_bar_set_value(progress_bar, progress_value, LV_ANIM_ON);

    char buffer[32];
    lv_snprintf(buffer, sizeof(buffer), "Hacking the Planet... %d%%", progress_value);
    lv_label_set_text(loading_label, buffer);

    // If loading is complete, transition to dashboard
    if (progress_value >= 100)
    {
        finish_loading();
    }
}

// BAP initialization task - runs in separate task to avoid blocking LVGL
static void bap_init_task(void *pvParameters)
{
    ESP_LOGI(TAG, "BAP initialization task started");

    esp_err_t wdt_ret = esp_task_wdt_add(NULL);
    if (wdt_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to add BAP init task to watchdog: %s", esp_err_to_name(wdt_ret));
    }

    ESP_LOGI(TAG, "Waiting for system to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Reset watchdog before BAP initialization
    if (wdt_ret == ESP_OK)
    {
        esp_task_wdt_reset();
    }

#if MM_STANDALONE_WIFI
    // No BAP cable: join the saved WiFi network (if one has been set up)
    if (lvgl_port_lock(-1)) {
        wifi_autoconnect_saved();
        lvgl_port_unlock();
    }
#else
    ESP_LOGI(TAG, "Initializing BAP...");

    esp_err_t ret = BAP_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "BAP initialization failed: %s", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "BAP initialized successfully");
    }

#endif

    // Miner-Mods: read both miners over WiFi
    miners_start();

    // Remove from watchdog before deleting task
    if (wdt_ret == ESP_OK)
    {
        esp_task_wdt_delete(NULL);
    }

    vTaskDelete(NULL);
}

static void finish_loading(void)
{
    lv_timer_del(loading_timer);

    lv_obj_clean(screen);
#if MM_STANDALONE_WIFI
    if (!wifi_has_saved_credentials()) {
        // First boot: go straight to WiFi setup
        wifi_screen_create();
        lv_scr_load(wifi_get_screen());
    } else
#endif
    {
        home_screen_create();
        lv_scr_load(home_get_screen());
    }

    ESP_LOGI(TAG, "Creating BAP initialization task...");
    xTaskCreate(bap_init_task, "bap_init", 8192, NULL, 10, NULL);
}
