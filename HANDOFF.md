# ESP32-C3 AirPrint 猫打印机 — 交接文档

> 最后更新：2026-11-24 22:12
> 工程根目录：`/Users/dujianan/WorkBuddy/2026-11-24-20-31-59/esp32c3-airprint-catprinter/`

---

## 一、一句话现状

**编译、烧录、启动、BLE、IPP/mDNS 全部验证通过；唯一不稳定项是这块 C3 与该 360 路由器的 2.4G 关联 —— 有时 7 秒内连上并稳定（拿到 192.168.1.x，mDNS/IPP 全起），有时连续 `reason=2/201/205` 半天连不上。已确认与密码、5GHz、信号强度、PMF 无关的部分全部排除，剩下的是路由器侧（802.11ax/双频合一/多 BSSID）行为。**

关键实测（同一固件、不同次上电）：

```
# 成功的一次（PMF capable 打开后）
I (9857) app: WiFi connected: 192.168.1.19 via bssid c8:bf:4c:3f:31:d3 ch=6 rssi=-60
I (9864) mdns_adv: advertising catprinter._ipp._tcp at catprint-c3.local:631
I (9870) http_ipp: IPP server listening on :631/ipp/print
I (9878) app: print server up: AirPrint name 'catprinter'
I (9919) app: BLE: scanning for printer... (prefix GB01)

# 失败的一次
W (4120) app: WiFi disconnected (reason=2 ssid=360), attempt 1     ← auth expire

# 锁 BSSID 后反而更差（该 AP 掉到 -76，driver 快速扫描找不到）
W (5485) app: WiFi disconnected (reason=201 ssid=360), attempt 1    ← NO_AP_FOUND
```

已排除（都有实测依据，别再重查）：
- 密码不对 → 驱动回读 `ssid='360' pwlen=11`；且同一密码成功连过并拿到 IP
- 5GHz 问题 → 扫到两个 2.4G BSSID（ch1/ch6），`phy: 11b=1 11g=1 11n=1 11ax=0`
- 信号弱 → 最强 BSSID 曾达 -32 dBm
- PMF 缺失 → 加 `pmf_cfg.capable=true` 后首次即连通（这个是真修好的一项）
- BSSID 选错 → 锁定后更差，已撤销锁定，改为全信道扫描 + 按信号排序
- 固件「重试 10 次就永久放弃」→ 已改成独立任务里指数退避、永不放弃

---

## 二、环境（已验证）

| 项 | 值 |
|---|---|
| ESP-IDF | **v6.0.1**，源码在 **`/Users/dujianan/Coding/esp-idf`**（depth 1 + 全 submodule，645 MB） |
| 工具链 | `~/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204` |
| Python | 3.11.15，venv `~/.espressif/python_env/idf6.0_py3.11_env` |
| CMake / Ninja | pip 装的 cmake 4.4.5 / ninja 1.13 |
| 串口 | `/dev/cu.usbmodem1101` 当前在线（烧录端口） |

### 环境脚本

工程根的 `idf_env.sh`（已更新指向新 IDF，包含 `IDF_SKIP_CHECK_SUBMODULES=1`、`IDF_COMPONENT_MANAGER=0`）。

**必须用 bash 调用**，因为本机默认 shell 是 zsh，脚本里的 `esp-clang/*/...` 通配符无匹配会让 zsh 直接中止 source：

```bash
cd /Users/dujianan/WorkBuddy/2026-11-24-20-31-59/esp32c3-airprint-catprinter
bash -c 'source ./idf_env.sh && "$VENV_PY" "$IDF_PATH/tools/idf.py" build'
# 其它子命令同理：set-target / menuconfig / -p /dev/cu.usbmodem1101 flash monitor / size
```

---

## 三、kconfgen 挂起的真正根因（重要，别再走弯路）

**macOS TCC 拒绝了本机 agent 进程访问 `~/Documents` 和 `~/Desktop`：**

```
/Users/dujianan/Documents  → EPERM (errno 1 Operation not permitted)
/Users/dujianan/Desktop    → EPERM
/Users/dujianan/Coding     → OK
```

原来的 IDF 放在 `~/Documents/code/bell-esp32-C3/esp-idf`，于是所有由它 fork 出来的子进程（`git`、`cmake`、`kconfgen`、编译器）读源码全部 EPERM：外在表现就是「配置阶段长时间不动」和「`git submodule status` 永久挂住」。

所以之前怀疑的 Kconfig 循环、组件注册表不可达、BASH_ENV 污染**都不是**原因；代码和 Kconfig 一直没毛病。

**解法（已生效）**：在可访问目录重新拉一份 IDF → `~/Coding/esp-idf`。

> 直连 GitHub 只有 0.4–1 MB/s；`export https_proxy=http://127.0.0.1:7897` 后 7.9 MB/s，3 分钟拉完。以后 clone 都记得带代理。

---

## 四、本轮改了什么代码

### 1. `components/catprinter/catprinter.c` — 适配 IDF 6 / NimBLE 1.5 的 API

| 旧写法（编译报错） | 新写法 |
|---|---|
| `uuid16_value(&chr->uuid)` | `uuid16_value(&chr->uuid.u)`（`chr->uuid` 现在是 `ble_uuid_any_t`） |
| `ble_gap_connect(&addr, &params, 10000, cb, NULL)` | `ble_gap_connect(CP_OWN_ADDR_TYPE, &addr, 10000, &params, cb, NULL)`（新签名 6 参数，`duration_ms` 在 params **之前**） |
| `BLE_GAP_ADDR_TYPE_PUBLIC / _RANDOM_STATIC` | `BLE_OWN_ADDR_PUBLIC / BLE_OWN_ADDR_RANDOM`（`ble.h`，且这参数是**本机**地址类型，不是对端的 —— 原语义是错的） |
| `#include "services/gap/ble_svc_gap.h"` + `ble_svc_gap_init()` | 删除。本 build 是 central-only（`CONFIG_BT_NIMBLE_ROLE_PERIPHERAL/GATTS` 关），gap service 不参与编译，调用必然 undefined reference |

### 2. `main/app_main.c` — 页缓冲改为「提前 + 降级」分配

原来 `pb_init()` 在 WiFi + BLE 都起来之后才 `malloc` 103 KiB 连续内存，C3 上大概率 `ESP_ERR_NO_MEM`。现在：

- `pagebuf_init_best_effort()` 在 NVS 之后、WiFi/BLE 之前分配；
- 依次尝试 2200 / 1100 / 550 行，失败降级而不是 abort（长页会被截断，但不会拒绝整单）；
- 启动日志打印实际行数与 `esp_get_free_heap_size()`，真机可直接看余量。

实测：`I (408) app: page buffer: 2200 rows (103 KiB), free heap 145388` —— 提前分配成功。

### 3. `main/app_main.c` — WiFi 不再阻塞整个启动 + 内置 WiFi 诊断

原来 `xEventGroupWaitBits(..., portMAX_DELAY)` 意味着 WiFi 连不上就永远不初始化 BLE，设备完全哑掉。现在：

- 等 WiFi 最多 25 s（`WIFI_WAIT_MS`），超时后继续初始化 BLE，**BLE 打印不依赖 IP**；
- `ps_init()` 提前到 WiFi 之前（只记录名字和回调），IPP/mDNS 在 `IP_EVENT_STA_GOT_IP` 回调里启动，且带 `s_ps_started` 去重 —— WiFi 后到也能自动补上；
- 断连日志打印 **reason 码 + SSID**，最多重试 10 次后停止刷屏；
- 新增 `wifi_scan_dump()`：连接前先扫一遍目标 SSID，打印 `ch / rssi / authmode / bssid`，并回读 `esp_wifi_get_config()` 确认 SSID 没被截断（只打印密码长度，不打印密码）。

这套诊断直接把问题定死在「AP 侧拒绝认证」，而不是靠猜。

---

## 五、实测数据（本轮真跑出来的）

```
build/airprint_catprinter.bin  0x113170 (1,126,768 B)
app 分区 0x1e0000 → 余 0xcce90 (43%)
bootloader.bin 0x5250，余 36%

DRAM  127,475 / 321,296 B (39.68%)，剩 193,821 B
  .text 92,774 / .bss 20,432 / .data 14,269
Flash code 869,310 B，Flash data 149,928 B

host_test: ALL PROTOCOL TESTS PASSED
  start_printing (9): 51 78 a3 00 01 00 00 00 ff
```

