下面是重新梳理后的版本，可以直接放到文档里：

------

`${deviceId}` 表示设备编号。

不同类型网关中，`${deviceId}` 的含义不同：

- 银尔达 4G 网关：`${deviceId}` 表示设备 IMEI
  示例：`862323082181556`
- ESP32-S3 自研网关：`${deviceId}` 表示设备 Wi-Fi MAC 地址
  示例：`7CDFA13C9E42`

`${groupId}` 为固定网关分组标识，可取以下值之一：

- `gw_4g_yed_hex`
- `yx_gw`
- `deivce1218`

获取 MQTT 用户名接口：

```
GET https://yxzhhz.net/api/mqtt/getUserNameByMac?mac=${deviceId}&groupId=GID_${groupId}
```

说明：

- ```
  mac
  ```

   参数传入设备标识：

  - 银尔达网关传 IMEI
  - ESP32-S3 自研网关传 Wi-Fi MAC

- `groupId` 参数格式固定为：`GID_${groupId}`

MQTT 订阅主题：

```
/${groupId}/YxzhNode${deviceId}/thing/model/down_raw
```

MQTT 发布主题：

```
/${groupId}/YxzhNode${deviceId}/thing/model/up_raw
```

示例，银尔达 4G 网关：

```
deviceId = 862323082181556
groupId  = gw_4g_yed_hex

GET https://yxzhhz.net/api/mqtt/getUserNameByMac?mac=862323082181556&groupId=GID_gw_4g_yed_hex

订阅主题:
/gw_4g_yed_hex/YxzhNode862323082181556/thing/model/down_raw

发布主题:
/gw_4g_yed_hex/YxzhNode862323082181556/thing/model/up_raw
```

示例，ESP32-S3 自研网关：

```
deviceId = 7CDFA13C9E42
groupId  = yx_gw

GET https://yxzhhz.net/api/mqtt/getUserNameByMac?mac=7CDFA13C9E42&groupId=GID_yx_gw

订阅主题:
/yx_gw/YxzhNode7CDFA13C9E42/thing/model/down_raw

发布主题:
/yx_gw/YxzhNode7CDFA13C9E42/thing/model/up_raw
```