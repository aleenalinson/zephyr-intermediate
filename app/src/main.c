
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/tracing/tracing.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <stdint.h>
#include <stdbool.h>

LOG_MODULE_REGISTER(homework, LOG_LEVEL_INF);

/* ================================================================
 * Configuration
 * ================================================================ */

#define STACK_SIZE                 2048

/* Baseline: intentionally incorrect priority assignment.
 * In Zephyr, a smaller number means a higher priority.
 * Swap these two values to test the correction.
 */
#define CONTROL_PRIORITY           4
#define MAINTENANCE_PRIORITY       7

#define EVENT_PERIOD_MS            250
#define FIRST_EVENT_DELAY_MS       500
#define RESPONSE_DEADLINE_MS       10
#define MAINTENANCE_LOAD_US        45000

#define CONTROL_QUEUE_DEPTH        4
#define WARNING_INTERVAL           10

/* ================================================================
 * Event and queue
 * ================================================================ */

struct control_event {
    uint32_t seq;
    uint32_t ready_ms;
};

K_MSGQ_DEFINE(control_queue,
              sizeof(struct control_event),
              CONTROL_QUEUE_DEPTH,
              4);

K_SEM_DEFINE(maintenance_start, 0, 1);

/* ================================================================
 * Statistics
 * ================================================================ */

static atomic_t deadline_miss_count;
static atomic_t processed_count;
static atomic_t dropped_count;

static uint32_t next_sequence;

/* ================================================================
 * Timer callback
 *
 * Zephyr executes this callback in interrupt context.
 * Never block or perform lengthy logging here.
 * ================================================================ */

static void event_timer_expiry(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    struct control_event event = {
        .seq = next_sequence++,
        .ready_ms = k_uptime_get_32(),
    };

    /* Do not wait if the queue is full. */
    int ret = k_msgq_put(&control_queue, &event, K_NO_WAIT);

    if (ret != 0) {
        atomic_inc(&dropped_count);

        sys_trace_named_event("event_dropped",
                              event.seq,
                              event.ready_ms);
        return;
    }

    /* Signal maintenance so the competing workload is triggered
     * for this event. Control is also made ready by the queue put.
     */
    k_sem_give(&maintenance_start);

    sys_trace_named_event("event_ready",
                          event.seq,
                          event.ready_ms);
}

/* Start the timer from main(). */
K_TIMER_DEFINE(event_timer, event_timer_expiry, NULL);

/* ================================================================
 * Control thread
 * ================================================================ */

K_THREAD_STACK_DEFINE(control_stack, STACK_SIZE);
static struct k_thread control_thread_data;

static void control_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (true) {
        struct control_event event;

        int ret = k_msgq_get(&control_queue,
                             &event,
                             K_FOREVER);

        if (ret != 0) {
            LOG_ERR("[CONTROL] Queue receive failed: %d", ret);
            continue;
        }

        /* Measure response-to-start latency. */
        uint32_t start_ms = k_uptime_get_32();
        uint32_t latency_ms = start_ms - event.ready_ms;

        sys_trace_named_event("control_start",
                              event.seq,
                              latency_ms);

        atomic_inc(&processed_count);

        LOG_INF("[CONTROL] seq=%u ready=%u start=%u latency=%u ms",
                event.seq,
                event.ready_ms,
                start_ms,
                latency_ms);

        /* Count every deadline miss, but limit warning output. */
        if (latency_ms > RESPONSE_DEADLINE_MS) {
            int misses = atomic_inc(&deadline_miss_count) + 1;

            if ((misses == 1) ||
                ((misses % WARNING_INTERVAL) == 0)) {
                LOG_WRN("[DEADLINE] miss #%d seq=%u "
                        "latency=%u ms deadline=%u ms",
                        misses,
                        event.seq,
                        latency_ms,
                        RESPONSE_DEADLINE_MS);
            }
        }

        sys_trace_named_event("control_done",
                              event.seq,
                              latency_ms);
    }
}

/* ================================================================
 * Maintenance thread
 *
 * Lower importance, but its priority is intentionally higher in
 * the baseline configuration. Busy wait simulates CPU load.
 * ================================================================ */

K_THREAD_STACK_DEFINE(maintenance_stack, STACK_SIZE);
static struct k_thread maintenance_thread_data;

static void maintenance_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (true) {
        k_sem_take(&maintenance_start, K_FOREVER);

        sys_trace_named_event("maintenance_start",
                              k_uptime_get_32(),
                              MAINTENANCE_LOAD_US);

        k_busy_wait(MAINTENANCE_LOAD_US);

        sys_trace_named_event("maintenance_done",
                              k_uptime_get_32(),
                              MAINTENANCE_LOAD_US);
    }
}

/* ================================================================
 * Main
 * ================================================================ */

int main(void)
{
    LOG_INF("=== L6 Homework: Runtime Investigation ===");
    LOG_INF("Response-to-start deadline: %u ms",
            RESPONSE_DEADLINE_MS);
    LOG_INF("Event period: %u ms", EVENT_PERIOD_MS);
    LOG_INF("Maintenance workload: %u us",
            MAINTENANCE_LOAD_US);
    LOG_INF("Control priority: %d", CONTROL_PRIORITY);
    LOG_INF("Maintenance priority: %d", MAINTENANCE_PRIORITY);

    k_thread_create(&control_thread_data,
                    control_stack,
                    K_THREAD_STACK_SIZEOF(control_stack),
                    control_fn,
                    NULL, NULL, NULL,
                    CONTROL_PRIORITY,
                    0,
                    K_NO_WAIT);

    k_thread_name_set(&control_thread_data, "control");

    k_thread_create(&maintenance_thread_data,
                    maintenance_stack,
                    K_THREAD_STACK_SIZEOF(maintenance_stack),
                    maintenance_fn,
                    NULL, NULL, NULL,
                    MAINTENANCE_PRIORITY,
                    0,
                    K_NO_WAIT);

    k_thread_name_set(&maintenance_thread_data, "maintenance");

    k_timer_start(&event_timer,
                  K_MSEC(FIRST_EVENT_DELAY_MS),
                  K_MSEC(EVENT_PERIOD_MS));

    /* Periodically report counters. The application keeps running
     * so the kernel shell and trace can be inspected.
     */
    while (true) {
        k_sleep(K_SECONDS(5));

        LOG_INF("[STATS] processed=%d deadline_misses=%d dropped=%d",
                (int)atomic_get(&processed_count),
                (int)atomic_get(&deadline_miss_count),
                (int)atomic_get(&dropped_count));
    }

    return 0;
}
