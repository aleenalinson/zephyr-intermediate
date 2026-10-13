#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(demo, LOG_LEVEL_INF);

/* Configuration */
#define SAMPLE_COUNT          12
#define SENSOR_QUEUE_DEPTH     8

#define PRODUCER_STACK_SIZE 1024
#define CONSUMER_STACK_SIZE 1024
#define HEALTH_STACK_SIZE   1024

#define PRODUCER_PERIOD_MS    50
#define CONSUMER_STUCK_MS   2000
#define WATCHDOG_TIMEOUT_MS 1000
#define HEALTH_PERIOD_MS    100
#define HEALTH_WARN_PERCENT  75

/* Sensor message */
struct sensor_sample {
    uint32_t seq;
    int32_t value;
    uint32_t timestamp_ms;
};

/* Producer-consumer queue */
K_MSGQ_DEFINE(sensor_queue,
              sizeof(struct sensor_sample),
              SENSOR_QUEUE_DEPTH,
              4);

/* Completion signals */
K_SEM_DEFINE(producer_done_sem, 0, 1);
K_SEM_DEFINE(consumer_done_sem, 0, 1);
K_SEM_DEFINE(health_done_sem, 0, 1);

/* Shared status */
static atomic_t producer_finished;
static atomic_t watchdog_fired;

static int consumer_wdt_channel = -1;

/* ================================================================
 * Task watchdog callback
 * ================================================================ */

static void watchdog_callback(int channel_id, void *user_data)
{
    ARG_UNUSED(user_data);

    atomic_set(&watchdog_fired, 1);

    LOG_ERR("[WATCHDOG] TIMEOUT! channel=%d", channel_id);
    LOG_ERR("[WATCHDOG] Consumer did not feed watchdog in time");
}

/* ================================================================
 * Producer
 * ================================================================ */

static void producer_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    for (uint32_t i = 0; i < SAMPLE_COUNT; i++) {
        struct sensor_sample sample = {
            .seq = i,
            .value = 100 + (int32_t)i,
            .timestamp_ms = k_uptime_get_32(),
        };

        int ret = k_msgq_put(&sensor_queue,
                             &sample,
                             K_NO_WAIT);

        if (ret != 0) {
            LOG_WRN("[PRODUCER] QUEUE FULL: dropped seq=%u",
                    sample.seq);
        } else {
            LOG_INF("[PRODUCER] queued seq=%u queue=%u/%u",
                    sample.seq,
                    k_msgq_num_used_get(&sensor_queue),
                    SENSOR_QUEUE_DEPTH);
        }

        k_msleep(PRODUCER_PERIOD_MS);
    }

    atomic_set(&producer_finished, 1);

    LOG_INF("[PRODUCER] finished");

    k_sem_give(&producer_done_sem);
}

/* ================================================================
 * Consumer
 * ================================================================ */

static void consumer_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    bool simulate_stuck = true;

    LOG_INF("[CONSUMER] started");

    while (true) {
        /*
         * Feed the watchdog while the consumer is operating normally.
         * The intentional long sleep below prevents further feeding.
         */
        int ret = task_wdt_feed(consumer_wdt_channel);

        if (ret != 0) {
            LOG_ERR("[CONSUMER] watchdog feed failed: %d", ret);
        }

        struct sensor_sample sample;

        ret = k_msgq_get(&sensor_queue,
                         &sample,
                         K_MSEC(100));

        if (ret != 0) {
            if (atomic_get(&producer_finished) &&
                k_msgq_num_used_get(&sensor_queue) == 0) {
                break;
            }

            continue;
        }

        LOG_INF("[CONSUMER] received seq=%u value=%d",
                sample.seq, sample.value);

        /*
         * Intentionally simulate a stuck consumer once.
         * This sleep is longer than the watchdog timeout.
         */
        if (simulate_stuck) {
            simulate_stuck = false;

            LOG_WRN("[CONSUMER] SIMULATING STUCK TASK for %d ms",
                    CONSUMER_STUCK_MS);

            k_sleep(K_MSEC(CONSUMER_STUCK_MS));

            LOG_WRN("[CONSUMER] resumed after long sleep");
        }

        LOG_INF("[CONSUMER] processed seq=%u", sample.seq);
    }

    LOG_INF("[CONSUMER] finished");

    if (consumer_wdt_channel >= 0) {
        int ret = task_wdt_delete(consumer_wdt_channel);

        if (ret != 0) {
            LOG_WRN("[CONSUMER] watchdog delete failed: %d", ret);
        }
    }

    k_sem_give(&consumer_done_sem);
}

