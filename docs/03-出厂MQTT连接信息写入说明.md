# 出厂 MQTT 连接信息写入说明

## 1. 文档定位

本文档保留编号 `03`，但当前版本不再定义固件运行时 HTTP 注册流程。

设备管理 MQTT 连接信息由出厂/产测流程写入设备 NVS。固件启动后只读取本地 MQTT 连接信息并尝试连接 `dev_mqtt`；如果连接信息缺失、损坏或解密失败，固件进入 `mqtt_status=not_configured`，不得主动访问 HTTP 注册接口获取 MQTT 参数。

HTTP 注册接口如果仍由后台或产测工具使用，属于固件外部流程。固件侧只关心最终写入 NVS 的 MQTT 参数。

## 2. 出厂必须写入的字段

MQTT 连接信息写入 NVS namespace `mqtt`，所有字段必须使用系统参数加密写入规则保存。

| NVS key | 必填 | 说明 |
| --- | --- | --- |
| `host` | 是 | MQTT Broker 域名或 IP，不带协议头 |
| `port` | 是 | MQTT Broker 端口，字符串形式，例如 `1883` |
| `username` | 是 | MQTT 用户名 |
| `password` | 是 | MQTT 密码 |
| `client_id` | 是 | MQTT Client ID |

设备身份字段写入 NVS namespace `factory`：

| NVS key | 必填 | 说明 |
| --- | --- | --- |
| `product_id` | 是 | 产品 ID；当前默认 `yx_gw` |
| `group_id` | 是 | MQTT Topic 组 ID；当前联调阶段暂时使用 `yx_gw` |
| `device_id` | 是 | MQTT Topic 设备节点 ID；设备初始化时使用 MAC 十六进制大写字符串作为 `device_id` |

设备管理 MQTT Topic 使用固定组 `yx_gw` 和 `device_id` 生成节点名：

```text
/yx_gw/YxzhNode${device_id}/thing/model/down_raw
/yx_gw/YxzhNode${device_id}/thing/model/up_raw
```

其中 `device_id` 在设备初始化阶段由硬件 MAC 的十六进制大写字符串生成，例如 `80B54EE92138`。

## 3. 固件启动行为

启动后线程 1 的 MQTT 连接行为如下：

```text
BOOT_READY
  -> 启动默认 4G
  -> 网络可用
  -> 读取 NVS 中的 MQTT 连接信息
  -> 信息完整：连接 dev_mqtt
  -> 信息缺失/损坏：mqtt_status=not_configured，等待重新出厂写入或维护处理
```

固件不得因为 MQTT 信息缺失而执行 HTTP GET、HTTP 注册、后台账号申请或任何自动补齐流程。

## 4. MQTT 连接信息转换

固件读取 NVS 后按以下方式创建 MQTT 连接参数：

| MQTT 连接项 | 来源 |
| --- | --- |
| Broker URI | `mqtt://<host>:<port>` |
| Username | `username` |
| Password | `password` |
| Client ID | `client_id` |

示例：

```text
host = post-cn-to34doo3g01.mqtt.aliyuncs.com
port = 1883
broker = mqtt://post-cn-to34doo3g01.mqtt.aliyuncs.com:1883
```

## 5. 异常处理

1. 缺少任一 MQTT 字段：`mqtt_status=not_configured`，`status=offline`。
2. MQTT 信息解密失败或 CRC 校验失败：视为 `not_configured`，不得删除 `device_id`。
3. MQTT 鉴权失败：`mqtt_status=auth_failed`，保留本地 MQTT 信息，不自动清除、不自动 HTTP 注册。
4. 需要更换 MQTT 信息时，必须由出厂/维护工具重新写入 NVS 后重启或重新初始化。
5. 恢复出厂可以清除 MQTT 连接信息；清除后设备必须重新写入出厂参数才能上线。

## 6. 安全要求

1. `password` 不得打印到日志、属性、状态或 MQTT 上报中。
2. NVS 中的系统参数必须按 `06-模块设计-存储与文件系统.md` 的 AES-256-CTR 规则加密存储。
3. 发布版本禁止打印完整 MQTT 连接信息，只允许打印 host、port、client_id 长度等脱敏诊断信息。
