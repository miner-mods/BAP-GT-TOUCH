/**
 * @file miners.c
 * @brief Miner-Mods dual-miner support.
 *
 * Polls http://<ip>/api/system/info on each miner, adds the numbers together
 * and feeds them to the existing screens through the same update functions
 * the BAP cable uses, so the whole UI shows one combined "double" miner.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "miners.h"
#include "home.h"
#include "block.h"
#include "wifi.h"
#include "lvgl_port.h"
#include "ota_update.h"

static const char *TAG = "MINERS";

#define HTTP_BUF_SIZE (16 * 1024)

typedef struct {
    const char *ip;
    bool ok;                 // last poll succeeded
    int64_t last_ok_us;      // time of last successful poll
    char hostname[33];
    double hashrate_ghs;
    double power_w;
    double temp1;
    double temp2;
    double vr_temp;
    int fan_rpm;
    unsigned long shares_ok;
    unsigned long shares_rej;
    double best_diff;
    double best_session_diff;
    double error_pct;
    unsigned long uptime_s;
    int blocks_found;
    unsigned long block_height;
    char asic_model[16];
    int asic_count;
    double chip_ghs[4];
    unsigned long chip_errors[4];
    char pool_url[128];
    char pool_port[16];
    char pool_user[64];
} miner_state_t;

static miner_state_t s_miners[MINER_COUNT] = {
    {.ip = MINER_0_IP},
    {.ip = MINER_1_IP},
};

static char *s_http_buf = NULL;
static int s_http_len = 0;
static TaskHandle_t s_task = NULL;
static volatile int64_t s_last_any_ok_us = 0;

// ---------------------------------------------------------------- helpers

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        int room = HTTP_BUF_SIZE - 1 - s_http_len;
        int n = evt->data_len < room ? evt->data_len : room;
        if (n > 0) {
            memcpy(s_http_buf + s_http_len, evt->data, n);
            s_http_len += n;
            s_http_buf[s_http_len] = '\0';
        }
    }
    return ESP_OK;
}

static double json_num(const cJSON *obj, const char *key, double fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) {
        return it->valuedouble;
    }
    if (cJSON_IsString(it) && it->valuestring) {
        // Older firmware sends values like "2.84G"
        char *end = NULL;
        double v = strtod(it->valuestring, &end);
        if (end && *end) {
            switch (*end) {
                case 'k': case 'K': v *= 1e3; break;
                case 'M': v *= 1e6; break;
                case 'G': v *= 1e9; break;
                case 'T': v *= 1e12; break;
                case 'P': v *= 1e15; break;
                default: break;
            }
        }
        return v;
    }
    return fallback;
}

static void json_str(const cJSON *obj, const char *key, char *out, size_t out_size)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring) {
        strncpy(out, it->valuestring, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

static void format_diff(double d, char *out, size_t out_size)
{
    const char *suffix[] = {"", "K", "M", "G", "T", "P", "E"};
    int i = 0;
    while (d >= 1000.0 && i < 6) {
        d /= 1000.0;
        i++;
    }
    if (i == 0) {
        snprintf(out, out_size, "%.0f", d);
    } else {
        snprintf(out, out_size, "%.2f%s", d, suffix[i]);
    }
}

// ---------------------------------------------------------------- polling

static bool poll_miner(miner_state_t *m)
{
    char url[64];
    snprintf(url, sizeof(url), "http://%s/api/system/info", m->ip);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 2500,
    };

    s_http_len = 0;
    s_http_buf[0] = '\0';

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || s_http_len == 0) {
        ESP_LOGW(TAG, "%s: no answer (%s, HTTP %d)", m->ip, esp_err_to_name(err), status);
        return false;
    }

    cJSON *root = cJSON_Parse(s_http_buf);
    if (!root) {
        ESP_LOGW(TAG, "%s: bad JSON", m->ip);
        return false;
    }

    m->hashrate_ghs = json_num(root, "hashRate", 0);
    m->power_w = json_num(root, "power", 0);
    m->temp1 = json_num(root, "temp", 0);
    m->temp2 = json_num(root, "temp2", 0);
    m->vr_temp = json_num(root, "vrTemp", 0);
    m->fan_rpm = (int)json_num(root, "fanrpm", 0);
    m->shares_ok = (unsigned long)json_num(root, "sharesAccepted", 0);
    m->shares_rej = (unsigned long)json_num(root, "sharesRejected", 0);
    m->best_diff = json_num(root, "bestDiff", 0);
    m->best_session_diff = json_num(root, "bestSessionDiff", 0);
    m->error_pct = json_num(root, "errorPercentage", 0);
    m->uptime_s = (unsigned long)json_num(root, "uptimeSeconds", 0);
    m->blocks_found = (int)json_num(root, "lifetimeBlocksFound", 0);
    m->block_height = (unsigned long)json_num(root, "blockHeight", 0);
    json_str(root, "hostname", m->hostname, sizeof(m->hostname));
    json_str(root, "ASICModel", m->asic_model, sizeof(m->asic_model));
    json_str(root, "stratumURL", m->pool_url, sizeof(m->pool_url));
    json_str(root, "stratumUser", m->pool_user, sizeof(m->pool_user));
    snprintf(m->pool_port, sizeof(m->pool_port), "%d", (int)json_num(root, "stratumPort", 0));

    // Per-chip data (HexOS "hashrateMonitor"); absent on stock AxeOS
    m->asic_count = 0;
    const cJSON *mon = cJSON_GetObjectItemCaseSensitive(root, "hashrateMonitor");
    const cJSON *asics = mon ? cJSON_GetObjectItemCaseSensitive(mon, "asics") : NULL;
    if (cJSON_IsArray(asics)) {
        const cJSON *chip = NULL;
        cJSON_ArrayForEach(chip, asics) {
            if (m->asic_count >= 4) {
                break;
            }
            m->chip_ghs[m->asic_count] = json_num(chip, "total", 0);
            m->chip_errors[m->asic_count] = (unsigned long)json_num(chip, "errorCount", 0);
            m->asic_count++;
        }
    }

    cJSON_Delete(root);
    return true;
}

// ------------------------------------------------------- push to screens

static void push_combined_to_ui(void)
{
    double ghs = 0, watts = 0, max_temp = 0, best = 0;
    unsigned long ok = 0, rej = 0, height = 0;
    int live = 0, chips = 0;
    const miner_state_t *first_live = NULL;

    for (int i = 0; i < MINER_COUNT; i++) {
        const miner_state_t *m = &s_miners[i];
        if (!m->ok) {
            continue;
        }
        live++;
        if (!first_live) {
            first_live = m;
        }
        ghs += m->hashrate_ghs;
        watts += m->power_w;
        if (m->temp1 > max_temp) max_temp = m->temp1;
        if (m->temp2 > max_temp) max_temp = m->temp2;
        if (m->best_diff > best) best = m->best_diff;
        ok += m->shares_ok;
        rej += m->shares_rej;
        if (m->block_height > height) height = m->block_height;
        chips += m->asic_count;
    }
    if (live == 0) {
        return;
    }

    char hashrate[16], power[16], temp[16], fan[16], shares[32], bd[32], buf[16];
    snprintf(hashrate, sizeof(hashrate), "%.0f", ghs);
    snprintf(power, sizeof(power), "%.1f", watts);
    snprintf(temp, sizeof(temp), "%.1f", max_temp);
    snprintf(fan, sizeof(fan), "%d/%d", s_miners[0].ok ? s_miners[0].fan_rpm : 0,
             s_miners[1].ok ? s_miners[1].fan_rpm : 0);
    snprintf(shares, sizeof(shares), "%lu / %lu", ok, rej);
    format_diff(best, bd, sizeof(bd));

    // Per-miner breakdown for the Hardware popup
    static char details[640];
    int pos = 0;
    for (int i = 0; i < MINER_COUNT && pos < (int)sizeof(details) - 1; i++) {
        const miner_state_t *m = &s_miners[i];
        const char *name = m->hostname[0] ? m->hostname : m->ip;
        if (!m->ok) {
            pos += snprintf(details + pos, sizeof(details) - pos, "%s: OFFLINE\n\n", name);
            continue;
        }
        pos += snprintf(details + pos, sizeof(details) - pos,
                        "%s  (%s)\n%.2f TH/s   %.1f W   %.0f/%.0f C   VR %.0f C   err %.2f%%\n",
                        name, m->ip, m->hashrate_ghs / 1000.0, m->power_w,
                        m->temp1, m->temp2, m->vr_temp, m->error_pct);
        for (int c = 0; c < m->asic_count && pos < (int)sizeof(details) - 1; c++) {
            pos += snprintf(details + pos, sizeof(details) - pos,
                            "   chip %d: %.0f GH/s  (%lu errors)\n", c + 1, m->chip_ghs[c], m->chip_errors[c]);
        }
        if (pos < (int)sizeof(details) - 1) {
            pos += snprintf(details + pos, sizeof(details) - pos, "\n");
        }
    }

    char chip_text[32];
    if (chips > 0 && first_live->asic_model[0]) {
        snprintf(chip_text, sizeof(chip_text), "%dx %s", chips, first_live->asic_model);
    } else {
        snprintf(chip_text, sizeof(chip_text), "%s", first_live->asic_model);
    }

    pool_info_t pool = {0};
    strncpy(pool.url, first_live->pool_url, sizeof(pool.url) - 1);
    strncpy(pool.port, first_live->pool_port, sizeof(pool.port) - 1);
    strncpy(pool.worker_name, first_live->pool_user, sizeof(pool.worker_name) - 1);

    if (!lvgl_port_lock(200)) {
        ESP_LOGW(TAG, "UI busy, skipping this update");
        return;
    }
    home_update_hashrate(hashrate);
    extern void night_update_hashrate(const char *hashrate);
    night_update_hashrate(hashrate);
    home_update_power(power);
    home_update_temperature(temp);
    home_update_fan_speed(fan);
    home_update_shares(shares);
    home_update_best_difficulty(bd);
    // These close an open popup when called, so only send them when they change
    static char last_model[48] = "", last_chip[32] = "", last_pool[256] = "";
    const char *model = live == MINER_COUNT ? "Dual GT 801 (Miner-Mods)" : "Dual GT 801 - ONE OFFLINE";
    if (strcmp(model, last_model) != 0) {
        strncpy(last_model, model, sizeof(last_model) - 1);
        home_update_device_model(model);
    }
    if (strcmp(chip_text, last_chip) != 0) {
        strncpy(last_chip, chip_text, sizeof(last_chip) - 1);
        home_update_asic_model(chip_text);
    }
    char pool_key[256];
    snprintf(pool_key, sizeof(pool_key), "%s:%s:%s", pool.url, pool.port, pool.worker_name);
    if (strcmp(pool_key, last_pool) != 0) {
        strncpy(last_pool, pool_key, sizeof(last_pool) - 1);
        home_update_pool_info(&pool);
    }
    home_update_miner_details(details);
    if (height > 0) {
        snprintf(buf, sizeof(buf), "%lu", height);
        block_update_height(buf);
    }
    lvgl_port_unlock();
}

static void miners_task(void *arg)
{
    (void)arg;
    while (1) {
        if (ota_update_is_running() || !wifi_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(MINERS_POLL_INTERVAL_MS));
            continue;
        }

        int64_t now = esp_timer_get_time();
        for (int i = 0; i < MINER_COUNT; i++) {
            miner_state_t *m = &s_miners[i];
            if (poll_miner(m)) {
                m->ok = true;
                m->last_ok_us = now;
                s_last_any_ok_us = now;
            } else if (now - m->last_ok_us > (int64_t)MINERS_STALE_MS * 1000) {
                m->ok = false;   // keep last values briefly, then drop the miner
            }
        }
        push_combined_to_ui();
        vTaskDelay(pdMS_TO_TICKS(MINERS_POLL_INTERVAL_MS));
    }
}

// ---------------------------------------------------------------- public

void miners_start(void)
{
    if (s_task) {
        return;
    }
    s_http_buf = heap_caps_malloc(HTTP_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_http_buf) {
        s_http_buf = malloc(HTTP_BUF_SIZE);
    }
    if (!s_http_buf) {
        ESP_LOGE(TAG, "Out of memory, dual-miner mode disabled");
        return;
    }
    xTaskCreate(miners_task, "miners", 8192, NULL, 5, &s_task);
    ESP_LOGI(TAG, "Dual-miner polling started (%s, %s)", MINER_0_IP, MINER_1_IP);
}

bool miners_wifi_active(void)
{
    int64_t last = s_last_any_ok_us;
    return last != 0 && (esp_timer_get_time() - last) < (int64_t)MINERS_STALE_MS * 1000;
}

typedef struct {
    int freq;
    int volt;
} setting_job_t;

static void patch_task(void *arg)
{
    setting_job_t *job = (setting_job_t *)arg;
    char body[64];
    snprintf(body, sizeof(body), "{\"frequency\":%d,\"coreVoltage\":%d}", job->freq, job->volt);

    for (int i = 0; i < MINER_COUNT; i++) {
        char url[64];
        snprintf(url, sizeof(url), "http://%s/api/system", s_miners[i].ip);
        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_PATCH,
            .timeout_ms = 4000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            continue;
        }
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        ESP_LOGI(TAG, "%s: set %d MHz / %d mV -> %s (HTTP %d)", s_miners[i].ip, job->freq, job->volt,
                 esp_err_to_name(err), status);
    }
    free(job);
    vTaskDelete(NULL);
}

esp_err_t miners_set_freq_voltage(int frequency_mhz, int core_voltage_mv)
{
    setting_job_t *job = malloc(sizeof(setting_job_t));
    if (!job) {
        return ESP_ERR_NO_MEM;
    }
    job->freq = frequency_mhz;
    job->volt = core_voltage_mv;
    if (xTaskCreate(patch_task, "miner_patch", 6144, job, 5, NULL) != pdPASS) {
        free(job);
        return ESP_FAIL;
    }
    return ESP_OK;
}
