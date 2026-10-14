# 出厂 MQTT 连接信息写入说明

## 1. 文档定位

本文档说明统一 `dev_mqtt` 的身份和连接凭据来源。生产固件启动后由业务 MQTT worker
通过 HTTPS 注册接口按 eFuse MAC 申请 MQTT 凭据；注册成功后只在内存中创建这一条 MQTT
会话。旧版出厂 NVS MQTT 字段仅作为兼容/自检路径保留，不会在生产启动路径创建第二条连接。

注册请求固定为 `?mac=<12位大写eFuse_MAC>&groupId=GID_yx_gw`。Topic 分组固定为 `yx_gw`，
发布 QoS 为 0，订阅 QoS 为 1。

## 2. 兼容 NVS 字段（非生产默认路径）

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
| `group_id` | 是 | MQTT Topic 组 ID，固定 `yx_gw` |
| `device_id` | 是 | MQTT Topic 设备节点 ID，固定为 eFuse MAC 十六进制大写字符串 |

设备管理 MQTT Topic 使用固定组 `yx_gw` 和 `device_id` 生成节点名：

```text
/yx_gw/YxzhNode${device_id}/thing/model/down_raw
/yx_gw/YxzhNode${device_id}/thing/model/up_raw
```

其中 `device_id` 在设备初始化阶段由硬件 MAC 的十六进制大写字符串生成，例如 `80B54EE92138`。

## 3. 固件启动行为（生产统一 transport）

启动后线程 1 的 MQTT 连接行为如下：

```text
BOOT_READY
  -> 启动默认 4G
  -> 网络可用
  -> HTTPS 注册：mac=<eFuse_MAC>&groupId=GID_yx_gw
  -> 注册成功：内存加载 broker/username/password/client_id
  -> 创建唯一 dev_mqtt，订阅 down_raw(QoS1)
  -> 发布上行统一使用 up_raw(QoS0)
  -> 注册/鉴权失败：按退避策略重试，不创建第二 client
```

注册 HTTPS 失败时不进入协议应答流程；设备保持离线状态并由 worker 按普通或鉴权退避重试。

## 4. MQTT 连接信息转换

生产固件收到注册响应后按以下方式创建 MQTT 连接参数：

| MQTT 连接项 | 来源 |
| --- | --- |
| Broker URI | 注册响应中的 broker 地址 |
| Username | `username` |
| Password | `password` |
| Client ID | `client_id` |

兼容 NVS 路径示例（仅自检/维护模式）：

```text
host = post-cn-to34doo3g01.mqtt.aliyuncs.com
port = 1883
broker = mqtt://post-cn-to34doo3g01.mqtt.aliyuncs.com:1883
```

## 5. 异常处理

1. HTTPS 注册字段缺失、响应非 2xx 或响应格式非法：保持离线并按注册退避重试。
2. MQTT 鉴权失败：`mqtt_status=auth_failed`，清除本轮内存凭据后按鉴权退避重新注册。
3. 兼容 NVS 字段解密失败时只影响自检/维护路径，不覆盖 eFuse MAC 身份。
4. 恢复出厂可以清除兼容 NVS 连接信息；生产路径仍按 eFuse MAC 重新注册。

## 6. 安全要求

1. `password` 不得打印到日志、属性、状态或 MQTT 上报中。
2. NVS 中的系统参数必须按 `06-模块设计-存储与文件系统.md` 的 AES-256-CTR 规则加密存储。
3. 发布版本禁止打印完整 MQTT 连接信息，只允许打印 host、port、client_id 长度等脱敏诊断信息。