sdkconfig 关键项已确认生效：`BT_ENABLED`、`BT_NIMBLE_ENABLED`、`BT_NIMBLE_ROLE_CENTRAL`、`BT_NIMBLE_ATT_PREFERRED_MTU=256`（协商后约 247，故单包有效载荷 `MTU-3`）、`ESP_COEX_SW_COEXIST_ENABLE`、`MDNS_MAX_SERVICES=4`、`PARTITION_TABLE_CUSTOM`、`AIRPRINT_*` / `CATPRINTER_*` 全部为空默认值（**WiFi SSID 还是空字符串**）。

---

## 六、下一步（按优先级）

1. **先配 WiFi，再烧**
   ```bash
   bash -c 'source ./idf_env.sh && "$VENV_PY" "$IDF_PATH/tools/idf.py" menuconfig'
   # AirPrint Cat Printer → WiFi SSID / 密码 / 打印机名 / BLE 名称前缀(GB01|MX06|X6h)
   bash -c 'source ./idf_env.sh && "$VENV_PY" "$IDF_PATH/tools/idf.py" -p /dev/cu.usbmodem1101 flash monitor'
   ```

2. **真机验证顺序**：串口能看到 `WiFi connected: <ip>` → `BLE: printer ready: tx=… rx=… mtu=…` → iPhone 隔空打印列表出现 `CatPrint-C3`。记下真实 MTU。

3. **已知设计缺口（未改）**
   - `on_print_job` 在 httpd handler 里同步跑完整个 BLE 打印（最长 180 s），期间 IPP 服务不响应其它请求（iOS 轮询 job-state 会超时）。要稳就该拆独立打印任务 + 队列。
   - `urf.c` 两个 TODO 仍在：CS→bpp 映射要靠真机抓包定死；RLE(PackBits) 路径未实现，遇到压缩流会直接报错。
   - 每行 20 ms 的固定节流：800 行 ≈ 16 s、2200 行 ≈ 44 s。真机上建议改成「有流控通知才停，无通知不等待」，否则打长图会显得像卡死。
   - `CONFIG_AIRPRINT_USE_FLASH_SPOOL` 只是 Kconfig 占位，flash spool 代码没写。
   - `sdkconfig.defaults` 里几个 C3 不存在/已改名的项（如 `CONFIG_ESP32_WIFI_SW_COEXIST_ENABLE`）会被 kconfgen 静默忽略，不影响构建。

---

## 九、WiFi 关联排查结论（mesh/中继环境，2026-11-24 夜）

现场网络：SSID `360`，由**两个中继组成 mesh**，扫到两个 BSSID：

| BSSID | 信道 | 实测 RSSI | 行为 |
|---|---|---|---|
| `c8:bf:4c:38:70:dd` | ch1 | -32 ~ -47 | **认证阶段就被丢弃**（reason=2 AUTH_EXPIRE），从未连上 |
| `c8:bf:4c:3f:31:d3` | ch6 | -60 ~ -76 | 认证能过；多数次在关联阶段超时（reason=4 ASSOC_EXPIRE）；偶尔完整连上（用户实测 8 s 拿到 192.168.1.19，随后掉线） |

两块 BSSID 同属 `c8:bf:4c` 厂牌，authmode=3（WPA2-PSK），`phy: 11b/11g/11n=1, 11ax=0`，country=??。

固件侧已做（都无法单独修好，但都是正确设置）：
`pmf_cfg.capable=true`、全信道扫描 + 按信号排序、`threshold.authmode=WPA2_PSK`、`threshold.rssi=-100`、强制 20 MHz、`WIFI_PS_NONE`、`ft_enabled`+`btm_enabled`、指数退避永不放弃重连、BSSID 锁定开关（默认留空）。

**结论：两端节点对这块 11n-only 客户端都不友好 —— 一个在 auth 丢弃、一个在 assoc 丢弃。这是 mesh 节点侧的加密/漫游协商问题，不是 ESP 代码问题。** 需要在 mesh APP 里统一并简化：

1. 两个节点的 **加密方式与密码必须完全一致**，都选 **WPA2-PSK(AES)**，关闭 WPA2/WPA3 混合、关闭「强加密/802.11w 强制」
2. 关闭 **WiFi 6 / 802.11ax 模式**（走 11n 兼容模式）
3. 关闭「双频合一」，给 2.4G 一个独立 SSID，C3 只连这个
4. 关闭「快速漫游 802.11r/k/v」或设为可选
5. 检查中继是否开了「客户端隔离/接入限制/防蹭网」

若上述都改过仍不稳，做一次隔离实验即可定性：用 iPhone 个人热点（单独 2.4G）给 C3 连 —— 能稳定连上 → 板子与固件无问题，问题 100% 在 mesh；连热点也不稳 → 回头查板子供电/USB 供电噪声。

---

## 十、★ 最终根因：SuperMini 必须限制发射功率（2026-11-24 深夜破案）

**现象全家桶**：能扫到 AP、连接报 `reason=2 AUTH_EXPIRE` / `reason=4 ASSOC_EXPIRE` / `reason=15`，
偶尔连上并拿到 IP 但几秒后断开；把它烧成 softAP 时手机贴着也搜不到；两块板表现一致。

**根因**：ESP32-C3 **SuperMini** 在满功率（`80` = 20 dBm）下，板载陶瓷/PCB 天线失真、反射过大，
认证帧与 4-way 握手帧被毁 → AP 根本收不到（所以是 auth expire，不是密码错）。
参考：阿里云社区《ESP C3 Super Mini 踩坑记：WiFi 能搜到却连不上？降低功率试试》。

**修复（已生效，实测）**：

```c
// 必须在 esp_wifi_start() 之后再调用（否则返回 ESP_ERR_WIFI_NOT_STARTED），
// 且在 esp_wifi_connect() 之前。
esp_wifi_set_max_tx_power(CONFIG_AIRPRINT_TX_POWER_QDBM);   // 默认 34 = 8.5 dBm
```

实测日志：

```
I (546) app: max tx power (default): 80 (20.00 dBm)
I (549) app: max tx power (now): 34 (8.50 dBm), set rc=ESP_OK
I (6933) app: WiFi connected: 192.168.1.21 via bssid c8:bf:4c:38:70:dd ch=6 rssi=-43
I (6953) app: print server up: AirPrint name 'catprinter'
```

配套验证：ping 0% 丢包（13–51 ms）、`curl http://192.168.1.21:631/` = 200、`catprint-c3.local` 解析正常、
60 秒窗口内无任何 `WiFi disconnected`。

真 IPP 验证（Mac → 设备，POST /ipp/print，Get-Printer-Attributes）：

```
HTTP/1.1 200 OK   application/ipp   421 bytes
version=0x0101 status=0x0000 (OK) request-id=1
  pdl-supported              = image/urf
  urf-supported              = CP1,CR1,SRGB24,RS300,FB8
  printer-resolution-default = 300x300 dpi
  printer-state              = 3 (idle), printer-is-accepting-jobs = true
```

> Kconfig：`AirPrint Cat Printer → AIRPRINT_TX_POWER_QDBM`（range 8–84，默认 34）。
> 距离不够时再往上调（52=13dBm，68=17dBm），**不要一上来给 80**。
> 排查过程中曾把功率拉到 78，反而彻底连不上 —— 这是反方向，别再试。

### 附带修掉的自有 bug：IPP `POST /ipp/print` 挂住

`http_ipp.c` 的 `read_body()` 原先「一直 recv 到失败」，小 body（~100 B）读完后会阻塞在 socket 超时上，
导致 iPhone/Mac 的 IPP 请求 20 秒以上无响应。已改为**按 `req->content_len` 精确读取**，超时则重试。

### 附带修掉的自有 bug 2：mDNS 服务名多了一个 `_tcp`（iPhone 搜不到打印机的真凶）

`mdns_adv.c` 里写成 `mdns_service_add(NULL, "_ipp._tcp", "_tcp", 631, ...)`。
IDF 的 API 是 `service_type="_ipp"` + `proto="_tcp"`，于是实际广播成
`catprinter._ipp._tcp._tcp.local` —— 任何 AirPrint 客户端都不会查这个名字，
**A 记录能解析、ping/curl 都通，但 iPhone 打印面板永远空白**，极容易被误判成网络/防火墙问题。

修正后实测：

