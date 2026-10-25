/**
 * @file perf_stats.c
 * @brief Periodic CPU-load and heap report on the console.
 *
 * Every BASMIO_PERF_STATS_PERIOD_MS the task samples FreeRTOS run-time
 * counters and logs the CPU share each task used since the previous sample.
 * Disable with CONFIG_BASMIO_PERF_STATS=n.
 */

#include "perf_stats.h"
#include "sdkconfig.h"

#if CONFIG_BASMIO_PERF_STATS && CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "perf";

#define PERF_MAX_TASKS      40
#define PERF_MIN_PCT_X10    5   /* hide tasks below 0.5% */

typedef struct {
    TaskHandle_t handle;
    configRUN_TIME_COUNTER_TYPE runtime;
} perf_prev_t;

static perf_prev_t s_prev[PERF_MAX_TASKS];
static UBaseType_t s_prev_count;
static configRUN_TIME_COUNTER_TYPE s_prev_total;

static configRUN_TIME_COUNTER_TYPE prev_runtime(TaskHandle_t h)
{
    for (UBaseType_t i = 0; i < s_prev_count; i++) {
        if (s_prev[i].handle == h) {
            return s_prev[i].runtime;
        }
    }
    return 0;
}

static void perf_report(TaskStatus_t *tasks)
{
    configRUN_TIME_COUNTER_TYPE total = 0;
    UBaseType_t n = uxTaskGetSystemState(tasks, PERF_MAX_TASKS, &total);
    const configRUN_TIME_COUNTER_TYPE span = total - s_prev_total;

    if (s_prev_total != 0 && span > 0) {
        /* Run-time counter advances once per core, so a fully busy core is 100%. */
        const uint64_t denom = (uint64_t)span * portNUM_PROCESSORS;
        ESP_LOGI(TAG, "---- CPU load (%u tasks, both cores = 200%%) ----", (unsigned)n);
        for (UBaseType_t i = 0; i < n; i++) {
            const configRUN_TIME_COUNTER_TYPE d = tasks[i].ulRunTimeCounter - prev_runtime(tasks[i].xHandle);
            const unsigned pct_x10 = (unsigned)(((uint64_t)d * 2000u) / denom);
            if (pct_x10 < PERF_MIN_PCT_X10) {
                continue;
            }
            const BaseType_t core = xTaskGetCoreID(tasks[i].xHandle);
            ESP_LOGI(TAG, "%-16s %3u.%u%%  prio=%2u core=%c stack_free=%u",
                     tasks[i].pcTaskName,
                     pct_x10 / 10u, pct_x10 % 10u,
                     (unsigned)tasks[i].uxCurrentPriority,
                     core == tskNO_AFFINITY ? '*' : (char)('0' + core),
                     (unsigned)tasks[i].usStackHighWaterMark);
        }
        ESP_LOGI(TAG, "heap internal free=%u min=%u largest=%u | psram free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    s_prev_count = n;
    for (UBaseType_t i = 0; i < n; i++) {
        s_prev[i].handle = tasks[i].xHandle;
        s_prev[i].runtime = tasks[i].ulRunTimeCounter;
    }
    s_prev_total = total;
}

static void perf_stats_task(void *arg)
{
    (void)arg;
    TaskStatus_t *tasks = calloc(PERF_MAX_TASKS, sizeof(TaskStatus_t));
    if (!tasks) {
        ESP_LOGW(TAG, "no memory for task snapshot");
        vTaskDelete(NULL);
        return;
    }
    while (1) {
        perf_report(tasks);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BASMIO_PERF_STATS_PERIOD_MS));
    }
}

void perf_stats_start(void)
{
    if (xTaskCreate(perf_stats_task, "perf_stats", 4096, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "perf_stats task create failed");
    }
}

#else

void perf_stats_start(void)
{
}

#endif
