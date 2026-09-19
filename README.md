# esp32s3_net — Zephyr ESP32-S3 WiFi 工程

ESP32-S3 + Zephyr 的 WiFi 连接工程，WiFi 连接逻辑运行在独立线程中，
基于事件驱动 + 信号量实现自动重连状态机。

## 1. 工程结构

```
esp32s3_net/
├── boards/espressif/esp32s3_net/    # 板级定义（PROCpu/APPcpu 双核板）
│   ├── Kconfig.defconfig            # 板级默认配置（含 WiFi 所需堆内存默认值）
│   ├── Kconfig.esp32s3_net          # 板级 Kconfig 符号
│   └── esp32s3_net_procpu_defconfig # PROCpu 镜像板级配置
├── prj.conf                         # 应用配置（WiFi/网络栈/Shell）
├── src/
│   ├── main.c                       # 主线程（占位测试代码）
│   ├── wifi.c                       # WiFi 连接状态机（wifi 线程）
│   ├── mqtt.c / mqtt.h              # MQTT 客户端
│   └── wifi.h                       # （暂为空，暂无对外 API）
└── CMakeLists.txt
```

> 说明：当前构建的是 **PROCpu 单镜像**，WiFi 驱动直接跑在 PROCpu 上
> （ESP-IDF 的 WiFi 任务默认也在 PRO_CPU 上，架构上没有问题）。
> 板级文件是从上游 esp32s3_devkitc 板复制改造的，注意 Kconfig 符号名已改为
> `BOARD_ESP32S3_NET_ESP32S3_PROCPU` / `BOARD_ESP32S3_NET_ESP32S3_APPCPU`。

## 2. 构建与烧录

```sh
# 首次配置 / 换板子 / 全量重建（-p = pristine）
west build -b esp32s3_net -d build

# 日常增量编译（改源码、prj.conf、设备树后均可直接用）
ninja -C build

# 烧录
west flash
```

`west build` 与 `ninja -C build` 的关系：

| | west build | ninja -C build |
|---|---|---|
| 干什么 | 完整流程：定位 ZEPHYR_BASE/工具链 → CMake 配置（选板/应用）→ 调 ninja 编译 | 只在已配置好的 build 目录做增量编译 |
| CMake 配置阶段 | 每次都会跑（有缓存，较快） | 跳过（除非 CMakeLists 等依赖变化，ninja 会自动重新生成） |
| 何时用 | 首次配置、换板子/应用、pristine 重建、west flash 烧录 | 日常改代码后快速编译 |

Zephyr 特有点：prj.conf / Kconfig / 设备树的改动都编进了 ninja 依赖图
（.config、autoconf.h、edt 是编译目标），所以改配置后直接 `ninja -C build`
也能正确触发配置重新生成。

## 3. Wi-Fi 配置流程

配置生效的层级关系：

```
板级 Kconfig.defconfig（堆内存等硬件相关默认值）
        ↓ 叠加
prj.conf（应用配置，可覆盖板级默认）
        ↓ 生成
build/zephyr/.config（最终生效配置）
```

### 3.1 关键配置项（prj.conf）