```
$ dns-sd -B _ipp._tcp local
  Add local.  _ipp._tcp.  catprinter

$ dns-sd -L catprinter _ipp._tcp local
  catprinter._ipp._tcp.local. can be reached at catprint-c3.local.:631
  txt: pdl=image/urf URF=CP1,CR1,SRGB24,RS300,FB8 rp=ipp/print qtotal=1 txtvers=1

$ dns-sd -Q _universal._sub._ipp._tcp.local PTR
  _universal._sub._ipp._tcp.local. PTR catprinter._ipp._tcp.local.
```

同时 `mdns_service_remove("_ipp", "_tcp")` 也要用同样的拆分形式。

## 十一、★ AirPrint 全链路打通记录（2026-11-25 凌晨，iPhone 实测出纸）

最终可用参数（全部已固化进 Kconfig）：

| 项 | 值 | 说明 |
|---|---|---|
| WiFi 发射功率 | `CONFIG_AIRPRINT_TX_POWER_QDBM=34` (8.5 dBm) | SuperMini 满功率会打死 WiFi |
| BLE 名称前缀 | `CONFIG_CATPRINTER_BLE_NAME_PREFIX="PD01"` | |
| BLE 写间隔 | `CONFIG_CATPRINTER_WRITE_DELAY_MS=30` | 无响应写，太快会丢行 |
| 打印头能量 | `CONFIG_CATPRINTER_ENERGY=0xFFFF` | 与参考实现 pdprint.swift 一致 |
| 打印中走纸速度 | **不设置**（只在 lattice 结束后 `0xBD 08`） | 设慢 = 每点受热过多 = 糊 |
| 二值化 | **纯阈值 128**，无 Floyd-Steinberg | FS 会把文字边缘打碎成噪点 |
| 缩放 | 区域**取最暗**（横向 + 纵向） | 平均会让 6.5 倍下采样的文字变灰雾 |
| 页缓冲 | 1200 行（56 KiB） | A4 缩放后约 543 行 |
| 文档传输 | 边收边解码进页缓冲，**不落 RAM 也不落 flash** | 13 KB 堆装不下，擦 flash 会拖死 WiFi |

打通路上共 6 个根因（每个都曾单独让打印失败）：

1. **mDNS 服务类型多写 `_tcp`** → 广播成 `xxx._ipp._tcp._tcp.local`，A 记录能解析但 iPhone 列表空白
2. **IPP 响应版本写死 1.1**，iOS 用 **IPP 2.0** → 响应被判无效，iOS 无限重试 Get-Printer-Attributes（表现为每秒十几个轮询、永不提交任务）
3. **TXT 记录不全**：`kind=thermal` 是非法值（只能 document/photo/envelope/label），`URF` 缺 `W8,DM1`，
   缺 `ty`/`product`/`Color`/`Duplex` → iOS 能选中但点击打印无反应
4. **`esp_http_server` 读不了 chunked 请求体**（解析阶段就吞掉 body，`content_len=0`，`httpd_req_recv` 又被
   `remaining_len=0` 截断）→ 自写最小 TCP 服务器（`ipp_server.c`），Content-Length 与 chunked 都自己解，
   开 `TCP_NODELAY`，2 个连接 worker + 队列（iOS 轮询会淹没单线程）
5. **IPP 属性 tag 值全错**（0x21/0x22/0x23/0x47/0x48/0x49 是 RFC 8011 的值，我们写成了 0x47/0x46/0x48/...）
   → iOS 解析不出打印机能力，拒绝选中
6. **★ URF 解码状态机 bug（最隐蔽）**：PackBits 的 `0x00..0x7F` 是「**一个**像素字节 + 重复次数」，
   状态机误按「每输出一个点读一个像素字节」处理，把后续操作码当成像素吃掉 → 整页错位成条纹。
   排查手法：把解码后的点阵以 ASCII 打到串口（`CONFIG_AIRPRINT_DEBUG_RASTER_DUMP=y`），
   再用 Python 忠实复刻 C 状态机跑同一个 .urf 文件对比 —— 两边的 ASCII 图一致才说明解码器无问题。

验证脚本（Mac 侧，不走 iPhone）：

- `/tmp/print_poem.py` —— 渲染《静夜思》成 384 宽 URF 并打印（384 = 打印头点数，无缩放，最干净的驱动测试）
- `/tmp/print_pattern.py` —— 几何图案（左右半黑/黑白条/每 8 点竖线/边框），判定镜像与位序

iPhone 端到端：打印面板出现 `catprinter` → 选中 → 点打印 → 设备日志出现
`op=0x0004`（Validate-Job）→ `op=0x0002`（Print-Job）→ `urf: URF 2480x3507 ...` → `decoded 543 rows`
→ `print progress` → `BLE: printer ready`，纸上出字。

---

## 七、命令速查

```bash
cd /Users/dujianan/WorkBuddy/2026-11-24-20-31-59/esp32c3-airprint-catprinter

# 编译
bash -c 'source ./idf_env.sh && "$VENV_PY" "$IDF_PATH/tools/idf.py" build'

# 体积/RAM
bash -c 'source ./idf_env.sh && "$VENV_PY" "$IDF_PATH/tools/idf.py" size'

# 只跑协议单测（不需要 IDF）
cd host_test && make test

# 从零重建（换机器时）
rm -rf build sdkconfig
bash -c 'source ./idf_env.sh && "$VENV_PY" "$IDF_PATH/tools/idf.py" set-target esp32c3'
bash -c 'source ./idf_env.sh && "$VENV_PY" "$IDF_PATH/tools/idf.py" build'
```

---

## 八、验证结果汇总

| 检查项 | 结果 |
|---|---|
| 协议单测（CRC、帧字节、位反转、小端、边界） | ✅ 通过 |
| `idf.py set-target esp32c3` | ✅ 通过（sdkconfig 91 KB 正常生成） |
| `idf.py build` | ✅ 通过，`airprint_catprinter.bin` 已生成 |
| 静态 RAM | ✅ DRAM 39.68%，剩 193 KB（页缓冲 103 KB 可在早期分配） |
| 烧录（`idf.py flash` @460800） | ✅ 通过，hash verified |
| 真机启动日志（C3 rev0.4 / 4MB） | ✅ 正常启动，页缓冲 103 KB，BLE 扫描，IPP :631 监听 |
| WiFi 关联 | ✅ 已连上 192.168.1.21（早期 reason=2/205 的排查见第一节） |
| iPhone 隔空打印 | ✅ AirPrint 可打印；URF 解码与 BLE 出纸内容正确 |
| 连续打印（打完一次再打） | ✅ 已修复 IPP 作业状态机，见第九节 |

---

## 九、打完一次后 iOS 显示「打印机不再可用」（2026-11-25 13:44–14:05）

**现象**：第一次打印成功后，iPhone 打印对话框显示打印机不可用 / 一直转，约 3 分钟后才肯再提交。

**不是**网络或 BLE 挂了：设备 ping 通、`curl` 发 IPP 返回 200、`dns-sd -L` 正常解析、BLE 未断；日志里 `job 2 done` 后下一次 `Print-Job` 隔了 186 秒，期间 iOS 轮询 37 次 `Get-Printer-Attributes`，但 `job 3` 最终也完整打印成功。所以是 **IPP 作业状态没正确收尾，iOS 打印中心进入重试退避**。

**根因（`components/printserver/ipp_server.c` 三处语义错误）**

1. `Get-Jobs` 在作业完成后仍然返回那个已完成作业 → iOS 认为队列永远没清空。
2. `job-uri` 三处硬编码 `ipp://...?job=1`，而 `job-id` 递增 → 第一次打印之后 URI 与实际作业不匹配。
3. `Validate-Job` 复用 `build_job_ack()`，回了一个 `job-id=0, job-state=processing` 的假作业；且 `job-state=9` 时 `job-state-reasons` 写 `"none"`（不是终态原因）。

此外 `printer-state` / `queued-job-count` 用 `ps_job_state()==5` 判断，与 `Get-Jobs` 的结果可能互相矛盾。

**修复**

