#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

// #include <zephyr/posix/sys/socket.h>
// #include <zephyr/posix/unistd.h>
// #include <zephyr/posix/arpa/inet.h>

#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/http/client.h>

#include <string.h>
#include <errno.h>

#include "cfca_root.h"

LOG_MODULE_REGISTER(http, LOG_LEVEL_INF);

#define CA_CERTIFICATE_TAG 	1
#define HTTPS_HOSTNAME 		"jgyw.crfsdi.com.cn"
#define HTTPS_PORT 		"51111"

static int sockfd = -1;
static uint8_t recv_buf[512];

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
			LOG_ERR("set TLS_HOSTNAME: %s", strerror(errno));
			zsock_close(sockfd);
			sockfd = -1;
			continue;
		}
		
		/* 设 SIN hostname (通配符证书 *.crfsdi.com.cn 必需) */
		ret = zsock_setsockopt(sockfd, SOL_TLS, TLS_HOSTNAME, HTTPS_HOSTNAME, strlen(HTTPS_HOSTNAME));
		if (ret < 0) {
			LOG_ERR("set SEC_TAG_LIST: %s", strerror(errno));
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

	k_sleep(K_SECONDS(20));

	/* 连接,内部用IPPROTO_TLS_1_2 */
	sockfd = connect_socket();
	if (sockfd == -1) {
		LOG_ERR("Connect socket failed!");
		return;
	} else {
		LOG_INF("Connect socket done!");
	}
	
	struct http_request req = { 0 };
	req.method = HTTP_GET;
	req.url = "/backendapi/auth/publicKey";
	req.host = HTTPS_HOSTNAME; 
	req.protocol = "HTTP/1.1";
	req.response = response_cb;
	req.recv_buf = recv_buf;
	req.recv_buf_len = sizeof(recv_buf);

	ret = http_client_req(sockfd, &req, 2000, "IPv4 GET");
	if (ret < 0) {
		LOG_ERR("Client error %d", ret);
	}

	zsock_close(sockfd);

	while (1) { 
		k_msleep(200);
	}

}

K_THREAD_DEFINE(myhttp, 8192, http_thread_entry,NULL, NULL, NULL,5, 0, 0);