| 配置项 | 值 | 作用 |
|---|---|---|
| `CONFIG_WIFI` / `CONFIG_WIFI_ESP32` | y | 启用 WiFi 子系统与 ESP32 驱动 |
| `CONFIG_NETWORKING` / `CONFIG_NET_L2_ETHERNET` | y | 网络栈 + 以太网 L2（WiFi 基于它） |
| `CONFIG_NET_IPV4` / `CONFIG_NET_IPV6` | y / n | 只用 IPv4 |
| `CONFIG_NET_DHCPV4` | y | DHCP 客户端 |
| `CONFIG_ESP32_WIFI_STA_AUTO_DHCPV4` | y | 连上 AP 后驱动自动跑 DHCP |
| `CONFIG_ESP32_WIFI_STA_RECONNECT` | y | 驱动层（HAL）自动重连，应用层重连只是兜底 |
| `CONFIG_ESP32_WIFI_IRAM_OPT` / `CONFIG_ESP32_WIFI_RX_IRAM_OPT` | n | 关闭 IRAM 优化，省 IRAM（WiFi 能正常工作的前提条件之一） |
| `CONFIG_SHELL` / `CONFIG_SHELL_BACKEND_SERIAL` | y | Shell 与串口后端 |
| `CONFIG_NET_L2_WIFI_SHELL` | y | **WiFi shell 命令**（旧版本叫 `WIFI_SHELL`），不加则 `wifi scan` 等命令不存在 |
| `CONFIG_NET_MGMT_EVENT_QUEUE_SIZE` | 16 | 网络管理事件队列深度（防扫描结果事件丢失） |
| `CONFIG_NET_MGMT_EVENT_QUEUE_TIMEOUT` | 5000 | 事件在队列中的超时（ms） |
| `CONFIG_NET_BUF_DATA_SIZE` | 1600 | **数据块大小必须 ≥1514**（一帧一个 buffer）。默认 128 时一帧拆成 12 个碎片，突发流量瞬间耗尽池（见 §5.5） |
| `CONFIG_NET_PKT_RX_COUNT` / `CONFIG_NET_BUF_RX_COUNT` | 32 | RX 包/缓冲池数量（广播环境要大） |
| `CONFIG_NET_PKT_TX_COUNT` / `CONFIG_NET_BUF_TX_COUNT` | 16 | TX 包/缓冲池数量 |
| `CONFIG_NET_TX_STACK_SIZE` / `CONFIG_NET_RX_STACK_SIZE` | 2048 | 网络收发线程栈 |

### 3.2 堆内存（重要）

**不要在 prj.conf 里显式设置 `CONFIG_HEAP_MEM_POOL_SIZE`！**

ESP WiFi HAL 的所有内存（RX/TX buffer、WPA supplicant、内部结构）都从
Zephyr 系统堆分配（`esp_wifi_adapter.c` 的 `wifi_malloc` → `k_malloc`）。
堆大小由板级 `Kconfig.defconfig` 提供：

```kconfig
# boards/espressif/esp32s3_net/Kconfig.defconfig
if BOARD_ESP32S3_NET_ESP32S3_PROCPU
config HEAP_MEM_POOL_ADD_SIZE_BOARD
	default 65535 if WIFI && BT
	default 51200 if WIFI      # ← WiFi 至少需要 50KB
	default 40960 if BT
	default 4096
endif
```

生效机制（kernel/Kconfig 中 `HEAP_MEM_POOL_SIZE` 的语义）：
实际堆大小 = max(`HEAP_MEM_POOL_SIZE`, 各 `HEAP_MEM_POOL_ADD_SIZE_*` 之和)，
前提是 `CONFIG_HEAP_MEM_POOL_IGNORE_MIN` 未启用。