- `printserver.c` / `printserver.h`：新增 `volatile bool s_job_active` 与 `ps_job_active()`；`ps_dispatch_job()` 在 **HTTP ACK 发出之前**就发布 `s_last_job / s_job_state=5 / s_job_active=true`，消除 print worker 尚未被调度时轮询读到上一作业终态的竞态；handler 返回后置 `false`。
- `ipp_server.c`：新增 `add_job_group()` 统一生成作业属性组（`job-uri` 用真实 job-id；终态 reason 用 `job-completed-successfully` / `job-completed-with-errors` / `job-canceled-by-user`）；`Get-Jobs` 仅在 `ps_job_active()` 时列作业；`Validate-Job` 只回操作属性；`build_job_ack` 的 `job-uri` 动态化；`printer-state` 与 `queued-job-count` 改用 `ps_job_active()`，保证三者一致。
- HTTP 层：`body_read()` 在 `sz==0` 时只在缓冲已有数据时跳过尾部 CRLF（绝不阻塞，否则每次轮询 +2 s）；`ipp_worker` 在 `close(fd)` 前先 `shutdown(fd, SHUT_WR)`，避免未读 trailer 触发 TCP RST 让 iOS 丢弃已收到的响应。

**验证**（chunked IPP 请求 + 真实 8 行小 URF 文档，共走纸约 6 cm）

| 检查 | 结果 |
|---|---|
| Print-Job ACK | `job-id=1 / job-uri=?job=1 / job-state=3` |
| 打印中 | `printer-state=4`、`queued-job-count=1`、`Get-Jobs` 有 1 个作业（`state=5`） |
| 打印完成 | `printer-state=3`、`queued-job-count=0`、**`Get-Jobs` 空**、`Get-Job-Attributes = 9 / job-completed-successfully` |
| Validate-Job | 无 job 组（不再伪造作业） |
| 连续 3 次作业 | 每次 ACK 延迟 **0.36–0.38 s**，完成 0.5–0.75 s，队列每次立刻清空，串口均 `job N done` + `BLE: printer ready` |

编译与三分区烧录通过，固件 `build/airprint_catprinter.bin` = 0x1103a0 (1,114,272 B)。

> 若 iPhone 打印中心仍显示旧状态，在 iOS「设置 → 打印机」中删除并重新添加该打印机以刷新缓存。

---

## 十、"进度条走完很久才开始打" —— 上传慢（2026-11-25）

**真实 iPhone 日志证据**（`/tmp/monitor_before_rxopt.log`，设备启动后毫秒数）：

| 时刻 | 事件 |
|---|---|
| 459013 | `op=0x0002`（Print-Job 的 IPP 头到达） |
| 461895 | `URF 2480x3507 ... row_step 6.46`（头到齐，比 IPP 头晚 2.88 s） |
| 479038 | `decoded 543 of 3507 rows` |
| 479040 | `job 4 queued (543 rows, 175335 doc bytes, 20023 ms upload)` |
| 479307 | `BLE: printing`（距 queued 仅 **267 ms**） |
| 498356 | `job 4 done` |

**结论**：20.0 秒全部花在上传 175 KB 的 A4@300dpi URF 光栅（8.76 KB/s）；上传一结束到 BLE 开打只有 0.27 s；BLE 打印 543 行约 19 s，属正常热敏速度。上传期间 iOS 并发轮询约 9 次。

**根因**：`components/printserver/ipp_server.c` 的 `RD_BUF=512`，`recv()`/`body_read()` 每轮最多 512 B，175 KB 需 ~350 次收发，每轮吃一次 WiFi 往返。

**改动**
- `RD_BUF` 512 → **2048**；`brd_t.buf` 改成指针，指向 `worker_ctx_t.rxbuf[RD_BUF]`（静态区，不占 6 KB 任务栈）；`body_feed_urf()` 复用 `ctx->pre` 作解码输入缓冲。
- `body_feed_urf()` 每 2 s 打 `rx <bytes> B / <ms>`，EOF 打 `rx done`；`job N queued` 日志拆成 `upload / decode / total` 三段。
- `handle_client()` 尝试 `TCP_QUICKACK`（`#ifdef` 保护；**此 IDF 的 lwIP 未导出该宏，实际未生效**）；`TCP_NODELAY` 保留。

**压测**（175,171 B chunked POST，1024 B/HTTP chunk，连续发送）

| | 旧固件（真实 iPhone） | 新固件（压测） |
|---|---|---|
| 收 175 KB | 20,023 ms | **3,568 ms** |
| 速率 | 8.76 KB/s | **48.8 KB/s（5.6×）** |
| queued → BLE printing | 267 ms | 254 ms |

进度 `rx 82944 B / 2024 ms` → `rx done 174147 B / 3568 ms`，稳定 41–59 KB/s，无长停顿。客户端 `send=1.529 s / ack_wait=2.069 s / total=3.598 s`。

IPP 状态机回归通过：`printer-state=3`、`queued-job-count=0`、`Get-Jobs` 空、`Get-Job-Attributes=9 / job-completed-successfully`、`Validate-Job` 无 job 组。固件 `0x110540` (1,115,456 B)，已烧录，串口监控在跑。

**再压测发现设备侧已到天花板**：175 KB 连续灌入只要 3.5 s（48.8 KB/s），真实 iPhone 却只有 8.76 KB/s。用 `SO_SNDBUF=2048` 模拟「每 chunk 等 ACK」直接 60 s 超时卡死 → iOS 是按 ACK 节奏分块上传的，175 KB 无论设备多快都要 ~60 个往返。

### 第二轮：加快 ACK + 减少数据量

- `CONFIG_LWIP_TCP_TMR_INTERVAL` **250 → 50**（`sdkconfig` 与 `sdkconfig.defaults` 同步）。A/B：等 ACK 压测从「60 s 超时」变成 **16.5 s 完成**，仍不够；连续压测 3.95 s 无回归。
- **`media-default` / `media-ready` 由 `iso_a4_210x297mm` 改为 `iso_a6_105x148mm`**（A4/Letter/A6/4x6 仍都在 `media-supported`）。设备最终只打印 384 点宽，A4@300dpi 的 2480×3507 光栅 99% 像素被丢弃，却要 iOS 全量渲染 + 上传。A6 面积 1/4，文字在 48 mm 小票纸上也更合适。
- `TCP_QUICKACK` 此 IDF lwIP 未导出，`#ifdef` 跳过，未生效。

**验证（A6 固件）**：`Get-Printer-Attributes` 返回 `media-default=iso_a6_105x148mm`；1240 px 宽 A6 光栅 `URF 1240x200 ... row_step 3.23` 解码正常；40 KB 文档 `2045 ms upload`，`job 1 done`，状态机回归通过（`printer-state=3`、`Get-Jobs` 空、`Get-Job-Attributes=9 / job-completed-successfully`）。固件 `0x110540` (1,115,456 B)，串口监控在跑。

> **用户需操作**：iOS 缓存打印机能力，必须到「设置 → 打印机」删除 `catprint2` 后重新添加，A6 才生效。内容会按 A6 重排（文字相对变大）。若想恢复 A4 排版，把 `ipp_server.c` 的 `media-default` / `media-ready` 两行改回 `iso_a4_210x297mm` 重新编译烧录即可。

## 十一、Mac 侧发起打印（A6 端到端验证，2026-11-25 下午）

用户不在电脑旁，由 Mac 代替 iPhone 发起打印验证 A6 优化。

**Mac 原生 AirPrint 队列走不通**：`lpadmin -m everywhere` 报"无法创建PPD文件"（设备只声明 `image/urf`，macOS 的 IPP Everywhere 生成器不认）；`-m raw` 报"macOS不再支持原始队列"。结论：命令行下 Mac 侧唯一路径是 CUPS 官方客户端 **`ipptool`**（支持 `-C` 强制 chunked，与 iOS 一致）。

**测试文档由 Mac 本地真实渲染**（`/tmp/make_urf.py`，PIL + Arial Bold）：两行文字 + 实心方块，URF 参数与真实 iPhone 完全一致（`UNIRAST`、bits=8、cs=0、bpp=1、白=0xff、行首重复字节、PackBits 变体 `c<=0x7f` 重复 / `c>=0x81` 字面量 / `0x80` 清行尾）。
- A6@300dpi：1240×1748 = **63,093 B**
- A4@300dpi：2480×3507 = **192,178 B**（与真实 iPhone 的 175,335 B 同量级，可互为印证）

**结果**（设备 192.168.1.21，固件 0x110540 A6 版）：

| 场景 | 文档 | 上传 | queued→BLE 开打 | 打印 |
|---|---|---|---|---|
| 真实 iPhone，A4（优化前对照） | 175,335 B | 20,023 ms | 267 ms | 543 行 19.2 s |
| Mac `ipptool -C` 连续发送，A6 | 63,093 B | **3,181 ms** | 260 ms | 180 行 7.8 s |
| `SO_SNDBUF=2048` 模拟 iPhone ACK 节奏，A6 | 62,224 B | **4,516 ms** | 260 ms | 180 行 8.1 s |

