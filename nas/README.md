# NAS 侧部署：光栅引擎 + 控制台（方案 B）

ESP32 只做 BLE 透传（POST /raw），全部光栅工作在本目录的脚本/应用里完成。

## 架构

```
iPhone / Mac / 任意 AirPrint 客户端
        │  mDNS 发现 "CatPrint"（avahi 广播在 NAS 上）
        ▼
NAS CUPS :631
        │  IPP 收单 → PPD cupsFilter2 直通 image/urf → catbridge 后端
        ▼  UNIRAST 解码 → 裁空白 → 拼页 → 缩放 384 列 → 抖动 → BLE 指令流
        ▼  POST /raw
ESP32-C3 桥固件（分支 nas-cups-bridge） → BLE → 猫打印机
```

## 文件

| 文件 | 去向 |
|---|---|
| `cups-backend/catbridge` | NAS `/usr/lib/cups/backend/catbridge` (755)，完整光栅引擎 |
| `cups/CatPrint.ppd` | lpadmin -P 引用，cupsFilter2 直通 image/urf |
| `avahi/airprint.service` | NAS `/etc/avahi/services/airprint.service` |
| `web/bridge_web.py` | 控制台 :8080（配置 /etc/catbridge.conf、状态、CUPS 队列） |
| `setup.sh` | 通用 Linux 幂等安装（sudo 运行） |
| `deploy.py` | paramiko 一键部署（`NAS_HOST/NAS_USER/NAS_PASS` 环境变量，缺省会交互询问） |
| `fnos-app/` | 飞牛 fnOS 应用工程 + 打包产物 `catbridge-1.0.0.fpk` |

## catbridge 后端工作流

1. CUPS 以两种方式调用：无参数 = 设备发现；6 参数 = stdin 模式；7 参数 = 文件模式
2. `decode_urf`：UNIRAST 解码（兼容 1bpp 打包与 8bpp sRGB）
3. 逐页裁空白 → 纵向拼接（纸张无关，按连续纸输出）
4. 区域平均缩放到 384 列
5. 二值化（5 种算法，与 rbaron/catprinter 同名）
6. `build_command_stream`：每页独立 lattice 会话（中间 feed=0 无缝，末页按配置 feed）
7. POST 到 ESP32 `/raw`

调试不经过 CUPS：`catbridge --dump in.urf > out.bin`

## 配置（/etc/catbridge.conf）

```ini
host=192.168.1.x        # ESP32 桥地址
algo=floyd-steinberg    # 二值化算法
darkness=               # 浓度档位（空=默认）
speed=                  # 速度档位（空=默认）
feed=                   # 末页走纸行数
```

控制台 `:8080` 可视化修改。fnOS 应用自带控制台；systemd 部署由 `web/catbridge-web.service` 提供。

## 部署

### fnOS 应用（推荐）

应用中心 → 设置 → 手动安装 `fnos-app/catbridge-1.0.0.fpk`。

### 通用 Linux

```sh
scp -r nas/ user@nas:/tmp/catbridge-deploy
ssh user@nas 'sudo sh /tmp/catbridge-deploy/setup.sh'
```

## 为什么这条路稳定

- IPP 协议兼容（多值属性、collection、chunked/内联文档、纸张协商、iOS/macOS 客户端差异）
  全部由成熟的 CUPS 承担，ESP32 侧只面对一个我们自己写的 HTTP 客户端
- ESP32 固件少了一大块常驻内存（IPP 属性缓冲、mDNS 缓存），堆余量更大
- 改名 = 改 avahi 配置；纸张 = 改 PPD；都不用动固件

## 注意

- 必须搭配 `nas-cups-bridge` 分支固件（POST /raw 端点）；`main` 分支是方案 A 固件，
  烧录即切换
- NAS 与 ESP32 须同网段；ESP32 IP 变化时更新 `/etc/catbridge.conf` 的 host
  （或路由器 DHCP 保留）
- catbridge 后端等待 BLE 打印最长 10 分钟（UPLOAD_TIMEOUT_S），
  CUPS 报错重试策略按 cupsd.conf 默认
