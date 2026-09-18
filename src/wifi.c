#include <zephyr/kernel.h>
#include <zephyr/net/wifi.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/ethernet.h>

LOG_MODULE_REGISTER(wifi, LOG_LEVEL_INF);

#define WIFI_THREAD_STACK_SIZE 4096
#define WIFI_THREAD_PRIORITY 5

static void wifi_thread_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (1) {
		LOG_INF("Wifi Thread.");
		k_msleep(1000);
	}
	
}

void wifi_init(void)
{
	
}

K_THREAD_DEFINE(
	wifi_thread,
	WIFI_THREAD_STACK_SIZE,
	wifi_thread_entry,
	NULL,
	NULL,
	NULL,
	WIFI_THREAD_PRIORITY,
	0,
	0	
);