- 设备自动 `trimmed 361 trailing blank rows`：1748 源行解码出 541 行、尾部空白裁掉后实际只打 180 行，省纸省时。
- IPP 状态机回归通过（连打 job 2/job 3 均正常收尾，`job done` + `printer ready`）。
- **预测**：iPhone 打 A6 的「提交完成 → 开始打印」≈ **4.8 s**（原 20.3 s）；iOS 渲染 A6 光栅也比 A4 快约 4 倍，整体「点击 → 开打」预计 40 s → ~12–15 s。

> iPhone 端 A6 仍需用户在「设置 → 打印机」删除重加 `catprint2` 后才生效（能力缓存）。`/tmp/make_urf.py` 可随时再生 A6/A4 测试文档（`python3 /tmp/make_urf.py --a4`）。

## 十二、默认纸张改为 A8（52x74mm，2026-11-25 15:02）

用户要求"加上 A8，设为默认"。`ipp_server.c` 的 `media-supported` 增加 `iso_a8_52x74mm`，`media-default` / `media-ready` 由 `iso_a6_105x148mm` 改为 **`iso_a8_52x74mm`**（A4/Letter/A6/4x6 仍都可选）。

理由：打印头 384 点 @203dpi = 48mm 宽；A8 的 52mm 几乎 1:1 对应，且面积只有 A4 的 1/16，iOS 渲染 + 上传的数据量最小，文字在 48mm 小票纸上尺寸最合适。

**代价**：页高只有 74mm，长文本会被分成多页（AirPrint 是页面模型，没有连续纸概念）。若常打长内容，把 `media-default` / `media-ready` 改回 `iso_a6_105x148mm` 重编译即可。

固件 `0x110570` (1,115,504 B) 已编译烧录。验证：
- `Get-Printer-Attributes` 返回 5 个 `media-supported`（含 A8）、`media-default=media-ready=iso_a8_52x74mm`。
- Mac 本地渲染的 614×874 A8 URF = **22,179 B**（A6 63 KB 的 1/3、A4 192 KB 的 1/9）。
- 模拟 iPhone ACK 节奏提交：`rx done 21310 B in 1518 ms`（客户端 send 1.53s / total 1.54s）；`URF 614x874 row_step 1.60` 解码 546 行；queued→BLE 开打 256 ms；裁掉 311 行尾部空白后实际打印 235 行 10.1 s；`job 1 done`，`Get-Jobs` 队列清空。

**三代默认纸对比（iPhone 节奏上传耗时）**：A4 175 KB → 20.0 s；A6 63 KB → 4.5 s；**A8 21 KB → 1.5 s**。

> iPhone 仍需「设置 → 打印机」删除重加 `catprint2` 刷新能力缓存后才会看到 A8。`/tmp/make_urf.py` 现默认生成 A8 文档（加 `--a4` 生成 A4）。

## 十三、WebUI + 运行时设置 + 配网/配对（2026-11-25 16:20）

### 新增组件与文件
- `components/settings/`（新）：NVS namespace `catprint`，保存 WiFi SSID/密码/BSSID、AirPrint 名称、BLE 名称前缀、绑定 MAC + 地址类型、打印方向/浓度/速度/阈值/BLE 写入间隔/尾部走纸。**Kconfig 值作为首次启动默认值**，之后 WebUI 覆盖。
- `components/printserver/webui.c` + `webui_page.h`（新）：REST API + 单页 WebUI（10 KB HTML 常驻 flash `.rodata`，不占 DRAM）。
- `components/raster/pagebuf.c`：新增 `pb_apply_orientation()`，供真实作业与 WebUI 测试页共用。
- `components/catprinter/`：新增 `cp_scan_devices()`（被动扫描列表）、`cp_connect_mac()`（按 MAC 直连）、`cp_set_name_prefix()`、`cp_peer_mac()` / `cp_peer_addr_type()`；能量、BLE 写间隔、走纸改为运行时读取 settings。
- `main/app_main.c`：从 NVS 读配置；无 WiFi 凭据时进 SoftAP `CatPrint-Setup`（开放，192.168.4.1，含 DNS 劫持）；有绑定 MAC 时开机直连；打印前应用方向变换。
- `ipp_server.c`：**80 与 631 双端口**共用同一 accept 循环（`select`）和 2 个 worker；GET/POST `/api/*` 交给 webui，IPP 仍只认 `POST /ipp/print`。`printserver.c::ps_start()` 改为 mDNS 失败不阻断 HTTP（否则 AP 模式无法配置）。

### 访问方式
- 正常联网：`http://catprint-c3.local/` 或 `http://192.168.1.21/`（mDNS TXT 的 `adminurl` 就指向这里）。
- 配网模式：清除 WiFi 后设备开放热点 `CatPrint-Setup`，手机连上访问 `192.168.4.1`（DNS 劫持会把任意域名指到本机）。

