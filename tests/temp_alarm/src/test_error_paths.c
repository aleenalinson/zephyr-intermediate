/*
 * temp_alarm - Error-path unit tests (homework skeleton)
 *
 * Task 2: fill in the stubs according to TEST_SPEC.md. Each stub calls
 * ztest_test_skip(), so the binary builds and runs cleanly until the test is
 * implemented.
 *
 * Run:
 *   west twister -T tests/temp_alarm -p native_sim
 */

#include <errno.h>
#include <zephyr/fff.h>
#include <zephyr/ztest.h>

#include "sensor_fake.h"
#include "temp_alarm.h"

#define DEFAULT_THRESHOLD 30

/* Same before() hook as in test_basic.c: a clean fake and an initialised module. */
static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	sensor_fake_reset();
	sensor_fake_init();

	zassume_ok(temp_alarm_init(&fake_sensor_dev, DEFAULT_THRESHOLD),
		   "precondition: temp_alarm_init must succeed");
}

/*
 * ============================================================================
 * Test Suite: temp_alarm_sensor_errors
 *
 * Failures from the sensor driver, injected through the fakes.
 * ============================================================================
 */
ZTEST_SUITE(temp_alarm_sensor_errors, NULL, NULL, before, NULL, NULL);

ZTEST(temp_alarm_sensor_errors, test_fetch_error_propagates)
{
	struct temp_alarm_status status;

	fake_sensor_sample_fetch_fake.return_val = -EIO;
	zassert_equal(temp_alarm_check(), -EIO, "fetch failure should propagate");
	zassert_equal(fake_sensor_channel_get_fake.call_count, 0,
		      "channel_get must not run after a failed fetch");
	zassert_ok(temp_alarm_get_status(&status), "get_status should succeed");
	zassert_equal(status.last_error, -EIO, "last_error should record fetch failure");
}

ZTEST(temp_alarm_sensor_errors, test_channel_get_error_propagates)
{
	struct temp_alarm_status status;

	/* Disable the successful custom callback so the fake's return value is used. */
	fake_sensor_channel_get_fake.custom_fake = NULL;
	fake_sensor_channel_get_fake.return_val = -EIO;

	zassert_equal(temp_alarm_check(), -EIO, "channel_get failure should propagate");
	zassert_equal(fake_sensor_sample_fetch_fake.call_count, 1,
		      "sample_fetch should still be called once");
	zassert_ok(temp_alarm_get_status(&status), "get_status should succeed");
	zassert_equal(status.last_error, -EIO, "last_error should record channel_get failure");
}

ZTEST(temp_alarm_sensor_errors, test_fail_then_recover)
{
	static int fetch_results[] = {-EIO, 0};
	struct temp_alarm_status status;

	SET_RETURN_SEQ(fake_sensor_sample_fetch, fetch_results, ARRAY_SIZE(fetch_results));
	sensor_fake_set_temperature(20);

	zassert_equal(temp_alarm_check(), -EIO, "first check should report fetch failure");
	zassert_ok(temp_alarm_check(), "second check should recover");
	zassert_ok(temp_alarm_get_status(&status), "get_status should succeed");

	zassert_equal(status.last_temp, 20, "successful retry should update last temperature");
	zassert_false(status.is_alarming, "20 C should not raise the alarm");
	zassert_equal(status.last_error, -EIO, "last error remains until reset");
}

/*
 * ============================================================================
 * Test Suite: temp_alarm_invalid_input
 *
 * Arguments the public API must reject.
 * ============================================================================
 */
ZTEST_SUITE(temp_alarm_invalid_input, NULL, NULL, before, NULL, NULL);

ZTEST(temp_alarm_invalid_input, test_get_status_null)
{
	zassert_equal(temp_alarm_get_status(NULL), -EINVAL,
		      "NULL status pointer should return -EINVAL");
}

ZTEST(temp_alarm_invalid_input, test_init_without_device)
{
	zassert_equal(temp_alarm_init(NULL, DEFAULT_THRESHOLD), -ENODEV,
		      "missing device should return -ENODEV");
}

ZTEST(temp_alarm_invalid_input, test_init_threshold_out_of_range)
{
	zassert_equal(temp_alarm_init(&fake_sensor_dev, -1), -EINVAL,
		      "negative init threshold should return -EINVAL");
	zassert_equal(temp_alarm_init(&fake_sensor_dev, 101), -EINVAL,
		      "init threshold above maximum should return -EINVAL");
}

ZTEST(temp_alarm_invalid_input, test_set_threshold_out_of_range)
{
	struct temp_alarm_status status;

	zassert_equal(temp_alarm_set_threshold(-1), -EINVAL,
		      "negative threshold should return -EINVAL");
	zassert_equal(temp_alarm_set_threshold(101), -EINVAL,
		      "threshold above maximum should return -EINVAL");

	/* A reading above the original 30 C threshold proves it was preserved. */
	sensor_fake_set_temperature(31);
	zassert_ok(temp_alarm_check(), "check should succeed with original threshold");
	zassert_ok(temp_alarm_get_status(&status), "get_status should succeed");
	zassert_true(status.is_alarming, "31 C should exceed the unchanged 30 C threshold");
	zassert_equal(status.alarm_count, 1U, "alarm should be counted once");
}
