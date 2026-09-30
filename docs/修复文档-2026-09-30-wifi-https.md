# WiFi/HTTPS 与服务器通信故障修复文档

- 日期:2026-09-30
- 设备:ESP32-S3(Zephyr v3.7.2,板卡 `esp32s3_net/esp32s3/procpu`)
- 服务器:建管平台 `https://jgyw.crfsdi.com.cn:51111`(接口文档:锚注一体机接口文档.docx)

---

## 1. 问题现象

固件运行时 minicom 日志显示:

```
[00:00:07] <inf> wifi: wifi connecting...
[00:00:09] <err> esp32_wifi: Wi-Fi disconnect reason: 201   ← 扫描找不到 AP
[00:00:11] <inf> wifi: wifi connect busy (-1), ignore        ← 之后所有重试都被拒
[00:00:18] <inf> wifi: wifi connect busy (-1), ignore
[00:00:20] <err> net_sntp: Failed to send over UDP socket -1
[00:00:20] <err> http: SNTP failed: -1, TLS will likely fail
```

表面现象是"WiFi 连不上、无法与服务器通讯",实际排查后共发现 **3 个独立 bug**,分属驱动、事件派发、TLS 配置三个层面,逐一修复并烧录实测通过。

---

## 2. 根因链

| # | 层次 | 根因 | 后果 |
|---|---|---|---|
| 1 | WiFi 驱动 | `CONFIG_ESP32_WIFI_STA_RECONNECT=y` 时,驱动断开事件后把状态锁死在 `ESP32_STA_CONNECTING` 并内部自动重连;此后应用层所有连接请求被拒(busy -1),内部重连又静默无果 | 一次扫描失败(201)后**永远卡死**,拿不到 IP |
| 2 | net_mgmt 事件派发 | 回调掩码混入两个网络层的事件(L2 的 WIFI + L3 的 IPV4),派发器按 LAYER 位**精确相等**过滤,导致该回调**静默失效**——断开事件收不到,重试线程只能靠 15s 超时兜底 | 修复 #1 后暴露:断开后要 15s 才能重试,且"连接成功"标志永远置不上 |
| 3 | mbedTLS | `MBEDTLS_SSL_MAX_CONTENT_LEN=4096` 小于服务器证书链握手消息(实测 **4758 字节**),mbedTLS 3.6 报 `-0x7100`(BAD_INPUT_DATA,"requesting more data than fits") | 修复 #1/#2 后暴露:TLS 握手失败,`Connect socket failed! Code: -1` |

---

## 3. 修复内容

### 3.1 prj.conf

| 修改 | 位置 | 原因 |
|---|---|---|
| `CONFIG_ESP32_WIFI_STA_RECONNECT=n`(原 =y) | 第 23 行 | 见根因 #1。驱动自动重连是卡死 busy 的元凶。关掉后驱动断开即回到 `ESP32_STA_STARTED`,由 `wifi.c` 自己的重试循环接管(该循环原本就有,且每次重试都是全新全信道扫描) |
| `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=8192`(原 4096) | 第 80 行 | 见根因 #3。8192 足够容纳 4758 字节的证书链,也为后续较大的响应记录留出余量 |
| `CONFIG_NET_SOCKETS_CONNECT_TIMEOUT=10000`(原默认 3000) | 第 86 行 | Zephyr 3.7 的 TLS 握手在 `zsock_connect()` 内完成,默认 3s 上限在弱网下容易超时,放宽到 10s |

### 3.2 src/wifi.c

| 修改 | 位置 | 原因 |
|---|---|---|
| 新增第二个回调 `ipv4_mgmt_cb` + `ipv4_event_handler()`(第 21、63–78 行),监听 `NET_EVENT_IPV4_ADDR_ADD/DEL` | — | ① 见根因 #2:L2/L3 事件必须**分回调注册**,否则两个层的事件都匹配不上(掩码 LAYER 位是"或",和任何单一层都不相等,且无任何报错);② `CONFIG_ESP32_WIFI_STA_AUTO_DHCPV4=y` 时驱动连接成功后**不 raise CONNECT_RESULT**(`esp_wifi_drv.c` 的 `esp_wifi_handle_sta_connect_event` 只有启动 DHCP 的分支),原代码里"等 CONNECT_RESULT 成功"是死代码——真正可通信的标志是 DHCP 拿到 IPv4 地址 |
| 注册拆成两个 `net_mgmt_init_event_callback` + `net_mgmt_add_event_callback`(第 100–107 行) | — | 同上,分层注册 |