### REST API
| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/`、`/api/status`、`/api/settings` | 页面、系统状态、当前配置 |
| GET | `/api/wifi/scan`、`/api/printer/scan` | 扫描 WiFi AP / BLE 设备（后者首项固定为"当前连接"） |
| POST | `/api/settings` | 保存打印参数（即时生效） |
| POST | `/api/wifi/connect`、`/api/wifi/forget` | 保存并重启 / 清除凭据进配网模式 |
| POST | `/api/printer/connect`、`/api/printer/bind-current`、`/api/printer/unbind`、`/api/printer/disconnect` | 按 MAC 配对 / 绑定当前 / 解绑 / 断开 |
| POST | `/api/test-print`、`/api/reboot`、`/api/factory-reset` | 64 行测试页 / 重启 / 恢复出厂 |

表单为 `application/x-www-form-urlencoded`；响应 JSON。

### 参数映射
- 方向：0 正常 / 1 旋转180°（行序反转 + 行内位反转） / 2 左右镜像 / 3 镜像+180°。
- 浓度 0–15 → 加热能量 `0x1000 + d*0x400`（**8 → 0x3000**，与验证过的默认值一致；上限 0x5000 避免糊纸）。
- 速度 1–15 → BLE 写间隔 `60 - speed*4` ms（**5 → 40 ms**，15 → 8 ms 可能丢行）。另有高级项 `write_delay` 可单独覆盖。
- 阈值 0–255 → `dither_row()` 二值化阈值（默认 150）；`feed_pixels` → 尾部走纸（默认 128）。
- AirPrint 名称 / BLE 前缀：名称重启生效，前缀立即生效。

### 验证结果（固件 0x117480，1,144,096 B，已烧录）
- 启动：`settings: loaded` 正确、80/631 双端口监听、STA `192.168.1.21`、BLE ready（MAC `D1031913DFB9`, MTU 248）、free heap ~28 KB。
- `GET /` = 200 / 10,257 B；`/api/status` 字段齐全；`/api/settings` 默认值正确（energy 12288=0x3000）。
- `/api/wifi/scan` 返回 11+ AP（含中文 SSID，JSON 转义正常）；`/api/printer/scan` 返回 13 项，首项 `PD01 (当前连接)`。
- 改参数 → GET 回读一致（darkness 10 → energy 0x3800、speed 8 → delay 28ms），NVS 跨重启保持；再恢复默认。
- `bind-current` / `unbind` 往返正确；测试页打印成功（2.2 s）。
- **真实 A8 打印回归**：`rx done 21310 B in 1197 ms`、`orientation: normal`、裁空白后 235 行、`job 1 done`。
- **`/api/wifi/connect` 保存+重启**：22 s 后设备重新联网，配置不变、BLE 自动重连。
- IPP `Get-Printer-Attributes` 仍返回 200 / 1912 B，AirPrint 未受影响。

### 未实机验证（有意保留）
**SoftAP 配网模式的切换过程**：`forget WiFi` 会让设备脱离当前局域网，需要手机连上热点才能把凭据写回；远程环境无法保证连上该热点，贸然测试会把设备留在配网模式。代码路径（`esp_netif_create_default_wifi_ap()` + DHCP + DNS hijack + `ps_start()`）已编译通过，STA 侧所有同名 API 均已实测。需要验证时：点「忘记 WiFi」→ 手机连 `CatPrint-Setup` → 打开 `192.168.4.1` → 扫描/填写 → 保存连接，设备自动重启回 STA。

## 十四、多页无缝打印（2026-11-25 下午）

### 需求
iPhone 把长内容按页拆分发送（A8 默认后长内容拆成很多页），旧固件只打第 1 页、丢弃后续页。猫打印机是连续卷纸，用户要求：多页完整打印，且页与页**拼接成连续输出（不分页、无页边距间隙）**。

### 实现
- **`urf_stream.c/h`（解码器）**：支持多页 URF。每页 32B 页头逐页解析（`US_PAGEHDR` 状态），每页丢弃**页首空白行**（iOS 渲染的顶部页边距，`page_has_content` 门控）；页完成时若还有后续页则触发 `on_page_end` 回调。踩坑：页完成分支原本会丢弃触发字节，而它恰是下一页页头的第一个字节——已改为重新处理该字节。
- **`ipp_server.c`**：`page_flush_cb()`——中间页解完后立即裁掉页尾空白（`pb_trim_trailing_blank`）、交给打印 worker、**等打完才复用缓冲解下一页**（单缓冲串行；等待期间 TCP 零窗口让 iOS 暂停发送，天然背压）。方向为正常/镜像时走此流式模式；180° 旋转需要整篇反转页序，无法流式——回退为整篇缓冲再旋转（超长截断）。新增 `ps_job_set_hold(true)` 让多页作业期间状态稳定为 processing（页间不再闪断 completed）。
- **`catprinter.c/h`**：新增 `cp_print_page_ex(rows, n, timeout, feed_pixels)`，中间页走纸 0（无缝续打），末页走纸用设置值。
- **`printserver.c/h`**：`ps_job_t.last_page`；`ps_job_set_hold/ps_job_abort/ps_pages_printed`；hold 时页打完仍保持 processing。
- **`pagebuf.c/h`**：`pb_trim_trailing_blank()`。
- **`app_main.c`**：`on_print_job` 按 `last_page` 决定保留撕纸间隙（+24 行）与是否走纸；空白页/空白尾部直接 no-op 返回 ESP_OK。

### 踩过的坑
1. 页切换字节丢失（见上）→ 页 2 头错位 1 字节报 `implausible page 196608x76800`。
2. `page_flush_cb` 忘了 `pb_clear` → 页 2 把页 1 内容再打一遍（242 行 = 121+121）。修复后页 2 只打自己的 121 行。

### 验证（固件 0x1177xx 已烧录）
- 3 页 URF（768x300@300dpi/页，自制 `/tmp/make_urf_multi.py`，经 `/tmp/send_multi_ipp.py` chunked 上传）：页 1/2/3 各 121 行独立打印，页间无走纸、无缝衔接，末页带撕纸间隙，job done 无错误；连续测 3 轮全过。
- 单页 A8 回归：job 2，502 行解码→裁剪 191 行（页首空白现在也裁掉，比旧版 235 行更省纸），正常打印。
- 作业期间 `/api/status` 轮询：active 稳定 True，结束后 False（hold 生效）。
- 上传耗时 3 页 27KB ≈ 12s（含 2 次打印暂停，TCP 背压符合预期）。

### 测试工具
- `/tmp/make_urf_multi.py`：生成 N 页 URF（`python3 /tmp/make_urf_multi.py /tmp/multi_test.urf 3`）。
- `/tmp/send_multi_ipp.py`：chunked IPP 上传任意 URF（`python3 /tmp/send_multi_ipp.py /tmp/xxx.urf 1400`）。ipptool 不能直接发裸 URF（会当测试脚本解析）。
- 串口监控：`source /tmp/idf6.sh && python3 /tmp/serial_monitor.py`（后台跑，停止用 `touch /tmp/monitor.stop`；勿用 nohup+& 脱管，会静默死掉）。

## 十五、AirPrint 名称热更新 + 纸张诊断（2026-11-25 晚）

### 用户反馈
iPhone 打印面板的纸张列表里没有 A8；在 WebUI 改了 AirPrint 名称，iPhone 搜到的仍是旧名。

### 根因（两个独立问题）
1. **旧名残留 = iPhone/macOS 的 mDNS 缓存，不是设备还广播旧名。**
   `dns-sd -B _ipp._tcp` 同时列出 `catprint2` 和 `CatPrint`，但 `dns-sd -L catprint2` 解析不出主机/端口 —— 说明它只是缓存里的 PTR 记录（设备之前用该名广播，重启/刷机时没发 goodbye，记录按 PTR TTL 最多存活 75 分钟）。iOS 列表里点它必然失败。
2. **改名原本必须重启**（`ps_init()` 开机才读 NVS），且 TXT 里 `ty` / `product` / `note` 写死 `CatPrint-C3`，即使重命名成功，能力/型号字段仍报旧名。
3. **A8 看不到是 iOS 端限制**：打印面板的尺寸列表由 iOS 内置的媒体表过滤 `media-supported` 得出，A8 太冷门常被过滤掉；固件侧声明没问题（`iso_a8_52x74mm` 已在 supported/default/ready 里）。**不影响实际出纸**：不选尺寸时用 `media-default`=A8。

### 实现
- `mdns_adv.c`：拆出 `add_service()`；新增 `ps_mdns_rename()` = `mdns_service_remove("_ipp","_tcp")`（esp-mdns 会为旧名发 goodbye）+ 立即重新 add/probe。`ty`/`product`/`note` 改为跟随配置名称（静态缓冲 `s_ty`/`s_product`）。
- `printserver.c/h`：`ps_printer_rename()`（更新 `s_instance_name` + 热改名，mDNS 不可用时降级为下次开机生效）；`ps_note_media()` / `ps_last_media()`。
- `ipp_server.c`：新增 `find_attr_string()` 扫描 IPP 属性块，Print-Job 里解析 `media` 并记日志 `job media=...`（未发送则记 `none sent`）。
- `webui.c`：保存设置时若名称变化 → `ps_printer_rename()`，提示文案改为「已重新广播 + iPhone 侧清缓存方法」；`/api/status` 新增 `print.media`。
- `webui_page.h`：状态卡新增「纸张」（显示客户端实际请求的关键字），名称标签改「保存后立即广播，无需重启」，并补一段 A8 说明。

### 验证（固件已烧录）
- 热改名：`POST /api/settings airprint_name=CatPrint-Test` → 串口 `advertising 'CatPrint-Test'` → `dns-sd -B` 立即变新名，改回 `CatPrint` 同样即时生效，全程无重启。
- TXT 跟随：`dns-sd -L CatPrint` 返回 `ty=CatPrint product=(CatPrint) note=CatPrint`。
- media 解析：`/tmp/send_ipp_media.py <urf> 1400 iso_a8_52x74mm` → 日志 `job media=iso_a8_52x74mm`，`/api/status` 返回 `print.media=iso_a8_52x74mm`，作业 21019 B / 1212 ms、502 行解码→裁剪 191 行打印、job done。
- WebUI 页面 200 / 10853 B，新增文案就位。

### iPhone 侧清缓存（固件无法代劳）
设置 → 打印机 → 删除旧条目；或重启手机 / 开关一次飞行模式；或等 PTR TTL（~75 min）自然过期。

### 测试工具
`/tmp/send_ipp_media.py`：`send_multi_ipp.py` 的变体，第 3 个参数是 `media` 关键字。

## 十六、改名后"选不了打印机"的真正根因（2026-11-25 晚二）

### 现象
在 WebUI 改了 AirPrint 名称后，iPhone 打印面板选中打印机一直转圈，随后退回「请选择打印机」。设备最终彻底失联（ping 不通、631/80 全超时）。

### 诊断过程与证据
1. 失联时 `dns-sd -L` 解析不出 SRV、`catprint-c3.local` 解析失败 → 不是 mDNS 问题，是**整套网络栈死了**。
2. 串口日志 `alive: heap 3056`：heap 从 31 KB 掉到 3 KB。同期被 `com.apple.PrintKit.PrinterTool` 的 Get-Printer-Attributes（op=0x000b）以每秒 3~4 个的频率猛刷。
3. 复位后复现：改名本身完全正常（新名 SRV 可解析、TXT 跟随、无内存泄漏，连改 5 次 heap 稳定在 23.5 KB）；40 次串行请求 / 12 个并发请求 heap 都稳定 → **不是连接数问题**。
4. 用 CUPS 官方测试套件 `ipptool get-printer-attributes.test` 一跑，真凶现形：
   - `Duplicate "operations-supported"` ×5、`Duplicate "urf-supported"` ×6、`Duplicate "media-supported"` ×4 ……
   - `server-error-internal-error (IPP begCollection值不是0个字节)`
   - `server-error-internal-error (集合中命名的IPP属性无效)`

### 根因（三个真实 bug）
1. **多值属性重复编码**：`ipp_add_attr()` 每次都写完整属性名。按 RFC 8011 §4.1.2，一个属性是「值的集合」，名字只写一次，后续值必须用**长度为 0 的名字**。CUPS 只警告，Apple PrintKit 直接判定响应非法 → 无限重试 → heap 耗尽 → 设备失联。
2. **A8 不在 iOS 内置纸张表**：`media-default` 设成 `iso_a8_52x74mm` 时，iPhone 根本建不出纸张设置。改名会让 iOS 丢弃缓存的能力并重新协商，于是"改名之后就选不了"。
3. **media-col collection 编错**：begCollection(0x34)/endCollection(0x37) 的 value 长度必须为 0，成员是平铺的普通属性。我最初把成员塞进 value，CUPS 报 "begCollection值不是0个字节"；改成平铺后 member 校验仍不过。

### 修改
- `ipp.c` / `ipp.h`：`ipp_writer_t` 增加 `last_name[40]`，`ipp_add_attr()` 检测连续同名属性并写 0 长度名字；`ipp_add_group()` 清空 `last_name`。新增 `IPP_TAG_BEGIN_COLLECTION`/`IPP_TAG_END_COLLECTION`。
- `ipp_server.c`：
  - `media-default`/`media-ready` 改为 **`iso_a6_105x148mm`**，`media-supported` 去掉 A8、加 A5。注释写明 A8 绝不可再作为默认值。
  - 补 `printer-location` / `printer-more-info` / `printer-up-time`。
  - **media-col-default 故意不广播**：格式校验无法通过，而非法 collection 会污染整个属性集，比缺属性严重得多；平铺的 media-supported 已够 iOS 选纸。
  - 新增 heap 低水位保护 `HEAP_FLOOR = 6144`：heap 见底时直接关掉新连接，避免请求风暴把板子拖到失联（打印中 heap 约 10 KB，所以阈值必须低于它，否则会误伤进度轮询）。
  - `webui_page.h` 纸张说明改为 A6，状态卡「纸张」副标题改 A6。

### 验收（固件 0x118xxx）
- `ipptool get-printer-attributes`：`Duplicate` 计数 **0**；`RECEIVED: 1660 bytes`（修复前是 `0 bytes`）；仅剩 `EXPECTED: media-col-default` 这一条预期警告。
- **CUPS 复检频率：45 秒内新增 Get-Printer-Attributes 请求 = 0**（修复前每秒 3~4 个）。这是"客户端认可响应"的最硬指标。
- heap 稳定 26~32 KB，不再下滑。
- A6 单页打印：60647 B / 3148 ms，裁剪 340 行 → 打 161 行，job done。
- 多页回归：3 页各 121 行，页间 continuing 无缝。
- 改名热更新仍正常（CatPrint ↔ CatPrint-Test 往返，SRV/TXT 即时跟随）。

### 教训
- 判断"打印机不可用"先看 **heap 和客户端重试频率**，再看 IPP 内容。客户端反复重试几乎总是响应格式问题，不是网络问题。
- 未知/可疑属性宁可不发，也不要发一个可能非法的（collection 类尤其）。

## 十七、"搜不到打印机" = 设备静默失去网络 + 自愈看门狗（2026-11-25 晚三）

### 现象
用户报"搜不到打印机"。实测：ping 192.168.1.21 100% 丢包、631/80 全超时、`catprint-c3.local` 无法解析、`dns-sd -L CatPrint` 无 SRV 记录。但串口日志仍在每 30 s 打印 `alive: heap 8528`——**任务活着，网络死了**。

### 根因
设备被并发连接压到低内存后进入"网络静默"状态：accept 循环还能跑（所以 alive 日志不断），但收不到任何入站包（ping/ICMP、mDNS、TCP 全无响应），只有断电才能恢复。
压测复现（15 个并发 Get-Printer-Attributes）：heap 从 23.5 KB 直落 **3856 B**，正是 lwIP 无缓冲可用的状态。
关键放大因素：连接队列深度 8，每个排队连接在没被 worker 处理前就已占用 lwIP 收发缓冲（约 11 KB/个），iPhone 打印面板一开就并发探测多个连接，几十 KB 瞬间被预定掉。

### 修改（`ipp_server.c`）
1. **连接队列 8 → 3**：最多 5 个连接（2 个在处理 + 3 个排队），其余立即 close，由 iOS 重试。压测后 heap 最低只到 17 KB（改前 3.8 KB）。
2. **健康看门狗 `health_task`**（15 s 周期，优先级 2，打印期间跳过）：
   - 空 heap 连续 120 s 低于 10240 B → `esp_restart()`；
   - 自探测：对 `127.0.0.1:80` 发 `GET /api/status`，连续 3 次拿不到 `200` → `esp_restart()`。
   这样"网络静默"最长约 2 分钟自愈，不再需要手动断电。
3. 保留 `HEAP_FLOOR=6144` 拒连保护（压测中确实触发过 `low heap (3856), refusing connection`）。

### 验证
- 压测 3 轮 × 15 并发：设备始终保持 ping 通、服务可解析；成功响应 14/15 → 12/15 → 8/15（被拒的是队列满主动丢的），heap 结束后回到 21~23 KB。
- 看门狗日志正常：`self-probe failed (1/3), heap 4856` 但随后恢复，未误重启。
- A6 实打：`job media=iso_a6_105x148mm`、60647 B / 3292 ms、裁剪 340 行 → 打 161 行、`job 1 done`。
- 打印后复查：heap 22764、BLE ready、`dns-sd -L` 解析正常、无 panic。

### 给用户的操作
iPhone 上「设置 → 打印机」里因之前多次改名残留了 `catprint2` / `CatPrint-B~E` / `CatPrint-Test` 等死条目（mDNS 缓存，解析不出主机）。**全部删除**后重新添加 `CatPrint`。

## 十八、"名称对不上 + 进度条不动"：iPhone 用 A4 渲染把内存吃光（2026-11-25 晚四）

### 现象
1. 打印面板列表里叫 `CatPrint`，选中后标题变成 `esp32-cat printer`（像另一台机器）。
2. 打印中心进度条不动，作业一直不打印。

### 诊断（串口日志，按最后一次启动切分）
```
op=0x0002 rid=447                      <- 真正的 Print-Job
job media: none sent (client paper not set)
doc starts at 571, first bytes: 55 4e 49 52 41 53 54 ...   (UNIRAST)
urf: multi-page job: 2 pages
urf: page 1: 2480x3507 bits=8 cs=0 bpp=1 row_bytes=2480 (row_step 6.46)
... 564 ms 后 ...
W self-probe failed (1/3), heap 9556
W low heap (5372), refusing connection
```
两个结论：
1. **iPhone 不是按我们声明的 A6 渲染，而是按 A4（2480×3507）渲染**——`media` 属性根本没发。A4 一页就是 175 KB URF，两页 350 KB，上传必然慢，进度条看起来就是不动。
2. 解码器当时是「按客户端宽度分配缓冲」：`gray(width) + avg(width) + vacc(width*2)`，A4 下 2480+2480+4960 ≈ **10 KB**，加上 56 KB 页缓冲，2.3 万字节堆瞬间见底 → 之后 iOS 每次轮询都被 `low heap` 拒绝 → 进度条彻底卡死 → 最后整机网络静默。

### 修改
1. **解码器改为固定 384 列累加**（`urf_stream.c/h`）：
   - 删掉全宽的 `gray` / `avg` / `vacc`，改为 `acc[384] + vacc[384] + acc_cnt[384]`（共 2.3 KB，与页面宽度无关）。
   - 每解出一行就地折算成 384 列的列均值，再按 row_step 做垂直平均，最后 `dither_row()` 以 384 入 384 出（其 box filter 此时是恒等）。
   - 语义与原来等价（先横后竖的双重平均可交换），只是不再需要全宽缓冲。
2. **页缓冲 1200 行 → 800 行**（`pagebuf.h`，56 KB → 37 KB）。A4 满页只输出 543 行、A6 是 541 行，800 行含 24 行撕纸间隙仍有余量。**这一项直接把开机联网后的可用堆从 21 KB 提到 44 KB。**
3. **名称统一**：`printer-make-and-model` 原本写死 `"ESP32-C3 Cat Printer"`，iOS 选中后显示它，于是与列表里的 mDNS 名 `CatPrint` 打架。改为跟随配置名（`ps_printer_name()`），现在 name / info / make-and-model 三者都是 `CatPrint`。
4. **日志加对端 IP**（`peer=192.168.1.x`）：手机、Mac 的 CUPS、ipptool 都会探同一端口，必须能从串口日志直接看出是谁在压。

### 验证（固件 0x119290）
- 开机：`page buffer: 800 rows (37 KiB), free heap 194760`；联网+BLE 后 `/api/status` heap **44392**（改前 21 KB）。
- **满高 A4 双页**（2480×3507 ×2，291 660 B，与 iPhone 实际渲染完全同尺寸）：页 1 解码 538 行→打 534 行，页 2 同样，`job done`，页间无缝续打；上传全程 heap 稳定在 41.7~42.6 KB（改前会掉到 3~5 KB）。
- 属性一致性：`printer-name` / `printer-info` / `printer-make-and-model` 全为 `CatPrint`，`media-default=iso_a6_105x148mm`。
- 本次启动统计：Get-Printer-Attributes 仅 4 次（改前 30+ 次成串）、`queue full` 0 次、`low heap` 0 次——内存宽松后客户端也不再重试了。
- A4 宽度短页（2480×900 ×2）回归同样通过（130 / 135 行）。

### 已知取舍
- 页缓冲 800 行意味着「单页输出超过 800 行」会被截断（日志 `page buffer full`）。A4/A6/Letter/4×6 在 300 dpi 下最高 543 行，正常客户端够用；180° 旋转模式（整篇缓冲）超长会截断，该模式本来就是降级路径。
- 上传仍受限于 Wi-Fi：291 KB 实测 59 s（含 22 s 页间打印停顿），iPhone 直传会更快。

## 十九、raster 管线上移 NAS（方案 B 深化，2026-11-25 21:12-21:42）

需求：解码、缩放（任意纸张→热敏纸宽）、灰度二值化全部放 NAS，ESP32 只做字节透传。

### 架构（分支 nas-cups-bridge，最终形态）
```
Mac/iPhone → NAS CUPS (image/urf) → catbridge 后端（NAS 全部 raster 工作）
  UNIRAST 解码 → 每页裁空白边距 → 多页纵向拼接（连续纸，与所选纸张无关）
  → area-average 缩放到 384 列 → 二值化（catprinter 算法）→ 生成完整 BLE 指令流
  （帧+CRC8，darkness/speed/feed 来自 /etc/catbridge.conf）
