#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

// #include <zephyr/posix/sys/socket.h>
// #include <zephyr/posix/unistd.h>
// #include <zephyr/posix/arpa/inet.h>

#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/sntp.h>
#include <time.h>
#include <zephyr/net/http/client.h>

#include <zephyr/sys/atomic.h>
#include <string.h>
#include <errno.h>

#include "cfca_root.h"

LOG_MODULE_REGISTER(http, LOG_LEVEL_INF);

#define CA_CERTIFICATE_TAG 	1
#define HTTPS_HOSTNAME 		"jgyw.crfsdi.com.cn"
#define HTTPS_PORT 		"51111"
static const char *extra_headers[] = {
	"Connection: close\r\n",
	"Accept: */*\r\n",
	"User-Agent: Zephyr-HTTP-Client/1.0\r\n",
	NULL
};

static int sockfd = -1;
static uint8_t recv_buf[512];

/* 等待 WiFi + DHCP 拿到 IPv4, 而不是盲睡固定时长:
 * 首次连接含全信道扫描可能 >10s, 网络异常时也要能醒过来报错 */
static atomic_t have_ipv4;
static struct net_mgmt_event_callback ip_cb;

static void ip_event_handler(struct net_mgmt_event_callback *cb,
			     uint32_t mgmt_event, struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		atomic_set(&have_ipv4, 1);
	}
}

static bool ipv4_available(void)
{
	struct net_if *iface = net_if_get_default();

	return iface && net_if_ipv4_get_global_addr(iface, NET_ADDR_DHCP) != NULL;
}

static bool network_up(void)
{
	return ipv4_available() || atomic_get(&have_ipv4);
}

static int connect_socket(void) 
{
	struct zsock_addrinfo hints, *res, *p;
	int ret = 0;
	sec_tag_t sec_tag_list[] = { CA_CERTIFICATE_TAG };
	int peer_verify = TLS_PEER_VERIFY_NONE; // 是否难对端证书

	memset(&hints, 0, sizeof(struct zsock_addrinfo));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	ret = zsock_getaddrinfo("jgyw.crfsdi.com.cn", "51111", &hints, &res);
	if (ret != 0) {
		LOG_ERR("Get address information failed %d", ret);
		return -1;
	}

	for(p = res; p != NULL; p = p->ai_next) {
		sockfd = zsock_socket(p->ai_family, SOCK_STREAM, IPPROTO_TLS_1_2);
		if (sockfd == -1) {
			LOG_ERR("sockfd get failed %d", sockfd);
			continue;
		}
		
		/* 在 connect 之前设置 tls 选项 */
		/* 设 ca 证书 tag */
		ret = zsock_setsockopt(sockfd, SOL_TLS, TLS_SEC_TAG_LIST, sec_tag_list, sizeof(sec_tag_list));
		if (ret < 0) {
			LOG_ERR("set SEC_TAG_LIST: %s", strerror(errno));
			zsock_close(sockfd);
			sockfd = -1;
			continue;
		}

		/* 设 SNI hostname (通配符证书 *.crfsdi.com.cn 必需) */
		ret = zsock_setsockopt(sockfd, SOL_TLS, TLS_HOSTNAME, HTTPS_HOSTNAME, strlen(HTTPS_HOSTNAME) + 1);
		if (ret < 0) {
			LOG_ERR("set TLS_HOSTNAME: %s", strerror(errno));
			zsock_close(sockfd);
			sockfd = -1;
			continue;
		}

		/* 验证对端证书 */
		ret = zsock_setsockopt(sockfd, SOL_TLS, TLS_PEER_VERIFY,
		       &peer_verify, sizeof(peer_verify));
		if (ret < 0) {
			LOG_ERR("set PEER_VERIFY: %s", strerror(errno));
			zsock_close(sockfd);
			sockfd = -1;
			continue;
		}

		/* 现在 connect ( TLS 握手会在首次 send 时发生 ) */
		ret = zsock_connect(sockfd, p->ai_addr, p->ai_addrlen);
		if (ret != 0) {
			LOG_ERR("Connect socket failed! Code: %d", ret);
			zsock_close(sockfd);
			sockfd = -1;
			continue;
		}
		break;
	
	}
	
	zsock_freeaddrinfo(res);
	if (sockfd == -1) {
		LOG_ERR("All connect failed!");
	}
	return sockfd;
}

