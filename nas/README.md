# 方案 B 部署：协议层上移到 NAS，ESP32 只留 BLE 打印桥

## 架构

```
iPhone / Mac / 任何 AirPrint 客户端
        │  mDNS 发现 "CatPrint" (avahi 广播在 NAS 上)
        ▼
NAS (192.168.1.17) CUPS :631
        │  IPP 收单 → PPD cupsFilter2 直通 → catbridge 后端
        ▼  POST /print (原始 URF)
ESP32-C3 (192.168.1.21, 分支 nas-cups-bridge 固件)
        │  流式 URF 解码 → 384 列 → 抖动
        ▼  BLE
猫打印机
```

## 文件

| 文件 | 去向 |
|---|---|
| `cups-backend/catbridge` | NAS `/usr/lib/cups/backend/catbridge` (755) |
| `cups/CatPrint.ppd` | lpadmin -P 引用，cupsFilter2 直通 image/urf |
| `avahi/airprint.service` | NAS `/etc/avahi/services/airprint.service` |
| `setup.sh` | 在 NAS 上 sudo 运行，幂等 |

## 部署

```sh
scp -r nas/ dja@192.168.1.17:/tmp/catbridge-deploy
ssh dja@192.168.1.17
echo '<密码>' | sudo -S -p '' sh /tmp/catbridge-deploy/setup.sh
```

换桥地址（ESP32 IP）：

```sh
echo 'export CATBRIDGE_HOST=192.168.1.21' | sudo tee /etc/cups/catbridge.env
```

（或直接编辑 backend 里的 DEFAULT_HOST。）

## 为什么这条路稳定

- IPP 协议兼容（多值属性、collection、chunked/内联文档、纸张协商、
  iOS/macOS 客户端差异）全部由 NAS 上经过几十年打磨的 CUPS 承担，
  ESP32 侧只面对一个我们自己写的 HTTP 客户端。
- ESP32 固件少了一大块常驻内存（IPP 属性缓冲、mDNS 缓存），堆余量更大。
- 热改名 = 改 avahi service 文件；纸张问题 = 改 PPD，都不用动固件。

## 注意

- ESP32 固件必须刷 `nas-cups-bridge` 分支（POST /print 端点）；
  `main` 分支仍是 AirPrint 固件，两套可随时互切（烧录即切换）。
- NAS 必须与 ESP32 同网段；ESP32 IP 变化时更新 CATBRIDGE_HOST
  （或在路由器上给 ESP32 做 DHCP 保留）。
- catbridge 后端等待 BLE 打印最长 10 分钟（UPLOAD_TIMEOUT_S），
  CUPS 报错重试策略按 cupsd.conf 默认。