→ POST ESP32 /raw → cp_send_raw 透传 0xAE01 → 打印机
```

### 固件改动（commit 885e952）
- bridge_server.c 新增 `POST /raw`：body 逐块 `cp_send_raw()`（send_chunked 提供 MTU 分块 + 0xAE02 流控 + 每块写延时），零解码、零缓冲、TCP 背压。s_raw_busy 防并发；/print URF 路径保留兼容。

### NAS 后端重写（commit cddb58b）
- `nas/cups-backend/catbridge` 成为完整 raster 引擎（Python，numpy+PIL，Debian 包 python3-numpy python3-pil）：
  - UNIRAST 解码移植 urf_stream.c 语义（行首 repeat 字节 + PackBits 变体 + 0x80 清行尾）；
  - **关键坑：CUPS 2.4 的 rastertourf 生成真 1bpp 光栅**（bits=1，8 像素/字节打包，行宽 ceil(w/8)），固件的 `bits//8` 每像素一字节假设在此格式上完全错位；Mac CUPS 2.3.4 生成的是 bits=8 sRGB。1bpp：MSB-first、bit 1=黑、解码后展开为灰度；
  - RLE 写入必须 clamp（Python bytearray 切片越界会扩容，C 语义是丢弃）；
  - 二值化算法（/etc/catbridge.conf `algo=`）：floyd-steinberg / atkinson / halftone / mean-threshold / none，实现参照 rbaron/catprinter img.py；
  - 指令流复刻 cp_print_page_ex 序列：get_state ×2 → set_dpi(50) → set_energy(0x1000+darkness*0x400) → apply_energy → update_device → lattice_start → 逐行 draw_row（位反转载荷）→ lattice_end → set_speed → feed → get_state。
