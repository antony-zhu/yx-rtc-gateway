import 'package:flutter_test/flutter_test.dart';
import 'package:flutter/foundation.dart';
import 'package:yx_gateway_timer/data/ble/yx_gatt_client.dart';
import 'package:yx_gateway_timer/domain/protocol_error.dart';
import 'package:yx_gateway_timer/domain/timer_task.dart';

void main() {
  test('hello 能力列表解析 device_identify 且旧固件缺省为空', () {
    final supported = GatewayInfo.fromJson(
      _gatewayInfoJson(features: const <String>['device_identify']),
    );
    final legacy = GatewayInfo.fromJson(_gatewayInfoJson());

    expect(supported.features, contains('device_identify'));
    expect(legacy.features, isEmpty);
  });

  test('识别灯成功响应必须 accepted=true 且结果字段为整数', () {
    final result = DeviceIdentifyResult.fromJson(const <String, Object?>{
      'accepted': true,
      'led_index': 2,
      'gpio': 42,
      'duration_ms': 350,
    });

    expect(result.ledIndex, 2);
    expect(result.gpio, 42);
    expect(result.durationMs, 350);
    expect(
      () => DeviceIdentifyResult.fromJson(const <String, Object?>{
        'accepted': false,
        'led_index': 2,
        'gpio': 42,
        'duration_ms': 350,
      }),
      throwsA(isA<ProtocolException>()),
    );
    expect(
      () => DeviceIdentifyResult.fromJson(const <String, Object?>{
        'accepted': true,
        'led_index': 2,
        'gpio': '42',
        'duration_ms': 350,
      }),
      throwsA(isA<ProtocolException>()),
    );
  });

  test('场景号设置和读取响应必须返回 0～255 内的整数', () {
    final setResult = SceneNumberResult.fromJson(const <String, Object?>{
      'accepted': true,
      'scene_no': 6,
    }, requireAccepted: true);
    final readResult = SceneNumberResult.fromJson(const <String, Object?>{
      'scene_no': 9,
    }, requireAccepted: false);

    expect(setResult.sceneNo, 6);
    expect(readResult.sceneNo, 9);
    expect(
      () => SceneNumberResult.fromJson(const <String, Object?>{
        'accepted': false,
        'scene_no': 6,
      }, requireAccepted: true),
      throwsA(isA<ProtocolException>()),
    );
    expect(
      () => SceneNumberResult.fromJson(const <String, Object?>{
        'scene_no': 256,
      }, requireAccepted: false),
      throwsA(isA<ProtocolException>()),
    );
  });

  test('灯具测试响应保留业务目标，不接收网关拒绝结果', () {
    final result = LampTestResult.fromJson(const <String, Object?>{
      'accepted': true,
      'action': 'flash',
      'target': <String, Object?>{
        'network_id': 124,
        'type': 'virtual',
        'value': 1,
      },
    });

    expect(result.action, LampTestAction.flash);
    expect(result.target.networkId, 124);
    expect(result.target.type, LampTargetType.virtualNumber);
    expect(result.target.value, 1);
    expect(result.target.destinationAddress, 0x8001);
    expect(
      () => LampTestResult.fromJson(const <String, Object?>{
        'accepted': false,
        'action': 'light_on',
        'target': <String, Object?>{
          'network_id': 0,
          'type': 'number',
          'value': 1,
        },
      }),
      throwsA(isA<ProtocolException>()),
    );
  });

  test('Android 跳过动态 Info 长读取并由加密 hello 核验身份', () {
    expect(
      ReactiveYxGattClient.shouldReadInfoBeforeHello(TargetPlatform.android),
      isFalse,
    );
    expect(
      ReactiveYxGattClient.shouldReadInfoBeforeHello(TargetPlatform.iOS),
      isTrue,
    );
    expect(
      ReactiveYxGattClient.shouldProbeEncryptedResponseBeforeHello(
        TargetPlatform.android,
      ),
      isFalse,
    );
    expect(
      ReactiveYxGattClient.shouldProbeEncryptedResponseBeforeHello(
        TargetPlatform.iOS,
      ),
      isTrue,
    );
  });

  test('Android GATT 写入失败会归类为蓝牙传输故障', () {
    expect(
      ReactiveYxGattClient.isBleTransportFailure(
        Exception('WriteCharacteristicFailure: status 133 (GATT_ERROR)'),
      ),
      isTrue,
    );
    expect(
      ReactiveYxGattClient.isBleTransportFailure(
        const ProtocolException(ProtocolErrorCode.identityConflict, '身份不一致'),
      ),
      isFalse,
    );
  });
}

Map<String, Object?> _gatewayInfoJson({List<String>? features}) {
  final result = <String, Object?>{
    'protocol': 1,
    'device_id': '80B54EE92138',
    'alias': '测试网关',
    'firmware': '1.0.0',
    'max_tasks': 16,
    'task_count': 0,
    'utc_trusted': true,
    'tz_offset_min': 480,
    'table_revision': 1,
  };
  if (features != null) result['features'] = features;
  return result;
}
