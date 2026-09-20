#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(wifi, LOG_LEVEL_INF);

#define WIFI_THREAD_STACK_SIZE 4096
#define WIFI_THREAD_PRIORITY 5
#define SSID "QHDTUC2.4"
#define PASSWORD "QHDTUC11305610"

/* 连接结果信号量: 成功/失败/掉线事件都会 give, 唤醒 wifi 线程 */
static K_SEM_DEFINE(wifi_event_sem, 0, 1);
/* 跨线程(回调上下文 vs wifi 线程)共享的状态, 必须用原子操作 */
static atomic_t wifi_connected;
static struct net_mgmt_event_callback wifi_mgmt_cb;

static struct wifi_connect_req_params params = {
	.ssid        = SSID,
	.ssid_length = sizeof(SSID) - 1,
	.psk         = PASSWORD,
	.psk_length  = sizeof(PASSWORD) - 1,
	.channel     = 0,
	.security    = WIFI_SECURITY_TYPE_PSK,
	.band        = WIFI_FREQ_BAND_2_4_GHZ,
	.mfp         = WIFI_MFP_OPTIONAL,
};

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
			       uint32_t mgmt_event, struct net_if *iface)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;

	if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
		if (status->status) {
			if (status->status == -1) {
				/* 驱动处于 CONNECTING/未就绪时 raise 的"假失败"
				 * (esp_wifi_drv.c esp32_wifi_connect), 属正常现象 */
				LOG_INF("wifi connect busy (-1), will retry");
			} else {
				LOG_ERR("wifi connect failed %d", status->status);
			}
			atomic_set(&wifi_connected, 0);
		} else {
			LOG_INF("wifi connected");
			atomic_set(&wifi_connected, 1);
		}
		k_sem_give(&wifi_event_sem);
	} else if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		LOG_WRN("wifi disconnected");
		atomic_set(&wifi_connected, 0);
		k_sem_give(&wifi_event_sem);
	}
}

/* 每次调用都重新取 iface, 取不到返回 -ENODEV 由上层退避重试 */
static int wifi_connect_once(void)
{
	struct net_if *iface = net_if_get_default();

	k_msleep(2000);
	if (!iface) {
		return -ENODEV;
	}

	return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface,
			&params, sizeof(params));
}

static void wifi_thread_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	net_mgmt_init_event_callback(&wifi_mgmt_cb, wifi_event_handler,
		NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_mgmt_cb);

	/* 上电后先等驱动/PHY 稳定再发起首次连接:
	 * 否则第一次连接大概率以 WIFI_REASON_TIMEOUT(39) 失败 */
	k_sleep(K_SECONDS(2));

	while (1) {
		if (atomic_get(&wifi_connected)) {
			/* 已连接: 阻塞等掉线事件 */
			k_sem_take(&wifi_event_sem, K_FOREVER);
			/* 掉线后稍等, 给驱动内部自动重连留时间 */
			k_sleep(K_SECONDS(2));
		} else {
			k_sleep(K_SECONDS(2));
			int ret = wifi_connect_once();
			LOG_INF("wifi connecting...");

			if (ret == 0) {
				/* 请求已提交, 等结果事件; 带超时是防止事件丢失后
				 * 永远卡住. 首次连接含全信道扫描, 实测 >10s,
				 * 超时给足 15s */
				k_sem_take(&wifi_event_sem, K_SECONDS(15));
			} else if (ret == -EALREADY) {
				/* 驱动已经在连接中, 别急着重试, 耐心等 */
				k_sleep(K_SECONDS(5));
			} else {
				LOG_WRN("connect request failed (%d), retrying...", ret);
				k_sleep(K_SECONDS(1));
			}
		}
	}
}

K_THREAD_DEFINE(wifi_thread, WIFI_THREAD_STACK_SIZE, wifi_thread_entry,
		NULL, NULL, NULL, WIFI_THREAD_PRIORITY, 0, 0);