static void response_cb(struct http_response *rsp,
			enum http_final_call final_data,
			void *user_data)
{
	if (final_data == HTTP_DATA_MORE) {
		LOG_INF("Partial data received (%zd bytes)", rsp->data_len);
	} else if (final_data == HTTP_DATA_FINAL) {
		LOG_INF("All the data received (%zd bytes)", rsp->data_len);			
	}

	LOG_INF("Response to %s", (const char *)user_data);
	LOG_INF("Response status %s", rsp->http_status);
}

void http_thread_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	int ret = 0;
	
	/* 注册 ca 证书, 只需要一次 */
	ret = tls_credential_add(CA_CERTIFICATE_TAG, TLS_CREDENTIAL_CA_CERTIFICATE,
				cfca_root_der,
				cfca_root_der_len);
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("tls_credential_add: %d", ret);
		return;
	}
	LOG_INF("CA cert registered.");

	/* 注册 IPV4 事件回调(整个生命周期只注册一次) */
	net_mgmt_init_event_callback(&ip_cb, ip_event_handler,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ip_cb);

	struct http_request req = { 0 };
	req.method = HTTP_GET;
	req.url = "/backendapi/auth/publicKey";
	req.host = HTTPS_HOSTNAME;
	req.protocol = "HTTP/1.1";
	req.response = response_cb;
	req.recv_buf = recv_buf;
	req.recv_buf_len = sizeof(recv_buf);
	req.header_fields = extra_headers;

	/* 循环: 等网络 → HTTPS 流程 → 周期重测。
	 * 工地网络可能几分钟后才通, 不能等一次就退出 */
	bool was_up = false;
	int sntp_tries = 0;

	while (1) {
		bool up = network_up();
		if (!up) {
			was_up = false;
			k_sleep(K_SECONDS(2));
			continue;
		}
		if (!was_up) {
			LOG_INF("Network ready (IPv4 acquired)");
			was_up = true;
		}

		/* 尽力同步系统时间(最多试 3 次)。当前
		 * TLS_PEER_VERIFY_NONE 不校验证书, 失败不致命 */
		if (sntp_tries < 3) {
			sntp_tries++;
			struct sntp_time ts;
			int sntp_ret = sntp_simple("120.25.115.20", 15000, &ts);
			if (sntp_ret == 0) {
			    struct timespec now = {
				.tv_sec  = ts.seconds,
				.tv_nsec = ((uint64_t)ts.fraction * 1000000000ULL) >> 32,
			    };
			    clock_settime(CLOCK_REALTIME, &now);
			    LOG_INF("SNTP synced, time=%llu", ts.seconds);
			    sntp_tries = 3; /* 成功即止 */
			} else {
			    LOG_WRN("SNTP failed: %d, continue anyway", sntp_ret);
			}
		}

		/* 弱网下 TLS 握手 + 首次请求可能较慢, 失败重试几次 */
		for (int attempt = 0; attempt < 3; attempt++) {
			/* 连接,内部用IPPROTO_TLS_1_2; Zephyr 3.7 的 TLS 握手
			 * 在 zsock_connect() 内完成 */
			sockfd = connect_socket();
			if (sockfd == -1) {
				LOG_ERR("Connect socket failed (attempt %d)!", attempt + 1);
				k_sleep(K_SECONDS(5));
				continue;
			}
			LOG_INF("Connect socket done!");

			ret = http_client_req(sockfd, &req, 10000, "IPv4 GET");
			if (ret < 0) {
				LOG_ERR("Client error %d (attempt %d)", ret, attempt + 1);
			}

			zsock_close(sockfd);
			sockfd = -1;
			if (ret >= 0) {
				break;
			}
			k_sleep(K_SECONDS(5));
		}

		/* 每 10s 重测一轮; 断网后回到上面的等待 */
		k_sleep(K_SECONDS(10));
	}

}

K_THREAD_DEFINE(myhttp, 8192, http_thread_entry,NULL, NULL, NULL,5, 0, 0);
