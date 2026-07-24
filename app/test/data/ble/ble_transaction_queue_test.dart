import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:yx_gateway_timer/data/ble/ble_frame_codec.dart';
import 'package:yx_gateway_timer/data/ble/ble_transaction_queue.dart';
import 'package:yx_gateway_timer/domain/protocol_error.dart';

void main() {
  test('命令逐片确认并通过 Notify 完成', () async {
    final transport = _NotifyTransport();
    late BleTransactionQueue queue;
    queue = BleTransactionQueue(transport: transport, negotiatedMtu: () => 23);
    transport.onCommand = (command, transactionId) {
      final response = BleFrameCodec.encodeJson(
        json: <String, Object?>{
          'v': 1,
          'id': transactionId,
          'ok': true,
          'result': <String, Object?>{'echo': command['op']},
        },
        transactionId: transactionId,
        negotiatedMtu: 23,
      );
      scheduleMicrotask(() {
        for (final fragment in response) {
          queue.onResponseFragment(fragment);
        }
      });
    };

    final result = await queue.execute('hello', <String, Object?>{
      'text': '中文参数用于强制多片',
    });
    expect(result['echo'], 'hello');
    expect(transport.writeCount, greaterThan(1));
    expect(transport.maximumConcurrentWrites, 1);
  });

  test('Notify 超时后仅用匹配当前 id 的 Response Read 恢复', () async {
    final transport = _NotifyTransport(
      readValue: utf8.encode(
        jsonEncode(const <String, Object?>{
          'v': 1,
          'id': 1,
          'ok': true,
          'result': <String, Object?>{'recovered': true},
        }),
      ),
    );
    final queue = BleTransactionQueue(
      transport: transport,
      negotiatedMtu: () => 247,
      timeout: const Duration(milliseconds: 5),
    );
    final result = await queue.execute('hello', const <String, Object?>{});
    expect(result['recovered'], isTrue);
    expect(transport.readCount, 1);
  });

  test('Response Read 的 id 不匹配时不得恢复', () async {
    final transport = _NotifyTransport(
      readValue: utf8.encode(
        jsonEncode(const <String, Object?>{
          'v': 1,
          'id': 99,
          'ok': true,
          'result': <String, Object?>{},
        }),
      ),
    );
    final queue = BleTransactionQueue(
      transport: transport,
      negotiatedMtu: () => 247,
      timeout: const Duration(milliseconds: 5),
    );
    await expectLater(
      queue.execute('timer.delete', const <String, Object?>{}),
      throwsA(
        isA<ProtocolException>().having(
          (error) => error.code,
          'code',
          ProtocolErrorCode.timeout,
        ),
      ),
    );
    expect(transport.readCount, 1);
  });

  test('断线会清空待响应事务且不自动重放', () async {
    final transport = _NotifyTransport();
    final queue = BleTransactionQueue(
      transport: transport,
      negotiatedMtu: () => 23,
      timeout: const Duration(seconds: 1),
    );
    final future = queue.execute('timer.upsert', <String, Object?>{
      'x': 'y' * 40,
    });
    final disconnected = expectLater(future, throwsA(isA<ProtocolException>()));
    await Future<void>.delayed(Duration.zero);
    queue.reset();
    await disconnected;
    final writes = transport.writeCount;
    await Future<void>.delayed(const Duration(milliseconds: 10));
    expect(transport.writeCount, writes);
  });

  test('device.identify 命令携带空 params 对象', () async {
    final transport = _NotifyTransport();
    late BleTransactionQueue queue;
    queue = BleTransactionQueue(transport: transport, negotiatedMtu: () => 247);
    transport.onCommand = (command, transactionId) {
      expect(command['op'], 'device.identify');
      expect(
        command['params'],
        isA<Map<Object?, Object?>>().having(
          (value) => value,
          'params',
          isEmpty,
        ),
      );
      final response = BleFrameCodec.encodeJson(
        json: <String, Object?>{
          'v': 1,
          'id': transactionId,
          'ok': true,
          'result': const <String, Object?>{
            'accepted': true,
            'led_index': 2,
            'gpio': 42,
            'duration_ms': 350,
          },
        },
        transactionId: transactionId,
        negotiatedMtu: 247,
      );
      scheduleMicrotask(() {
        for (final fragment in response) {
          queue.onResponseFragment(fragment);
        }
      });
    };

    final result = await queue.execute(
      'device.identify',
      const <String, Object?>{},
    );
    expect(result['accepted'], isTrue);
  });

  test('场景测试只发送结构化操作和场景号', () async {
    final transport = _NotifyTransport();
    late BleTransactionQueue queue;
    queue = BleTransactionQueue(transport: transport, negotiatedMtu: () => 247);
    final operations = <String>[];
    transport.onCommand = (command, transactionId) {
      final operation = command['op']! as String;
      operations.add(operation);
      final params = command['params']! as Map<Object?, Object?>;
      if (operation == 'gs200.scene.set') {
        expect(params, const <String, Object?>{'scene_no': 6});
      } else {
        expect(params, isEmpty);
      }
      final response = BleFrameCodec.encodeJson(
        json: <String, Object?>{
          'v': 1,
          'id': transactionId,
          'ok': true,
          'result': <String, Object?>{
            if (operation == 'gs200.scene.set') 'accepted': true,
            'scene_no': 6,
          },
        },
        transactionId: transactionId,
        negotiatedMtu: 247,
      );
      scheduleMicrotask(() {
        for (final fragment in response) {
          queue.onResponseFragment(fragment);
        }
      });
    };

    await queue.execute('gs200.scene.set', const <String, Object?>{
      'scene_no': 6,
    });
    await queue.execute('gs200.scene.read', const <String, Object?>{});

    expect(operations, <String>['gs200.scene.set', 'gs200.scene.read']);
  });

  test('灯具测试只发送动作和业务目标，不发送底层 YMZL 帧', () async {
    final transport = _NotifyTransport();
    late BleTransactionQueue queue;
    queue = BleTransactionQueue(transport: transport, negotiatedMtu: () => 247);
    transport.onCommand = (command, transactionId) {
      expect(command['op'], 'gs200.light.test');
      expect(command.containsKey('frame'), isFalse);
      expect(command['params'], const <String, Object?>{
        'action': 'flash',
        'target': <String, Object?>{
          'network_id': 124,
          'type': 'virtual',
          'value': 1,
        },
      });
      final response = BleFrameCodec.encodeJson(
        json: <String, Object?>{
          'v': 1,
          'id': transactionId,
          'ok': true,
          'result': const <String, Object?>{
            'accepted': true,
            'action': 'flash',
            'target': <String, Object?>{
              'network_id': 124,
              'type': 'virtual',
              'value': 1,
            },
          },
        },
        transactionId: transactionId,
        negotiatedMtu: 247,
      );
      scheduleMicrotask(() {
        for (final fragment in response) {
          queue.onResponseFragment(fragment);
        }
      });
    };

    await queue.execute('gs200.light.test', const <String, Object?>{
      'action': 'flash',
      'target': <String, Object?>{
        'network_id': 124,
        'type': 'virtual',
        'value': 1,
      },
    });
  });

  test('任务编辑页场景测试发送场景号和当前业务目标', () async {
    final transport = _NotifyTransport();
    late BleTransactionQueue queue;
    queue = BleTransactionQueue(transport: transport, negotiatedMtu: () => 247);
    final operations = <String>[];
    transport.onCommand = (command, transactionId) {
      final operation = command['op']! as String;
      operations.add(operation);
      final params = command['params']! as Map<Object?, Object?>;
      final target = const <String, Object?>{
        'network_id': 124,
        'type': 'virtual',
        'value': 1,
      };
      expect(params['target'], target);
      if (operation == 'gs200.scene.set') {
        expect(params['scene_no'], 6);
      } else {
        expect(params.containsKey('scene_no'), isFalse);
      }
      final response = BleFrameCodec.encodeJson(
        json: <String, Object?>{
          'v': 1,
          'id': transactionId,
          'ok': true,
          'result': <String, Object?>{
            if (operation == 'gs200.scene.set') 'accepted': true,
            'scene_no': 6,
          },
        },
        transactionId: transactionId,
        negotiatedMtu: 247,
      );
      scheduleMicrotask(() {
        for (final fragment in response) {
          queue.onResponseFragment(fragment);
        }
      });
    };

    const target = <String, Object?>{
      'network_id': 124,
      'type': 'virtual',
      'value': 1,
    };
    await queue.execute('gs200.scene.set', const <String, Object?>{
      'scene_no': 6,
      'target': target,
    });
    await queue.execute('gs200.scene.read', const <String, Object?>{
      'target': target,
    });

    expect(operations, <String>['gs200.scene.set', 'gs200.scene.read']);
  });
}

class _NotifyTransport implements BleCommandTransport {
  _NotifyTransport({this.readValue = const <int>[]});

  final List<int> readValue;
  final BleFrameReassembler _commands = BleFrameReassembler();
  void Function(Map<String, Object?> command, int transactionId)? onCommand;
  int writeCount = 0;
  int readCount = 0;
  int _activeWrites = 0;
  int maximumConcurrentWrites = 0;

  @override
  Future<List<int>> readResponse() async {
    readCount++;
    return readValue;
  }

  @override
  Future<void> writeWithResponse(List<int> value) async {
    _activeWrites++;
    if (_activeWrites > maximumConcurrentWrites) {
      maximumConcurrentWrites = _activeWrites;
    }
    writeCount++;
    final frame = BleFrameCodec.decode(Uint8List.fromList(value));
    final command = _commands.accept(value);
    await Future<void>.delayed(Duration.zero);
    _activeWrites--;
    if (command != null) onCommand?.call(command, frame.transactionId);
  }
}