历史上踩过的坑见 [§5.1](#51-esp_wifi_start-内存分配失败)。

## 4. Wi-Fi API 调用流程关系

### 4.1 模块结构

```
┌────────────────────────────────────────────────────────────┐
│ wifi 线程 (wifi_thread_entry, K_THREAD_DEFINE)             │
│                                                            │
│  启动时（只做一次）:                                        │
│   net_mgmt_init_event_callback(&wifi_mgmt_cb, handler,     │
│       CONNECT_RESULT | DISCONNECT_RESULT)                  │
│   net_mgmt_add_event_callback(&wifi_mgmt_cb)               │
│                                                            │
│  状态机循环:                                                │
│   ├─ 未连接: wifi_connect_once() → 等 wifi_event_sem(2s)   │
│   └─ 已连接: 阻塞等 wifi_event_sem → 退避 2s → 重连         │
└────────────────────────────────────────────────────────────┘
        │ net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params)
        ▼
┌────────────────────────────────────────────────────────────┐
│ 网络管理栈 (subsys/net) → 驱动 esp32_wifi_connect()         │
│ 异步：返回 0 只表示请求已提交，真正结果走事件                 │
└────────────────────────────────────────────────────────────┘
        │ NET_EVENT_WIFI_CONNECT_RESULT / DISCONNECT_RESULT
        ▼（系统工作队列上下文，异步）
┌────────────────────────────────────────────────────────────┐
│ wifi_event_handler:                                        │
│  cb->info → struct wifi_status                             │
│  成功/失败/掉线 → atomic_set 状态 + k_sem_give 唤醒线程     │
└────────────────────────────────────────────────────────────┘
```

### 4.2 完整调用链

```
wifi 线程循环
 └─ wifi_connect_once()
     ├─ net_if_get_default()          # 每次重取，NULL → -ENODEV 退避重试
     └─ net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params))
         └─ 驱动 esp32_wifi_connect()
             ├─ esp_wifi_set_mode / esp_wifi_start / esp_wifi_set_config
             └─ esp_wifi_connect()    # 异步，返回 0 = 已提交
                 └─ (HAL 内部连接中……)
                     ├─ 成功: WIFI_EVENT_STA_CONNECTED
                     │   → 驱动 raise NET_EVENT_WIFI_CONNECT_RESULT(status=0)
                     ├─ 失败: raise NET_EVENT_WIFI_CONNECT_RESULT(status=err)
                     └─ 掉线: raise NET_EVENT_WIFI_DISCONNECT_RESULT
```

### 4.3 时序

```
wifi线程                net_mgmt/驱动              事件回调(工作队列)
   │ net_mgmt(CONNECT)      │                           │
   ├───────────────────────>│                           │
   │                        │ esp_wifi_connect() 异步    │
   │ k_sem_take(2s 超时)    │   ── STA_CONNECTED ──────> │
   │                        │                           │ CONNECT_RESULT(0)
   │<── k_sem_give ─────────┴───────────────────────────┤
   │ atomic connected = 1   │                           │
   │ k_sem_take(FOREVER)    │                           │
   │      …… 已连接，线程阻塞，不占 CPU ……               │
   │                        │   ── DISCONNECTED ───────> │
   │<── k_sem_give ─────────┴───────────────────────────┤ DISCONNECT_RESULT
   │ 退避 2s → 重连          │                           │
```

### 4.4 状态机

```
        ┌──────────────────────────────┐
        │        未连接 (connected=0)   │
        └──────────────┬───────────────┘
                       │ wifi_connect_once()
                       │ 成功提交 → 等事件(2s 超时兜底)
        ┌──────────────▼───────────────┐
        │     等结果 (CONNECTING)       │
        └───┬──────────────────────┬───┘
   结果=0   │                      │ 结果≠0 / 超时 / -EALREADY
        ┌───▼────────────┐         └─────► 退避重试（回到未连接）
        │ 已连接 (connected=1) │
        └───┬────────────┘
            │ DISCONNECT_RESULT
            ▼ 退避 2s 后回到未连接
```

### 4.5 关键设计点

1. **异步语义**：`net_mgmt(NET_REQUEST_WIFI_CONNECT)` 返回 0 只表示请求被驱动接受，
   连接成功与否必须等 `NET_EVENT_WIFI_CONNECT_RESULT` 事件（`cb->info` 为
   `struct wifi_status`，`status->status` 非 0 为失败）。
2. **iface 每次重取**：`net_if_get_default()` 放在 `wifi_connect_once()` 内，
   不缓存；接口未就绪返回 `-ENODEV` 由外层退避。
3. **跨线程状态用 `atomic_t`**：`wifi_connected` 在事件回调（工作队列上下文）写、
   wifi 线程读，必须原子操作。
4. **事件丢失兜底**：等待事件信号量带 2s 超时，超时后重发请求（驱动处于
   CONNECTING 时会返回 `-EALREADY`，属正常现象，继续退避即可）。
5. **驱动层 vs 应用层重连**：`CONFIG_ESP32_WIFI_STA_RECONNECT=y` 时驱动（HAL）是
   自动重连的主路径；应用层状态机只是兜底（初始连接失败、`WIFI_REASON_ASSOC_LEAVE`
   等驱动放弃的场景、事件丢失）。退避时间别设太短，避免两层重连互相打架。
   驱动收到重连请求时返回 `-EALREADY` 并 raise 一个 status=-1 的"假失败"事件，
   这是正常现象。
6. **回调只注册一次**：`net_mgmt_init_event_callback` + `net_mgmt_add_event_callback`
   在 wifi 线程入口执行一次；重复注册同一结构体会破坏回调链表。

### 4.6 连接参数（wifi_connect_req_params）

| 字段 | 值 | 说明 |
|---|---|---|
| `ssid` / `ssid_length` | `SSID` / `sizeof(SSID)-1` | 网络名（wifi.c 顶部宏定义） |
| `psk` / `psk_length` | `PASSWORD` / `sizeof(PASSWORD)-1` | 密码 |
| `channel` | 0 | 0 = 全信道扫描（`WIFI_CHANNEL_ANY`） |
| `security` | `WIFI_SECURITY_TYPE_PSK` | WPA/WPA2-PSK |
| `band` | `WIFI_FREQ_BAND_2_4_GHZ` | 2.4GHz |
| `mfp` | `WIFI_MFP_OPTIONAL` | 管理帧保护：AP 支持就用，不强制 |

> MFP = Management Frame Protection（IEEE 802.11w），防伪造 deauth 帧攻击。
> 注意：当前 ESP32 驱动的 PSK 分支把 `pmf_cfg` 写死了
> （`esp_wifi_drv.c` 中 `pmf_cfg.required = false`，未按 `params->mfp` 设置
> `capable`），所以这个字段目前实际不生效。

### 4.7 WiFi shell 使用

固件启用 `CONFIG_NET_L2_WIFI_SHELL=y` 后，串口 shell 可用：

```sh
wifi scan            # 扫描 AP（结果是异步打印的，先看到 "Scan requested"）
wifi status          # 接口状态
wifi disconnect      # 断开
```

注意：
- 扫描结果通过事件回调异步输出，命令本身不阻塞；
  事件丢失时只看到 "Scan requested" 而没有结果（见 §5.3）。
- 驱动同一时刻只支持一个扫描任务，上一次未结束时再次 `wifi scan`
  会报 "Scan request failed"（驱动返回 `-EINPROGRESS`）。

## 5. 踩坑记录

### 5.1 esp_wifi_start 内存分配失败

**现象**：开机 `esp32_wifi_adapter: memory allocation failed` →
`Failed to start Wi-Fi driver`，之后每次请求都报 `Failed to get Wi-Fi mode (12289)`
（12289 = 0x3001 = `ESP_ERR_WIFI_NOT_INIT`），`net_mgmt` 返回 -11（-EAGAIN）。

**根因**（两个 bug 叠加）：
1. prj.conf 里显式写了 `CONFIG_HEAP_MEM_POOL_SIZE=8192`，堆只有 8KB，
   远不够 WiFi HAL 用（至少 50KB）。
2. 板级 `Kconfig.defconfig` 从 esp32s3_devkitc 抄来时守卫条件没改：
   `if BOARD_ESP32S3_DEVKITC_ESP32S3_PROCPU` 与自己的板符号
   `BOARD_ESP32S3_NET_ESP32S3_PROCPU` 不匹配，导致 51200 的堆默认值从未生效。

**修复**：改守卫符号名 + 删除 prj.conf 中的 8192 覆盖。
验证：`.config` 中 `CONFIG_HEAP_MEM_POOL_ADD_SIZE_BOARD=51200`，
RAM 报告中 `kheap__system_heap 51200 B`。

### 5.2 wifi shell 命令不存在

`wifi scan` 报 command not found：这个 Zephyr 版本的配置项叫
**`CONFIG_NET_L2_WIFI_SHELL`**（旧版叫 `CONFIG_WIFI_SHELL`，写旧名会被静默忽略）。
只配 `CONFIG_SHELL` / `CONFIG_SHELL_BACKEND_SERIAL` 不会有 wifi 命令。

### 5.3 扫描结果事件丢失

`wifi scan` 发出后没有结果输出：扫描结果靠 `NET_EVENT_WIFI_SCAN_RESULT` /
`NET_EVENT_WIFI_SCAN_DONE` 事件异步回传，事件队列太浅会丢。
对策：`CONFIG_NET_MGMT_EVENT_QUEUE_SIZE=16`、`CONFIG_NET_MGMT_EVENT_QUEUE_TIMEOUT=5000`。

### 5.4 启动阶段 connect failed (-120) / -1 报错噪音

**现象**：开机后出现 `wifi disconnected (reason 39)` → `wifi connect failed -1` →
`connect request failed (-120)` 若干次，随后连接成功。

**解释**：
- `reason 39` = `WIFI_REASON_TIMEOUT`，首次连接在驱动刚上电/PHY 未稳定时发出，
  超时失败 → 驱动层自动重连（STA_RECONNECT 对非 ASSOC_LEAVE 的所有 reason 生效）
- `-120` = `-EALREADY`（注意 Zephyr libc 里 EALREADY=120，不是 Linux 的 114）：
  应用层重试撞上驱动正在 CONNECTING
- `wifi connect failed -1`：驱动在 CONNECTING 状态下收到连接请求时 raise 的
  "假失败"事件（esp_wifi_drv.c 的 `esp32_wifi_connect`），属于正常现象
- 日志里的英文 `Disconnected` / `Connected` / `Connection request failed (-1)`
  是 **wifi shell 模块**自己打印的（wifi_shell.c），不是 wifi.c 的输出

**对策**：wifi 线程首次连接前 `k_sleep(K_SECONDS(2))` 等驱动稳定；
事件等待超时取 **15s**（首次连接含全信道扫描，实测 >10s，太短会超时后
撞上 CONNECTING）；`-EALREADY` 返回时静默退避 5s（驱动已在连接，急也没用）；
status=-1 的假失败事件降级为 INF 日志。偶发仍属正常，状态机自愈。

### 5.5 Data buffer (1514) allocation failed 丢帧

**现象**：连接正常、网络可用一段时间后突然报
`net_pkt: Data buffer (1514) allocation failed` → `Failed to allocate net buffer`，
RX 丢帧。

**根因**：`CONFIG_NET_BUF_DATA_SIZE` 默认 128 字节，而 WiFi 帧最长 1514 字节，
**一帧要拆成 12 个碎片**（net_pkt.c `pkt_alloc_buffer` 逐片链）。而 RX 池只有
10 个 buffer（合计 1280B）—— **连一个全尺寸帧都装不下**：任何 ≥1281B 的单帧
（SSDP/mDNS 公告、路由器探测新设备、大 TCP 段）到达都必然报错丢弃。
**不需要"突发风暴"，一帧就够。** 可用 `ping -s 1400` 大包必现复现。

> 注意：128 不是不能用 —— esp32s2_net 工程（DRAM 98.83% 无余量）一直用
> 128×10 跑裸 MQTT 也没事，因为 MQTT 小帧（CONNACK/PUBACK 等 <128B）
> 不会拆碎片，且丢的广播帧靠协议自愈无感。但 **TLS 握手、OTA、大 payload
> 这类全尺寸帧场景必须 ≥1514**，否则随机丢帧/握手失败。数据块大小的选择
> 本质是 DRAM 预算的取舍。

**修复**：`CONFIG_NET_BUF_DATA_SIZE=1600`（一帧一个 buffer）+ RX 池 32 个。
代价是 RX 数据池占 ~51KB DRAM，当前 DRAM 总占用约 76%（244KB/313KB），
再加功能时注意余量。

### 5.6 其他

- **连接结果是异步的**：`net_mgmt` 返回 0 ≠ 连上了，必须等事件（§4.5.1）。
- **凭据硬编码**：SSID/密码目前写在 wifi.c 顶部宏里（已提交进 git），
  后续应迁移到 Kconfig 或 settings 子系统。
- **log 后端**：`CONFIG_LOG_BACKEND_UART=n` 时 LOG_* 输出没有串口后端
  （shell 与 log 共用 UART 时的取舍），调试排障时注意。