/* ================================================================
 * Health monitor
 * ================================================================ */

static void health_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    bool warned_high = false;

    LOG_INF("[HEALTH] monitor started");

    /*
     * Monitor for up to 25 intervals.
     * This covers the producer burst and the consumer's long sleep.
     */
    for (int i = 0; i < 25; i++) {
        k_msleep(HEALTH_PERIOD_MS);

        uint32_t used = k_msgq_num_used_get(&sensor_queue);
        uint32_t capacity = SENSOR_QUEUE_DEPTH;

        LOG_INF("[HEALTH] queue fill=%u/%u (%u%%)",
                used,
                capacity,
                (used * 100U) / capacity);

        if ((used * 100U) >=
            (capacity * HEALTH_WARN_PERCENT)) {

            if (!warned_high) {
                LOG_WRN("[HEALTH] WARNING: queue >= %d%% capacity",
                        HEALTH_WARN_PERCENT);
                warned_high = true;
            }
        } else {
            warned_high = false;
        }

        if (atomic_get(&watchdog_fired)) {
            LOG_WRN("[HEALTH] watchdog timeout has been detected");
        }
    }

    LOG_INF("[HEALTH] monitor finished");

    k_sem_give(&health_done_sem);
}

/* ================================================================
 * Thread definitions
 * ================================================================ */

K_THREAD_STACK_DEFINE(producer_stack, PRODUCER_STACK_SIZE);
K_THREAD_STACK_DEFINE(consumer_stack, CONSUMER_STACK_SIZE);
K_THREAD_STACK_DEFINE(health_stack, HEALTH_STACK_SIZE);

static struct k_thread producer_thread;
static struct k_thread consumer_thread;
static struct k_thread health_thread;

/* ================================================================
 * Main
 * ================================================================ */

int main(void)
{
    LOG_INF("=== L5: Queue Reliability and Task Watchdog ===");
    LOG_INF("Queue depth=%u", SENSOR_QUEUE_DEPTH);
    LOG_INF("Producer period=%d ms", PRODUCER_PERIOD_MS);
    LOG_INF("Watchdog timeout=%d ms", WATCHDOG_TIMEOUT_MS);

    /* Initialize software task watchdog without hardware fallback. */
    int ret = task_wdt_init(NULL);

    if (ret != 0) {
        LOG_ERR("[MAIN] task_wdt_init failed: %d", ret);
        return 0;
    }

    /* Register the consumer's watchdog channel. */
    consumer_wdt_channel = task_wdt_add(
        WATCHDOG_TIMEOUT_MS,
        watchdog_callback,
        NULL);

    if (consumer_wdt_channel < 0) {
        LOG_ERR("[MAIN] task_wdt_add failed: %d",
                consumer_wdt_channel);
        return 0;
    }

    LOG_INF("[MAIN] watchdog channel=%d",
            consumer_wdt_channel);

    /*
     * Start the consumer first, then the producer and health monitor.
     */
    k_thread_create(&consumer_thread,
                    consumer_stack,
                    K_THREAD_STACK_SIZEOF(consumer_stack),
                    consumer_fn,
                    NULL, NULL, NULL,
                    5, 0, K_NO_WAIT);

    k_thread_create(&producer_thread,
                    producer_stack,
                    K_THREAD_STACK_SIZEOF(producer_stack),
                    producer_fn,
                    NULL, NULL, NULL,
                    5, 0, K_NO_WAIT);

    k_thread_create(&health_thread,
                    health_stack,
                    K_THREAD_STACK_SIZEOF(health_stack),
                    health_fn,
                    NULL, NULL, NULL,
                    6, 0, K_NO_WAIT);

    /* Wait for all three activities to finish. */
    k_sem_take(&producer_done_sem, K_FOREVER);
    k_sem_take(&consumer_done_sem, K_FOREVER);
    k_sem_take(&health_done_sem, K_FOREVER);

    LOG_INF("======================================");
    LOG_INF("[SUMMARY] producer finished");
    LOG_INF("[SUMMARY] consumer finished");
    LOG_INF("[SUMMARY] watchdog fired=%d",
            (int)atomic_get(&watchdog_fired));
    LOG_INF("[SUMMARY] queue used=%u",
            k_msgq_num_used_get(&sensor_queue));
    LOG_INF("=== Demo complete ===");

    return 0;
}
