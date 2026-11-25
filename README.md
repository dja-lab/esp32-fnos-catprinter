# esp32-fnos-catprinter

把一台 **BLE 热敏猫打印机**（GB01 / MX06 / X6h 系列小票机）变成局域网内所有设备的 **AirPrint 打印机**。

- 手机 / Mac 打印列表里自动出现 "CatPrint"，无需安装任何 App
- 无论客户端选什么纸张，都按**热敏连续纸**整卷输出：自动裁空白、多页纵向无缝拼接
- NAS 负责全部"脏活"（协议、解码、缩放、抖动），ESP32 只做 BLE 透传，小内存也稳

## 架构

```
iPhone / Mac / 任意 AirPrint 客户端
        │  mDNS 发现 "CatPrint"（avahi 广播在 NAS 上）
        ▼
NAS（飞牛 fnOS 或任意 Linux）CUPS :631
        │  IPP 收单 → PPD cupsFilter2 把 image/urf 原样直通 → CUPS 后端 catbridge
        ▼   UNIRAST 解码 → 逐页裁空白 → 纵向拼接 → 缩放到 384 列
        │   灰度二值化 → 生成完整 BLE 指令流（帧 + CRC8）
        ▼   POST /raw（纯字节透传）
ESP32-C3 桥固件
        │  流式转发（MTU 分块 + 流控 + TCP 背压，零解码零整页缓冲）
        ▼  BLE
猫打印机
```

**为什么协议层放在 NAS**：IPP 协议兼容（多值属性、collection、chunked 文档、纸张协商、iOS/macOS 客户端差异）全部由成熟的 CUPS 承担；ESP32 侧只面对一个我们自己写的 HTTP 客户端。改名 = 改 avahi 配置，调纸张 = 改 PPD，都不用重刷固件。

## 仓库结构

```
├── components/            # ESP32 固件（ESP-IDF 5.x）
│   ├── catprinter/        #   打印机 BLE 协议（lattice 会话 / CRC8 / 逐行位图）
│   ├── printserver/       #   HTTP 桥：POST /raw 透传、/print 遗留 URF 路径、WebUI
│   ├── raster/            #   URF 解码 / 页缓冲 / 抖动（方案 A 遗留，方案 B 不再使用）
│   └── settings/          #   NVS 配置存储
├── main/                  #   固件入口 + Kconfig（WiFi / 打印机名 / BLE 前缀等）
├── host_test/             #   协议编码的主机端单元测试
├── nas/                   # NAS 侧部署物
│   ├── cups-backend/catbridge   # CUPS 后端：完整光栅引擎（Python + numpy + PIL）
│   ├── cups/CatPrint.ppd        # 打印机描述文件（image/urf 直通）
│   ├── avahi/airprint.service   # mDNS/AirPrint 广播
│   ├── web/bridge_web.py        # 打印控制台（:8080，stdlib 实现）
│   ├── setup.sh                 # 通用 Linux 幂等安装脚本
│   ├── deploy.py                # paramiko 一键部署
│   └── fnos-app/catbridge/      # 飞牛 fnOS 应用（.fpk，宿主机 CUPS/avahi）
└── docs/HANDOFF.md        # 开发日志：踩坑记录与关键决策（19+ 节）
```

## 分支说明

| 分支 | 内容 | 状态 |
|---|---|---|
| `nas-cups-bridge` | **推荐**。方案 B：NAS 光栅引擎 + ESP32 纯 BLE 桥 | ✅ 当前主线，三路验证通过 |
| `main` | 方案 A：ESP32 独立完成 IPP/mDNS/解码/抖动（对硬件要求高） | 存档，烧录即可切回 |

两套固件互不兼容协议，NAS 侧必须搭配 `nas-cups-bridge` 分支固件使用。

## NAS 部署

### 方式一：飞牛 fnOS 应用（推荐）

应用中心 → 设置 → 手动安装 `nas/fnos-app/catbridge-1.0.0.fpk`。应用会自动：

- 安装 Python 依赖（numpy / Pillow，走国内镜像）
- 装 CUPS 后端、创建 CatPrint 队列、注册 avahi AirPrint 服务
- 启动 ：8080 控制台（管理打印参数：二值化算法 / 浓度 / 速度 / 走纸 / 桥地址）

### 方式二：任意 Linux + 宿主机 CUPS

```sh
python3 nas/deploy.py        # 或手动：
scp -r nas/ user@nas:/tmp/catbridge-deploy
ssh user@nas 'sudo sh /tmp/catbridge-deploy/setup.sh'
```

换桥地址（ESP32 IP）：

```sh
echo 'host=192.168.1.x' | sudo tee /etc/catbridge.conf   # 或在控制台里改
```

依赖：CUPS 2.2+、avahi-daemon、Python 3 + numpy + Pillow。

## 固件编译（ESP32-C3）

```sh
idf.py set-target esp32c3
idf.py menuconfig    # AirPrint Cat Printer 菜单：WiFi / 打印机名 / BLE 前缀
idf.py build flash monitor
```

固件侧只保留：配网、BLE 连接、HTTP 透传、状态页。WebUI 提供 WiFi 配网 / BLE 绑定 / 测试页 / 重启。

## 打印参数（NAS 控制台 :8080）

| 参数 | 说明 |
|---|---|
| 二值化算法 | `floyd-steinberg` / `atkinson` / `halftone` / `mean-threshold` / `none`（与 rbaron/catprinter 同名） |
| 打印浓度 | 能量档位 |
| 打印速度 | 速度档位 |
| 尾部走纸 | 每页任务末尾 feed 行数（0 = 无缝连续） |
| 桥地址 | ESP32 IP |

## 已知限制

- 热敏纸只能打黑白的：灰度层次靠抖动算法近似
- 多页文档按逐页 lattice 会话发送（页间 `feed=0` 无缝衔接），单页超过 500 行会自动再分段
- CUPS 后端等待 BLE 打印最长 10 分钟；打印中请勿让 ESP32 断电
- ESP32 端 WebUI 里的打印参数对 `/raw` 链路无效——设置权在 NAS 控制台

## 致谢

- [rbaron/catprinter](https://github.com/rbaron/catprinter) —— BLE 协议逆向与抖动算法参考
- Apple / CUPS 社区对 URF（UNIRAST）格式的公开分析

## License

MIT（欢迎自取改造；打印机型号不同需自行确认协议帧格式）
