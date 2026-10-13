#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

LOG_MODULE_REGISTER(demo, LOG_LEVEL_INF);

#define STACK_SIZE       2048
#define SENSOR_COUNT       18
#define SENSOR_PERIOD_MS  100

struct sensor_data {
    int32_t temperature_mc;
    uint32_t timestamp_ms;
    uint8_t seq;
};

/* Display listener callback */
static void display_listener_cb(const struct zbus_channel *chan)
{
    const struct sensor_data *msg = zbus_chan_const_msg(chan);

    LOG_INF("[DISPLAY] seq=%u temp=%d mC timestamp=%u",
            msg->seq,
            msg->temperature_mc,
            msg->timestamp_ms);
}

/* Observers */
ZBUS_LISTENER_DEFINE(display_listener, display_listener_cb);
ZBUS_SUBSCRIBER_DEFINE(logger_subscriber, 16);

/* One channel shared by the publisher and observers */
ZBUS_CHAN_DEFINE(sensor_channel,
                 struct sensor_data,
                 NULL,
                 NULL,
                 ZBUS_OBSERVERS(display_listener, logger_subscriber),
                 ZBUS_MSG_INIT(.temperature_mc = 0,
                               .timestamp_ms = 0,
                               .seq = 0));

/* Publisher: generates a sample every 100 ms */
static void sensor_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    for (int i = 0; i < SENSOR_COUNT; i++) {
        struct sensor_data data = {
            .temperature_mc = 24000 + (i * 350),
            .timestamp_ms = k_uptime_get_32(),
            .seq = (uint8_t)i,
        };

        int ret = zbus_chan_pub(&sensor_channel, &data, K_MSEC(100));

        if (ret != 0) {
            LOG_ERR("[SENSOR] publish failed: %d", ret);
        } else {
            LOG_INF("[SENSOR] published seq=%u temp=%d mC",
                    data.seq, data.temperature_mc);
        }

        k_msleep(SENSOR_PERIOD_MS);
    }

    LOG_INF("[SENSOR] finished publishing");
}

/* Subscriber: processes message copies and logs more slowly */
static void logger_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    const struct zbus_channel *chan;
    int received = 0;

    while (received < SENSOR_COUNT) {
        struct sensor_data msg;

        /* Wait for a channel notification */
        int ret = zbus_sub_wait(&logger_subscriber,
                                &chan,
                                K_MSEC(3000));

        if (ret != 0) {
            LOG_WRN("[LOGGER] receive timeout: %d", ret);
            break;
        }

        /* Read the channel's current message */
        ret = zbus_chan_read(chan, &msg, K_MSEC(100));

        if (ret != 0) {
            LOG_WRN("[LOGGER] channel read failed: %d", ret);
            continue;
        }

        received++;

        LOG_INF("[LOGGER] received seq=%u temp=%d mC latency=%u ms",
                msg.seq,
                msg.temperature_mc,
                k_uptime_get_32() - msg.timestamp_ms);

        /* Simulate slower logging */
        k_msleep(250);
    }

    LOG_INF("[LOGGER] finished, received=%d", received);
}

K_THREAD_DEFINE(sensor_thread,
                STACK_SIZE,
                sensor_thread_fn,
                NULL, NULL, NULL,
                5, 0, 0);

K_THREAD_DEFINE(logger_thread,
                STACK_SIZE,
                logger_thread_fn,
                NULL, NULL, NULL,
                6, 0, 0);

int main(void)
{
    LOG_INF("=== L4 Task 1: zbus Pub-Sub ===");
    LOG_INF("Sensor period: %d ms", SENSOR_PERIOD_MS);
    LOG_INF("Samples: %d", SENSOR_COUNT);
    LOG_INF("Display uses a synchronous listener");
    LOG_INF("Logger uses a message subscriber");

    return 0;
}