行为变化:连接成功 = 收到 `IPV4_ADDR_ADD`;断线/地址丢失 = `IPV4_ADDR_DEL` 或 `DISCONNECT_RESULT`;两者都会 give 信号量唤醒重试线程,重试节奏从"15s 超时兜底"变为"事件驱动即时重试"(约 4.4s 一轮)。

### 3.3 src/http.c

| 修改 | 位置 | 原因 |
|---|---|---|
| 新增 `net_if.h`/`net_mgmt.h`/`sys/atomic.h` 头文件 | 第 8–14 行 | 支持 IP 事件等待 |
| 新增 `ip_event_handler` + `ipv4_available()` + `network_up()`(第 34–55 行):注册 `IPV4_ADDR_ADD` 事件回调 + 直接检查 `net_if` 上是否有 DHCP 地址 | — | 网络就绪判断,替代原代码盲睡 20s(网络慢/AP 弱时 20s 不够,网络正常时又白等) |
| SNTP 从"失败即 `return`"改为"最多试 3 次、失败仅告警继续"(第 194–208 行) | — | 当前 `TLS_PEER_VERIFY_NONE` 不校验证书时间,SNTP 不是硬依赖;原逻辑在 UDP 123 被封锁的环境下会让整个 HTTPS 流程直接夭折 |
| 线程主体改为循环:**等网络 → HTTPS 流程 → 10s 后重测**(第 177–240 行),网络中断后自动回到等待 | — | 原逻辑等 60s 拿不到 IP 就永久退出;实测工地 AP 可能 70s+ 才连上,一次性等待会错过。循环版本网络一旦恢复立即执行请求,且每 10s 持续重测 |
| HTTPS 请求失败重试 3 次(每次间隔 5s),`http_client_req` 超时 2000ms→10000ms(第 212–232 行) | — | 弱网下 TLS 握手 + 首次请求耗时不可控,短超时容易误伤 |
| 修正两处写反的日志文案:`set SEC_TAG_LIST` 与 `set TLS_HOSTNAME` 对调(第 85、94 行) | — | 原文案与实际 setsockopt 调用相反,排查时严重误导 |

---

## 4. 验证结果

三处修复均通过**实际烧录 + 串口抓取日志**验证:

1. 关掉 `STA_RECONNECT` 后:重试不再 busy,每次断开后都能重新发起连接;
2. 拆分回调后:`wifi disconnected` 事件即时到达,重试间隔由 17s(15s 超时+2s)变为 ~4.4s(2.4s 扫描+2s);
3. `MAX_CONTENT_LEN=8192` 后:TLS 握手通过。

最终运行日志(烧录后全自动):

```
[00:00:11] <inf> wifi: wifi connecting...
[00:00:15] <inf> wifi: wifi connected (IPv4 acquired)   ← 第二次尝试即连上
[00:00:16] <inf> http: Network ready (IPv4 acquired)
[00:00:16] <inf> http: SNTP synced, time=1790742991
[00:00:17] <inf> http: Partial data received (512 bytes)
[00:00:17] <inf> http: Response status OK               ← GET /backendapi/auth/publicKey
[00:00:17] <inf> http: All the data received (75 bytes) ← 共 592 字节,公钥 JSON
[00:00:27] <inf> http: Response status OK               ← 每 10s 重测,持续成功
```

服务器侧兼容性此前已用 openssl/curl 逐一验证(可达性、TLS 1.2、`TLS_RSA_WITH_AES_128_CBC_SHA256` 密码套件、证书链 `*.crfsdi.com.cn` → CFCA OV OCA → CFCA EV ROOT 与设备内 `cfca_root_der` 匹配),均无问题。

---

## 5. 遗留事项与建议

1. **AP 信号偏弱**:首扫常出现 201(NO_AP_FOUND)/39(关联超时),说明设备位置处 AP 信号处于边缘。当前固件已能自动恢复(事件驱动重试),但若装进金属机柜,需注意天线位置/外接天线。
2. **证书验证目前是关闭的**(`TLS_PEER_VERIFY_NONE`)。若后续改为 `TLS_PEER_VERIFY_REQUIRED`,SNTP 时间同步将重新成为硬依赖(证书有效期校验),届时 SNTP 失败策略需同步收紧。
3. **下一步业务开发**(按接口文档):GET publicKey → RSA 公钥加密密码 → POST login 获取 access_token → POST batchSave 上报锚注数据。
