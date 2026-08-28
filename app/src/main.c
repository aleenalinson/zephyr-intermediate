#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(demo, LOG_LEVEL_DBG);

#define STACK_SIZE 1024

#define PRIO_A 7
#define PRIO_B 7

static volatile uint32_t counter;
static K_MUTEX_DEFINE(counter_mutex);
static struct k_sem done_sem;
#define INCREMENTS      1000000   /* each thread increments this many times */

void thread_a_fn(void *p1, void *p2, void *p3)
{

    for (int i = 0; i < INCREMENTS; i++) {
        k_mutex_lock(&counter_mutex, K_FOREVER);
        counter++;
        k_mutex_unlock(&counter_mutex);
    }
    k_sem_give(&done_sem);
}

void thread_b_fn(void *p1, void *p2, void *p3)
{
   

    for (int i = 0; i < INCREMENTS; i++) {
        k_mutex_lock(&counter_mutex, K_FOREVER);
        counter++;
        k_mutex_unlock(&counter_mutex);
    }
    k_sem_give(&done_sem);
}


K_THREAD_DEFINE(thread_a, STACK_SIZE, thread_a_fn,
                NULL, NULL, NULL, PRIO_A, 0, 0);
K_THREAD_DEFINE(thread_b, STACK_SIZE, thread_b_fn,
                NULL, NULL, NULL, PRIO_B, 0, 0);


int main(void)
{
    LOG_INF("=== L2 Task1 1 ===");
    k_sem_take(&done_sem, K_FOREVER);
    k_sem_take(&done_sem, K_FOREVER);

    LOG_INF("Actual  final value: %u", counter);
    
    return 0;
}


