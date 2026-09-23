#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/mqtt.h>

#include <zephyr/posix/poll.h>
#include <zephyr/posix/arpa/inet.h>

#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(http_client, LOG_LEVEL_INF);

static struct net_mgmt_event_callback net_event_cb_structure;
static struct sockaddr_in broker;
static struct mqtt_client client;

static uint8_t rx_buffer[512];
static uint8_t tx_buffer[512];

static uint8_t payload_buffer[512];

static bool mqtt_connected;
static bool network_ready = false;

static struct pollfd fds[1];

static void net_event_handler(struct net_mgmt_event_callback *cb, uint32_t event, struct net_if *iface)
{
	if (event == NET_EVENT_IPV4_ADDR_ADD) {
		LOG_INF("IPv4 address obtained.");
		network_ready = true;
	}
}

static void wait_network(void) 
{
	net_mgmt_init_event_callback(&net_event_cb_structure, net_event_handler, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&net_event_cb_structure);
	LOG_INF("Waiting Network...");
	
	while (!network_ready) {
		k_sleep(K_SECONDS(1));
	}
	LOG_INF("Network Ready!");
}

static int broker_init(void) 
{
	int ret = 0;
	broker.sin_family = AF_INET;
	broker.sin_port = htons(1883);
	ret = inet_pton(AF_INET, "192.168.1.77", &broker.sin_addr);
	if (!ret) {
		LOG_ERR("Invalid broker IP");
		return -EINVAL;
	}
	return 0;
}

static atomic_t msg_id = ATOMIC_INIT(0);

static uint16_t generate_mqtt_id(void)
{
	uint16_t id;
	do {
		id = (uint16_t)(atomic_inc(&msg_id) & 0xffff);
	} while (id == 0);	
	
	return id;
}

static struct mqtt_topic topics[] = {
	{
		.topic = {
			.utf8 = (const uint8_t *)"esp32_s2/percent",
			.size = sizeof("topic1") - 1,
		},
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
	},
	{
		.topic = {
			.utf8 = (const uint8_t *)"topic2",
			.size = sizeof("topic2") - 1,
		},
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
	},
	{
		.topic = {
			.utf8 = (const uint8_t *)"topic3",
			.size = sizeof("topic3") - 1,
		},
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
	},
};  

static struct mqtt_subscription_list subscription = {
	.list = topics,
	.list_count = ARRAY_SIZE(topics),
	.message_id = 0,
};

static void mqtt_event_handler(struct mqtt_client *client, const struct mqtt_evt *evt)
{
	int ret;
	switch (evt->type) {
		case MQTT_EVT_CONNACK: {
			LOG_INF("MQTT waiting ack.");
			if (evt->result) {
				LOG_INF("Ack failed");
				mqtt_connected = false;
				return;
			}
			LOG_INF("MQTT connect successful");
			mqtt_connected = true;
			subscription.message_id = generate_mqtt_id();
			ret = mqtt_subscribe(client, &subscription);
			if (ret < 0) {
				LOG_ERR("subscribe failed: %d", ret);
			}
			break;
		}

		case MQTT_EVT_DISCONNECT: {
			LOG_INF("MQTT Disconnect");
			mqtt_connected = false;
			break;
		}

		case MQTT_EVT_SUBACK: { // Acknowledgment to a subscribe request.
			LOG_INF("Suback received id = %d", evt->param.suback.message_id);
			break;
		}

		case MQTT_EVT_PUBLISH: {
			LOG_INF("MQTT message received.");
			const struct mqtt_publish_param *p = &evt->param.publish;
			uint32_t len = p->message.payload.len;
			if (len < sizeof(payload_buffer)) {
				ret = mqtt_readall_publish_payload(client, payload_buffer, len);	
				if (ret >= 0) {
					payload_buffer[len] = 0;
					LOG_INF("Topic: %.*s ==> %s", p->message.topic.topic.size, p->message.topic.topic.utf8, payload_buffer);
					
				} else {
					LOG_ERR("MQTT read all publish payload failed: %d", ret);
				}
			} else {
				LOG_ERR("Payload is too large: %u >= %zu, dropped", len, sizeof(payload_buffer));
			}
			break;
		}

		default:
			break;
	}
}

static void client_init(struct mqtt_client *c)
{
	mqtt_client_init(c);
	
	c->broker = &broker;
	c->evt_cb = mqtt_event_handler;
	c->client_id.utf8 = (uint8_t *)"esp32_mqtt_client";
	c->client_id.size = sizeof("esp32_mqtt_client") - 1;
	c->password = NULL;
	c->user_name = NULL;
	c->protocol_version = MQTT_VERSION_3_1_1;
	c->transport.type = MQTT_TRANSPORT_NON_SECURE;
	c->rx_buf = rx_buffer;
	c->tx_buf = tx_buffer;
	c->rx_buf_size = sizeof(rx_buffer);
	c->tx_buf_size = sizeof(tx_buffer);
}

static void test_thread_entry(void *arg1, void *arg2, void *arg3)
{
	int ret;
	LOG_INF("test thread started.");	

	wait_network();
	broker_init();
	client_init(&client);

	while (1) {
		ret = mqtt_connect(&client);
		if (ret == 0) {
			LOG_INF("MQTT connect done");
			break;
		}
		LOG_ERR("MQTT connect failed");
		mqtt_abort(&client);
		k_sleep(K_SECONDS(1));
	}

	fds[0].fd = client.transport.tcp.sock;
	fds[0].events = ZSOCK_POLLIN;
	
	
	while (1) {
		int timeout = mqtt_keepalive_time_left(&client);
		if (timeout < 0) 
			timeout = 1000;

		ret = poll(fds, 1, timeout);
		
		if (ret < 0) {
			LOG_ERR("Poll error: %d", errno);
			break;
		}
		if (ret > 0 && (fds[0].revents & ZSOCK_POLLIN)) {
			ret = mqtt_input(&client);
			if (ret < 0) {
				LOG_ERR("mqtt input error");
				break;
			}
		}
		
		
		if (mqtt_connected) {
			ret = mqtt_live(&client);
			if (ret < 0 && ret != -EAGAIN) {
				LOG_ERR("mqtt live error");
				break;
			}
		}
	}

	mqtt_abort(&client);
}

K_THREAD_DEFINE(test, 4096, test_thread_entry, NULL, NULL, NULL, 5, 0, 0);