- 配置 `/etc/catbridge.conf`：host / algo / darkness / speed / feed。setup.sh 幂等写。

### 验证
- NAS lp A6（1bpp 光栅）、`-o media=A4` 强制 A4 多页连续、Mac 文本三路全部打通；ESP32 heap 62KB。
- 设备端日志：`POST /raw ... raw relay ok`（1633B/289ms、849B/166ms、1409B/249ms）。

### 调试备忘
- cupsd 有 PrivateTmp，后端写 /tmp 落在命名空间内看不见；
- backend 的 stderr INFO 级不进 error_log，需 `cupsctl --debug-logging` 临时打开；
- raster 经 stdin 交付（6 参数模式），spool 的 d 文件只是未过滤原文；
- 复现生成端差异最有效的办法：Mac `cupsfilter -p PPD -i text/plain -m image/urf -o media=A6`。

### 遗留
- 固件 /print 路径（urf_stream.c）对 1bpp URF 仍按每像素 1 字节解析——主路径已是 /raw 不受影响，如需保留 /print 兼容 CUPS 2.4 客户端需同样修 1bpp。
- ESP32 WebUI 的 darkness/feed 设置对 /raw 作业无效（设置权移交 NAS 配置）。

## 二十、设置 WebUI 上移 NAS（2026-11-25 21:47-21:58）

### ESP32（精简为桥接终端页）
- webui_page.h 重写：仅保留 WiFi 配网（扫描/连接/忘记/BSSID 锁定）、BLE 打印机连接（扫描/手动 MAC/绑定当前/断开/解绑）、状态（wifi/ble/sys）、测试页、重启。顶部固定链接跳转 NAS 控制台（Kconfig `CATBRIDGE_WEB_URL`，默认 http://192.168.1.17:8080；webui_page.h 需 `#include "sdkconfig.h"` 才能引用 CONFIG 宏）。
- webui.c 删除 /api/settings GET+POST；/api/status 瘦身为 wifi/ble/print/sys（去掉 settings blob 与 media）。settings NVS 里的 darkness/speed/feed/orientation 只剩旧 /print URF 路径使用。

### NAS（新增 bridge-web 控制台）
- `nas/web/bridge_web.py`：stdlib-only HTTP 服务 :8080（systemd `catbridge-web`）。功能：读改 /etc/catbridge.conf（algo/darkness/speed/feed/host，临时文件原子替换，作业启动即生效）、代理 ESP32 /api/status、显示 CUPS 待打队列。页面中文单页，5 秒自动刷新。
- setup.sh 第 6 步安装到 /opt/catbridge 并 enable --now。

### 验证
- /api/save 往返（mean-threshold/darkness=9 写入、读回一致、还原）✓
- ESP32 重启后 BLE 自动重连、Mac 打印 e2e（905B relay ok, heap 62KB）✓

## 二十一、多页只打第一页修复（2026-11-25 22:04-22:16）

现象：多页文档只有第一页出纸（NAS raster 管线时期）。

根因：catbridge 后端把整份文档所有行塞进**一个 lattice 会话**；打印机的行缓冲
约一页就停。旧固件是多页时每页跑一次完整 cp_print_page_ex（独立 prepare +
lattice start/end + 页间 feed=0），NAS 版漏掉了这个协议惯例。

修复（nas/cups-backend/catbridge）：
- urf_to_row_bytes -> urf_to_row_segments：按页产出行段（单页超 500 行再切分）；
- build_command_stream：每段一个完整会话（get_state x2 → set_dpi → energy →
  apply → update_device → lattice_start → 行 → lattice_end → set_speed →
  feed[中间=0 / 最后=conf] → get_state），与固件逐页序列一致。

验证：3 页密集 PDF → 1332 行 / 3 会话 / 74955B 全透传（24.7s）。Mac 端
cupsfilter 复现多页 URF 与后端解码（2 页返回 2 个数组）均正常——
问题纯在指令流分帧，不在解码或缩放。
