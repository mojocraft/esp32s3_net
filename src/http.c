/**
 * Zephyr 的 http_client_req() 只负责"发请求, 收回应",不负责连接,所以要你自己
 * 创建 socket -> 建立 TCP 连接 -> 做 TLS 握手 -> 调 http_client_req()
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/net/socket.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>

#include <zephyr/posix/poll.h>
#include <zephyr/posix/arpa/inet.h>

#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(http_client, LOG_LEVEL_INF);

/* 宏字符串化: 先展开参数, 再字符串化 */
#define STR(x) #x
#define XSTR(x) STR(x)

#define SERVER_HOSTNAME "jgyw.crfsdi.com.cn"
#define SERVER_PORT 51111
static uint8_t receive_buffer[512];
static int sock;
static struct http_request req = { 0 };

static void response_cb(struct http_response *rsp, enum http_final_call final_data, void *user_data)
{
	if (final_data == HTTP_DATA_MORE) {
		LOG_INF("Partial data received (%zd bytes)", rsp->data_len);
	} else if (final_data == HTTP_DATA_FINAL) {
		LOG_INF("ALL the data received (%zd bytes)", rsp->data_len);
	}

	LOG_INF("Response status %s", rsp->http_status);
}

static int connect_to_server(void)
{
	struct addrinfo hints = {0}; /* 填入地址信息 */
	struct addrinfo *res = NULL; /* 获取信息后返回的指针 */
	struct sockaddr_in addr;
	int ret;

	/* 设置解析条件 */
	hints.ai_family = AF_INET; /* IPv4 */
	hints.ai_socktype = SOCK_STREAM; /* TCP */
	hints.ai_protocol = IPPROTO_TCP;

	ret = zsock_getaddrinfo(SERVER_HOSTNAME, XSTR(SERVER_PORT), &hints, &res);
	if (ret != 0) {
		LOG_ERR("getaddrinfo failed: %d", ret);
		return -1;
	}

	/* 创建 socket */
	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		LOG_ERR("socket failed: %d", errno);
		zsock_freeaddrinfo(res);
		return -1;
	}

	ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen); 
	zsock_freeaddrinfo(res);

	if (ret < 0) {
		LOG_ERR("connect failed: %d", errno);
		zsock_close(sock);
		sock = -1;
		return -1;
	}

	LOG_INF("Connect to %s:%d", SERVER_HOSTNAME, SERVER_PORT);
	return 0;
}

void request_init(void)
{
	int ret = 0;

	LOG_INF("Request init");
	req.method = HTTP_GET;
	req.url = "/backendapi/auth/publicKey";
	req.host = SERVER_HOSTNAME ":" XSTR(SERVER_PORT);
	req.protocol = "HTTP/1.1";
	req.response = response_cb;
	req.recv_buf = receive_buffer;
	req.recv_buf_len = sizeof(receive_buffer);

	LOG_INF("Request init");

	ret = http_client_req(sock, &req, 5000, NULL);	
	if (ret < 0) {
		LOG_ERR("http client request failed: %d", ret);
	}
	zsock_close(sock);
	
	sock = -1;
}

void http_thread_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	k_msleep(30000);

	LOG_INF("HTTP thread start");

	if (connect_to_server() < 0) {
	LOG_ERR("connect to server failed");
	return;
	}

	request_init();
}


K_THREAD_DEFINE(http, 4096, http_thread_entry, NULL, NULL, NULL, 4, 0, 0);
