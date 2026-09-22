#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>

#include <zephyr/posix/poll.h>
#include <zephyr/posix/arpa/inet.h>

#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(mqtt_sub, LOG_LEVEL_INF);

#define MQTT_THREAD_STACK_SIZE 4096
#define MQTT_THREAD_PRIORITY   5

/* ================= MQTT CONFIG ================= */

#define BROKER_IP       "192.168.1.77"
#define BROKER_PORT     1883

#define CLIENT_ID       "esp32s3_01"

#define SUB_TOPIC       "esp32s2/percent"

#define KEEPALIVE_S     60

/* ================================================= */

static struct mqtt_client client;
static struct sockaddr_storage broker;

static uint8_t rx_buf[512];
static uint8_t tx_buf[512];
/* 载荷必须用独立 buffer: rx_buf 是 MQTT 库的接收缓冲区, 载荷本身
 * 就存在里面, 再往同一个 buffer memcpy 是重叠拷贝, 会损坏载荷 */
static uint8_t payload_buf[256];

static struct pollfd fds[1];

static bool mqtt_connected;

/*
 * Network ready flag
 */
static bool network_ready = false;

static struct net_mgmt_event_callback net_cb;

/*
 * MQTT packet id generator
 */
static atomic_t msg_id = ATOMIC_INIT(0);

static uint16_t mqtt_msg_id(void)
{
	uint16_t id;
	do {
		id = (uint16_t)(atomic_inc(&msg_id) & 0xffff);
	} while (id == 0);

	return id;
}

/*
 * =================================================
 * Network event callback
 * =================================================
 */
static void net_event_handler(struct net_mgmt_event_callback *cb, uint32_t event, struct net_if *iface)
{
	if(event == NET_EVENT_IPV4_ADDR_ADD) {
		LOG_INF("IPv4 address obtained");
		network_ready = true;
	}
}

static void wait_network_ready(void)
{
    	net_mgmt_init_event_callback(&net_cb, net_event_handler, NET_EVENT_IPV4_ADDR_ADD);
    	net_mgmt_add_event_callback(&net_cb);
	LOG_INF("Waiting network...");

    	while(!network_ready) {
        	k_sleep(K_SECONDS(1));
   	}
    	LOG_INF("Network ready");
}

/*
 * Broker address
 */
static int mqtt_broker_init(void)
{
    	struct sockaddr_in *broker4;

	broker4 = (struct sockaddr_in *)&broker;
	memset(&broker,0,sizeof(broker));

	broker4->sin_family = AF_INET;
	broker4->sin_port = htons(BROKER_PORT);
	if(inet_pton(AF_INET, BROKER_IP, &broker4->sin_addr) != 1) {
		LOG_ERR("Invalid broker IP");
		return -EINVAL;
	}
	return 0;
}

/*
 * MQTT subscription
 */
static struct mqtt_topic topics[] =
{
    	{
        	.topic = {
            		.utf8 = (const uint8_t *)SUB_TOPIC,
            		.size = sizeof(SUB_TOPIC)-1,
        	},
        	.qos = MQTT_QOS_0_AT_MOST_ONCE,
    	},
	{
		.topic = {
			.utf8 = (const uint8_t *)"computer/set/led",
			.size = sizeof("computer/set/led") - 1,
		},
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
	}
};


static struct mqtt_subscription_list subscription = {
	.list = topics,
	.list_count = ARRAY_SIZE(topics),
	.message_id = 0,
};

/*
 * MQTT callback
 */
static void mqtt_event_handler(struct mqtt_client *c, const struct mqtt_evt *evt)
{
	int ret;
	switch(evt->type) {
	case MQTT_EVT_CONNACK:
		if(evt->result != 0) {
		    LOG_ERR("CONNACK failed %d", evt->result);
		    mqtt_connected=false;
		    return;
	}
	LOG_INF("MQTT connected");
        mqtt_connected=true;
        subscription.message_id = mqtt_msg_id();
        ret = mqtt_subscribe(c, &subscription);
        if(ret < 0) {
		LOG_ERR("subscribe failed %d", ret);
        }
	break;

    case MQTT_EVT_SUBACK:
	LOG_INF("SUBACK received id=%d", evt->param.suback.message_id);
        break;

    case MQTT_EVT_PUBLISH: {
        const struct mqtt_publish_param *p = &evt->param.publish;
        uint32_t len = p->message.payload.len;
        LOG_INF("MQTT message received.");
        // LOG_INF("topic:%.*s", p->message.topic.topic.size, p->message.topic.topic.utf8);
        if(len < sizeof(payload_buf)) {
		ret = mqtt_readall_publish_payload(c, payload_buf, len);
		if(ret >= 0) {
			payload_buf[len]=0;
			LOG_INF("[topic: %.*s]: %s", p->message.topic.topic.size, p->message.topic.topic.utf8, payload_buf);
		}
	}
	break;
    }

    case MQTT_EVT_DISCONNECT:
	LOG_WRN("MQTT disconnected");
        mqtt_connected=false;
        break;

    default:
        break;
    }
}

/*
 * MQTT thread
 */
static void mqtt_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	int ret;
	LOG_INF("MQTT thread start");
	/*
	* 1. wait WiFi + DHCP
	*/
	wait_network_ready();

	/*
	* 2. broker init
	*/
	ret = mqtt_broker_init();

	if(ret < 0) {
		return;
	}

	/*
	* 3. MQTT init
	*/
	mqtt_client_init(&client);

	client.broker = &broker;
	client.evt_cb = mqtt_event_handler;
	client.client_id.utf8 = (uint8_t *)CLIENT_ID;
	client.client_id.size = sizeof(CLIENT_ID)-1;
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;
	client.keepalive = KEEPALIVE_S;
	client.rx_buf = rx_buf;
	client.rx_buf_size = sizeof(rx_buf);
	client.tx_buf = tx_buf;
	client.tx_buf_size = sizeof(tx_buf);

	LOG_INF("MQTT client set done.");

	/*
	* 4. connect retry
	*/
	while(1) {
		ret = mqtt_connect(&client);
		if(ret == 0) {
		    LOG_INF("MQTT connect OK");
		    break;
		}
		LOG_ERR("mqtt_connect failed %d", ret);
		k_sleep(K_SECONDS(5));
	}
	/*
	* 5. MQTT loop
	*/
	fds[0].fd = client.transport.tcp.sock;
	fds[0].events = ZSOCK_POLLIN;
	while(1) {
		int timeout;
		timeout = mqtt_keepalive_time_left(&client);
		if(timeout < 0) {
			timeout=1000;
		}

		ret = poll(fds, 1, timeout);

		if(ret > 0) {
		    if(fds[0].revents & ZSOCK_POLLIN) {
			ret = mqtt_input(&client);
			if(ret < 0) {
			    LOG_ERR("mqtt_input error");
			    break;
			}
		    }
		}
		if(mqtt_connected) {
			ret = mqtt_live(&client);
			if(ret < 0 && ret != -EAGAIN) {
				LOG_ERR("mqtt_live error");
				break;
			}
		}
	}
	mqtt_abort(&client);
}


K_THREAD_DEFINE(
        mqtt_tid,
        MQTT_THREAD_STACK_SIZE,
        mqtt_thread,
        NULL,
        NULL,
        NULL,
        MQTT_THREAD_PRIORITY,
        0,
        0);
