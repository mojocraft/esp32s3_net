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

static int sockfd = -1;

static int connect_socket(void) 
{
	struct zsock_addrinfo hints, *res, *p;
	int ret = 0;

	memset(&hints, 0, sizeof(struct zsock_addrinfo));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	ret = zsock_getaddrinfo("www.baidu.com", "443", &hints, &res);
	if (ret != 0) {
		LOG_ERR("Get address information failed %d", ret);
		return -1;
	}

	for(p = res; p != NULL; p = p->ai_next) {
		sockfd = zsock_socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (sockfd == -1) {
			LOG_ERR("sockfd get failed %d", sockfd);
			continue;
		}
		LOG_INF("Get socket successful!");
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

void http_thread_entry(void *a, void *b, void *c)
{
	ARG_UNUSE(a);
	ARG_UNUSE(b);
	ARG_UNUSE(c);

	int ret = 0;
	k_sleep(K_SECONDS(20));
	ret = connect_socket();
	if (ret = -1) {
		LOG_ERR("Connect socket failed!");
	} else {
		LOG_INF("Connect socket done!");
	}
	while (1) { 
		k_msleep(200);
	}

}

K_THREAD_DEFINE(myhttp, 4096, http_thread_entry,NULL, NULL, NULL,5, 0, 0);
