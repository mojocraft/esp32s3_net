#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
	while(1) {
		k_msleep(1000);
		LOG_INF("This is a test.");
	}
	
}
