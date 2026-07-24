import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:yx_gateway_timer/core/yx_constants.dart';
import 'package:yx_gateway_timer/data/ble/ble_frame_codec.dart';
import 'package:yx_gateway_timer/data/ble/ble_transaction_queue.dart';
import 'package:yx_gateway_timer/domain/gateway_device.dart';
import 'package:yx_gateway_timer/domain/protocol_error.dart';
import 'package:yx_gateway_timer/domain/timer_task.dart';

class GatewayInfo {
  const GatewayInfo({
    required this.deviceId,
    required this.alias,
    required this.firmware,
    required this.maxTasks,
    required this.taskCount,
    required this.utcTrusted,
    required this.tzOffsetMin,
    required this.tableRevision,
    this.features = const <String>{},
  });

  final String deviceId;
  final String alias;
  final String firmware;
  final int maxTasks;
  final int taskCount;
  final bool utcTrusted;
  final int tzOffsetMin;
  final int tableRevision;
  final Set<String> features;

  factory GatewayInfo.fromJson(Object? value) {
    final map = _map(value, 'Info');
    final deviceId = _string(map['device_id'], 'device_id').toUpperCase();
    if (!RegExp(r'^[0-9A-F]{12}$').hasMatch(deviceId)) {
      throw const ProtocolException(
        ProtocolErrorCode.malformedResponse,
        'device_id 格式错误',
      );
    }
    if (_integer(map['protocol'], 'protocol') != YxProtocol.version) {
      throw const ProtocolException(
        ProtocolErrorCode.malformedResponse,
        '设备协议版本不受支持',
      );
    }
    return GatewayInfo(
      deviceId: deviceId,
      alias: _string(map['alias'], 'alias'),
      firmware: _string(map['firmware'], 'firmware'),
      maxTasks: _integer(map['max_tasks'], 'max_tasks'),
      taskCount: _integer(map['task_count'], 'task_count'),
      utcTrusted: _boolean(map['utc_trusted'], 'utc_trusted'),
      tzOffsetMin: _integer(map['tz_offset_min'], 'tz_offset_min'),
      tableRevision: _integer(map['table_revision'], 'table_revision'),
      features: _features(map['features']),
    );
  }
}

class DeviceIdentifyResult {
  const DeviceIdentifyResult({
    required this.ledIndex,
    required this.gpio,
    required this.durationMs,
  });

  final int ledIndex;
  final int gpio;
  final int durationMs;

  factory DeviceIdentifyResult.fromJson(Object? value) {
    final map = _map(value, 'device.identify result');
    if (!_boolean(map['accepted'], 'accepted')) {
      throw const ProtocolException(
        ProtocolErrorCode.internalError,
        '网关未接受识别灯请求',
      );
    }
    return DeviceIdentifyResult(
      ledIndex: _integer(map['led_index'], 'led_index'),
      gpio: _integer(map['gpio'], 'gpio'),
      durationMs: _integer(map['duration_ms'], 'duration_ms'),
    );
  }
}

class SceneNumberResult {
  const SceneNumberResult({required this.sceneNo});

  final int sceneNo;

  factory SceneNumberResult.fromJson(
    Object? value, {
    required bool requireAccepted,
  }) {
    final map = _map(value, 'gs200.scene result');
    if (requireAccepted && !_boolean(map['accepted'], 'accepted')) {
      throw const ProtocolException(
        ProtocolErrorCode.internalError,
        '网关未接受场景号设置请求',
      );
    }
    final sceneNo = _integer(map['scene_no'], 'scene_no');
    if (sceneNo < 0 || sceneNo > 255) {
      throw const ProtocolException(
        ProtocolErrorCode.malformedResponse,
        '设备返回的场景号超出 0～255',
      );
    }
    return SceneNumberResult(sceneNo: sceneNo);
  }
}

class LampTestResult {
  const LampTestResult({required this.action, required this.target});

  final LampTestAction action;
  final LampTarget target;

  factory LampTestResult.fromJson(Object? value) {
    final map = _map(value, 'gs200.light.test result');
    if (!_boolean(map['accepted'], 'accepted')) {
      throw const ProtocolException(
        ProtocolErrorCode.internalError,
        '网关未接受灯具测试指令',
      );
    }
    return LampTestResult(
      action: LampTestAction.parse(map['action']),
      target: LampTarget.fromJson(map['target']),
    );
  }
}

class GatewaySessionSnapshot {
  const GatewaySessionSnapshot({
    required this.info,
    required this.tasks,
    required this.lastTimeSync,
  });

  final GatewayInfo info;
  final List<TimerTaskSummary> tasks;
  final DateTime lastTimeSync;
}

class TimerFiredEvent {
  const TimerFiredEvent({
    required this.taskId,
    required this.occurrenceId,
    required this.scheduledUtcMs,
    required this.success,
    required this.espError,
    required this.receivedAt,
  });

  final int taskId;
  final int occurrenceId;
  final int scheduledUtcMs;
  final bool success;
  final int espError;
  final DateTime receivedAt;

  factory TimerFiredEvent.fromJson(Object? value) {
    final root = _map(value, 'Event');
    if (root['v'] != YxProtocol.version || root['event'] != 'timer.fired') {
      throw const ProtocolException(
        ProtocolErrorCode.malformedResponse,
        '未知的事件格式',
      );
    }
    final data = _map(root['data'], 'event.data');
    return TimerFiredEvent(
      taskId: _integer(data['task_id'], 'task_id'),
      occurrenceId: _integer(data['occurrence_id'], 'occurrence_id'),
      scheduledUtcMs: _integer(data['scheduled_utc_ms'], 'scheduled_utc_ms'),
      success: _boolean(data['success'], 'success'),
      espError: _integer(data['esp_err'], 'esp_err'),
      receivedAt: DateTime.now(),
    );
  }
}

abstract interface class YxGattClient {
  Stream<void> get unexpectedDisconnects;
  Stream<TimerFiredEvent> get timerEvents;
  bool get isConnected;

  Future<GatewaySessionSnapshot> connectAndInitialize({
    required String peripheralId,
    required String expectedDeviceId,
    required void Function(GatewayConnectionPhase phase) onPhase,
  });

  Future<DateTime> syncTime();
  Future<List<TimerTaskSummary>> listAllTasks();
  Future<TimerTask> getTask(int taskId);
  Future<TimerTask> upsertTask(TimerTask task, {String? idempotencyKey});
  Future<TimerTask> setTaskEnabled(TimerTaskSummary task, bool enabled);
  Future<void> deleteTask(TimerTaskSummary task);
  Future<String> getAlias();
  Future<String> setAlias(String alias);
  Future<DeviceIdentifyResult> identifyDevice();
  Future<LampTestResult> testLamp(LampTestAction action, LampTarget target);
  Future<SceneNumberResult> setCurrentScene(int sceneNo, {LampTarget? target});
  Future<SceneNumberResult> readCurrentScene({LampTarget? target});
  Future<void> disconnect();
  Future<void> dispose();
}

class ReactiveYxGattClient implements YxGattClient {
  ReactiveYxGattClient(this._ble);

  @visibleForTesting
  static bool shouldReadInfoBeforeHello(TargetPlatform platform) =>
      platform != TargetPlatform.android;

  @visibleForTesting
  static bool shouldProbeEncryptedResponseBeforeHello(
    TargetPlatform platform,
  ) => platform != TargetPlatform.android;

  @visibleForTesting
  static bool isBleTransportFailure(Object error) {
    final message = error.toString().toLowerCase();
    return error is TimeoutException ||
        message.contains('writecharacteristicfailure') ||
        message.contains('readcharacteristicfailure') ||
        message.contains('bledisconnectedexception') ||
        message.contains('connectionfailure') ||
        message.contains('gatt');
  }

  static const MethodChannel _bondChannel = MethodChannel(
    'com.yx.yx_gateway_timer/bonding',
  );

  final FlutterReactiveBle _ble;
  final StreamController<void> _unexpectedDisconnectController =
      StreamController<void>.broadcast();
  final StreamController<TimerFiredEvent> _eventController =
      StreamController<TimerFiredEvent>.broadcast();
  final BleFrameReassembler _eventReassembler = BleFrameReassembler();

  StreamSubscription<ConnectionStateUpdate>? _connectionSubscription;
  StreamSubscription<List<int>>? _responseSubscription;
  StreamSubscription<List<int>>? _eventSubscription;
  QualifiedCharacteristic? _commandCharacteristic;
  QualifiedCharacteristic? _responseCharacteristic;
  BleTransactionQueue? _transactions;
  int _mtu = 23;
  int _connectionGeneration = 0;
  bool _connected = false;
  bool _intentionalDisconnect = false;

  @override
  Stream<void> get unexpectedDisconnects =>
      _unexpectedDisconnectController.stream;

  @override
  Stream<TimerFiredEvent> get timerEvents => _eventController.stream;

  @override
  bool get isConnected => _connected;

  @override
  Future<GatewaySessionSnapshot> connectAndInitialize({
    required String peripheralId,
    required String expectedDeviceId,
    required void Function(GatewayConnectionPhase phase) onPhase,
  }) async {
    await _clearConnection(intentional: true);
    _intentionalDisconnect = false;
    final generation = ++_connectionGeneration;
    final connected = Completer<void>();

    onPhase(GatewayConnectionPhase.connecting);
    _connectionSubscription = _ble
        .connectToDevice(
          id: peripheralId,
          servicesWithCharacteristicsToDiscover: <Uuid, List<Uuid>>{
            YxGatt.service: <Uuid>[
              YxGatt.info,
              YxGatt.command,
              YxGatt.response,
              YxGatt.event,
            ],
          },
          connectionTimeout: const Duration(seconds: 12),
        )
        .listen(
          (update) {
            if (generation != _connectionGeneration) return;
            switch (update.connectionState) {
              case DeviceConnectionState.connected:
                _connected = true;
                if (!connected.isCompleted) connected.complete();
              case DeviceConnectionState.disconnected:
                _handleDisconnected(connected);
              case DeviceConnectionState.connecting ||
                  DeviceConnectionState.disconnecting:
                break;
            }
          },
          onError: (Object error, StackTrace stackTrace) {
            if (!connected.isCompleted) {
              connected.completeError(error, stackTrace);
            }
            _handleDisconnected(connected);
          },
        );

    try {
      await connected.future.timeout(const Duration(seconds: 15));
      try {
        _mtu = await _ble.requestMtu(deviceId: peripheralId, mtu: 247);
      } on Object {
        // iOS/旧设备可能拒绝主动协商，此时协议使用最保守的 ATT MTU。
        _mtu = 23;
      }

      onPhase(GatewayConnectionPhase.discovering);
      await _discoverAllServices(peripheralId);
      final services = await _ble.getDiscoveredServices(peripheralId);
      _verifyGatt(services);

      _commandCharacteristic = QualifiedCharacteristic(
        serviceId: YxGatt.service,
        characteristicId: YxGatt.command,
        deviceId: peripheralId,
      );
      _responseCharacteristic = QualifiedCharacteristic(
        serviceId: YxGatt.service,
        characteristicId: YxGatt.response,
        deviceId: peripheralId,
      );
      final infoCharacteristic = QualifiedCharacteristic(
        serviceId: YxGatt.service,
        characteristicId: YxGatt.info,
        deviceId: peripheralId,
      );
      final eventCharacteristic = QualifiedCharacteristic(
        serviceId: YxGatt.service,
        characteristicId: YxGatt.event,
        deviceId: peripheralId,
      );
      _transactions = BleTransactionQueue(
        transport: _ReactiveBleCommandTransport(this),
        negotiatedMtu: () => _mtu,
      )..markConnected();

      if (Platform.isAndroid) {
        onPhase(GatewayConnectionPhase.pairing);
        final bonded =
            await _bondChannel.invokeMethod<bool>(
              'waitForBond',
              <String, Object?>{'deviceId': peripheralId},
            ) ??
            false;
        if (!bonded) {
          throw const ProtocolException(
            ProtocolErrorCode.disconnected,
            '系统配对未完成或已取消',
          );
        }
      }

      // Android 先完成 bonding 再写 CCCD，避免配对状态切换期间反复取消、重订阅
      // 触发厂商 GATT 133；iOS 仍由加密特征访问交给系统完成配对。
      onPhase(GatewayConnectionPhase.subscribing);
      _subscribeCharacteristics(eventCharacteristic);
      await Future<void>.delayed(const Duration(milliseconds: 300));

      GatewayInfo? info;
      if (!Platform.isAndroid) {
        try {
          final infoBytes = await _ble.readCharacteristic(infoCharacteristic);
          info = GatewayInfo.fromJson(_decodePlainJson(infoBytes, 'Info'));
        } on Object catch (error) {
          // 保留失败阶段但不记录特征内容，便于在不同厂商蓝牙栈上定位兼容问题。
          throw ProtocolException(
            ProtocolErrorCode.internalError,
            '读取 Info 特征失败：$error',
            cause: error,
          );
        }
      } else {
        // Android 的大 MTU 长读取会让部分旧固件在动态 Info 的 Read Blob 处理中
        // 停止响应，最终触发 GATT status 8。Android 直接使用后续加密 hello；
        // hello 仍须与广播 device_id 一致，因此不会绕过设备身份校验。
        info = null;
      }
      if (info != null && info.deviceId != expectedDeviceId.toUpperCase()) {
        throw const ProtocolException(
          ProtocolErrorCode.identityConflict,
          '广播身份与 Info 身份不一致',
        );
      }

      if (!Platform.isAndroid) {
        onPhase(GatewayConnectionPhase.pairing);
        // iOS 通过首次访问加密特征触发系统配对；Android 已在上方显式等待 bond，
        // 不再读取可能残留长响应的 Response，避免部分厂商栈沿用 Read Blob
        // offset 后返回 GATT_INVALID_OFFSET。随后的 hello 写入同样要求加密。
        try {
          await _ble.readCharacteristic(_responseCharacteristic!);
        } on Object catch (error) {
          throw ProtocolException(
            ProtocolErrorCode.internalError,
            '访问加密 Response 特征失败：$error',
            cause: error,
          );
        }
      }
      final hello = GatewayInfo.fromJson(await _execute('hello'));
      if (hello.deviceId != expectedDeviceId.toUpperCase() ||
          (info != null && hello.deviceId != info.deviceId)) {
        throw const ProtocolException(
          ProtocolErrorCode.identityConflict,
          '广播、Info 与 hello 身份不一致',
        );
      }

      onPhase(GatewayConnectionPhase.syncingTime);
      final lastTimeSync = await syncTime();
      onPhase(GatewayConnectionPhase.loadingTasks);
      final tasks = await listAllTasks();
      return GatewaySessionSnapshot(
        info: GatewayInfo(
          deviceId: hello.deviceId,
          alias: hello.alias,
          firmware: hello.firmware,
          maxTasks: hello.maxTasks,
          taskCount: tasks.length,
          utcTrusted: true,
          tzOffsetMin: DateTime.now().timeZoneOffset.inMinutes,
          tableRevision: hello.tableRevision,
          features: hello.features,
        ),
        tasks: tasks,
        lastTimeSync: lastTimeSync,
      );
    } on Object catch (error, stackTrace) {
      await _clearConnection(intentional: true);
      if (error is! ProtocolException && isBleTransportFailure(error)) {
        Error.throwWithStackTrace(
          ProtocolException(
            ProtocolErrorCode.disconnected,
            '网关蓝牙连接中断，请重试；若持续失败，请更新网关固件。',
            cause: error,
          ),
          stackTrace,
        );
      }
      rethrow;
    }
  }

  @override
  Future<DateTime> syncTime() async {
    final now = DateTime.now();
    await _execute('time.sync', <String, Object?>{
      'utc_ms': now.toUtc().millisecondsSinceEpoch,
      'tz_offset_min': now.timeZoneOffset.inMinutes,
    });
    return now;
  }

  @override
  Future<List<TimerTaskSummary>> listAllTasks() async {
    var cursor = 0;
    final visitedCursors = <int>{};
    final tasks = <TimerTaskSummary>[];
    while (true) {
      if (!visitedCursors.add(cursor)) {
        throw const ProtocolException(
          ProtocolErrorCode.malformedResponse,
          'timer.list 返回了循环 cursor',
        );
      }
      final result = await _execute('timer.list', <String, Object?>{
        'cursor': cursor,
        'limit': 3,
      });
      final items = result['items'];
      if (items is! List || result['done'] is! bool) {
        throw const ProtocolException(
          ProtocolErrorCode.malformedResponse,
          'timer.list 分页响应格式错误',
        );
      }
      tasks.addAll(items.map(TimerTaskSummary.fromJson));
      if (result['done']! as bool) break;
      cursor = _integer(result['next_cursor'], 'next_cursor');
    }
    return List<TimerTaskSummary>.unmodifiable(tasks);
  }

  @override
  Future<TimerTask> getTask(int taskId) async {
    final result = await _execute('timer.get', <String, Object?>{
      'task_id': taskId,
    });
    return TimerTask.fromJson(result['task']);
  }

  @override
  Future<TimerTask> upsertTask(TimerTask task, {String? idempotencyKey}) async {
    if (task.taskId == null &&
        (idempotencyKey == null ||
            !RegExp(
              r'^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$',
            ).hasMatch(idempotencyKey))) {
      throw const ProtocolException(
        ProtocolErrorCode.invalidParam,
        '新建任务缺少有效的幂等键',
      );
    }
    final result = await _execute('timer.upsert', <String, Object?>{
      if (task.taskId == null) 'idempotency_key': idempotencyKey!,
      'task': task.toJson(),
    });
    return TimerTask.fromJson(result['task']);
  }

  @override
  Future<TimerTask> setTaskEnabled(TimerTaskSummary task, bool enabled) async {
    final result = await _execute('timer.enable', <String, Object?>{
      'task_id': task.taskId,
      'revision': task.revision,
      'enabled': enabled,
    });
    return TimerTask.fromJson(result['task']);
  }

  @override
  Future<void> deleteTask(TimerTaskSummary task) async {
    await _execute('timer.delete', <String, Object?>{
      'task_id': task.taskId,
      'revision': task.revision,
    });
  }

  @override
  Future<String> getAlias() async {
    final result = await _execute('device.alias.get');
    return _string(result['alias'], 'alias');
  }

  @override
  Future<String> setAlias(String alias) async {
    if (utf8.encode(alias).length > 31) {
      throw const ProtocolException(
        ProtocolErrorCode.invalidParam,
        '设备 alias 最多 31 个 UTF-8 字节',
      );
    }
    final result = await _execute('device.alias.set', <String, Object?>{
      'alias': alias,
    });
    return _string(result['alias'], 'alias');
  }

  @override
  Future<DeviceIdentifyResult> identifyDevice() async =>
      DeviceIdentifyResult.fromJson(await _execute('device.identify'));

  @override
  Future<LampTestResult> testLamp(
    LampTestAction action,
    LampTarget target,
  ) async {
    final errors = target.validate();
    if (errors.isNotEmpty) {
      throw ProtocolException(ProtocolErrorCode.invalidParam, errors.first);
    }
    return LampTestResult.fromJson(
      await _execute('gs200.light.test', <String, Object?>{
        'action': action.wireName,
        'target': target.toJson(),
      }),
    );
  }

  @override
  Future<SceneNumberResult> setCurrentScene(
    int sceneNo, {
    LampTarget? target,
  }) async {
    if (sceneNo < 1 || sceneNo > 16) {
      throw const ProtocolException(
        ProtocolErrorCode.invalidParam,
        '场景号必须在 1～16 之间',
      );
    }
    final parameters = <String, Object?>{'scene_no': sceneNo};
    if (target != null) {
      final errors = target.validate();
      if (errors.isNotEmpty) {
        throw ProtocolException(ProtocolErrorCode.invalidParam, errors.first);
      }
      parameters['target'] = target.toJson();
    }
    return SceneNumberResult.fromJson(
      await _execute('gs200.scene.set', parameters),
      requireAccepted: true,
    );
  }

  @override
  Future<SceneNumberResult> readCurrentScene({LampTarget? target}) async {
    final parameters = <String, Object?>{};
    if (target != null) {
      final errors = target.validate();
      if (errors.isNotEmpty) {
        throw ProtocolException(ProtocolErrorCode.invalidParam, errors.first);
      }
      parameters['target'] = target.toJson();
    }
    return SceneNumberResult.fromJson(
      await _execute('gs200.scene.read', parameters),
      requireAccepted: false,
    );
  }

  Future<Map<String, Object?>> _execute(
    String operation, [
    Map<String, Object?> parameters = const <String, Object?>{},
  ]) {
    final queue = _transactions;
    if (!_connected || queue == null) {
      throw const ProtocolException(ProtocolErrorCode.disconnected, '设备尚未连接');
    }
    return queue.execute(operation, parameters);
  }

  void _subscribeCharacteristics(QualifiedCharacteristic eventCharacteristic) {
    _responseSubscription = _ble
        .subscribeToCharacteristic(_responseCharacteristic!)
        .listen(
          _transactions!.onResponseFragment,
          onError: (Object _) {
            // 首次写 CCCD 可能触发系统配对并返回 AUTH_FAIL；完成 bonding 后会
            // 重新订阅。命令阶段若订阅异常，事务超时和单次 Read 恢复负责兜底。
          },
        );
    _eventSubscription = _ble
        .subscribeToCharacteristic(eventCharacteristic)
        .listen(
          _onEventFragment,
          onError: (Object _) {
            // Event 与命令事务隔离，异常事件流不能终止 Response 处理。
          },
        );
  }

  Future<void> _discoverAllServices(String peripheralId) async {
    final deadline = DateTime.now().add(const Duration(seconds: 45));
    var waitedForSystemBond = false;
    while (true) {
      if (!_connected) {
        throw const ProtocolException(
          ProtocolErrorCode.disconnected,
          '配对期间 BLE 连接已断开',
        );
      }
      try {
        await _ble.discoverAllServices(peripheralId);
        return;
      } on Object catch (error) {
        if (!_isBondingInProgressError(error) ||
            DateTime.now().isAfter(deadline)) {
          rethrow;
        }
        // ColorOS 等系统可能在首次发现加密特征时启动 bonding；等待系统明确
        // 报告配对完成后复用当前连接，避免轮询操作与配对状态机相互竞争。
        if (!waitedForSystemBond && Platform.isAndroid) {
          waitedForSystemBond = true;
          final bonded =
              await _bondChannel.invokeMethod<bool>(
                'waitForBond',
                <String, Object?>{'deviceId': peripheralId},
              ) ??
              false;
          if (!bonded) {
            throw const ProtocolException(
              ProtocolErrorCode.disconnected,
              '系统配对未完成或已取消',
            );
          }
        }
        await Future<void>.delayed(const Duration(milliseconds: 500));
      }
    }
  }

  void _verifyGatt(List<Service> services) {
    final service = services
        .where((item) => item.id == YxGatt.service)
        .firstOrNull;
    if (service == null) {
      throw const ProtocolException(
        ProtocolErrorCode.badRequest,
        '未发现 Timer Service',
      );
    }
    final characteristicIds = service.characteristics
        .map((item) => item.id)
        .toSet();
    if (!characteristicIds.containsAll(<Uuid>{
      YxGatt.info,
      YxGatt.command,
      YxGatt.response,
      YxGatt.event,
    })) {
      throw const ProtocolException(
        ProtocolErrorCode.badRequest,
        'Timer Service 特征不完整',
      );
    }
  }

  void _onEventFragment(List<int> value) {
    try {
      final event = _eventReassembler.accept(value);
      if (event != null && !_eventController.isClosed) {
        _eventController.add(TimerFiredEvent.fromJson(event));
      }
    } on Object {
      // 外部 BLE 输入无效时丢弃事件，绝不影响命令事务。
      _eventReassembler.reset();
    }
  }

  void _handleDisconnected(Completer<void> initialConnection) {
    final wasConnected = _connected;
    _connected = false;
    _transactions?.reset();
    _eventReassembler.reset();
    if (!initialConnection.isCompleted) {
      initialConnection.completeError(
        const ProtocolException(ProtocolErrorCode.disconnected, '建立 BLE 连接失败'),
      );
    } else if (wasConnected && !_intentionalDisconnect) {
      if (!_unexpectedDisconnectController.isClosed) {
        _unexpectedDisconnectController.add(null);
      }
    }
  }

  @override
  Future<void> disconnect() => _clearConnection(intentional: true);

  Future<void> _clearConnection({required bool intentional}) async {
    _intentionalDisconnect = intentional;
    _connectionGeneration++;
    _connected = false;
    _transactions?.reset();
    _transactions = null;
    _eventReassembler.reset();
    await _responseSubscription?.cancel();
    await _eventSubscription?.cancel();
    await _connectionSubscription?.cancel();
    _responseSubscription = null;
    _eventSubscription = null;
    _connectionSubscription = null;
    _commandCharacteristic = null;
    _responseCharacteristic = null;
  }

  @override
  Future<void> dispose() async {
    await _clearConnection(intentional: true);
    await _unexpectedDisconnectController.close();
    await _eventController.close();
  }
}

class _ReactiveBleCommandTransport implements BleCommandTransport {
  const _ReactiveBleCommandTransport(this.client);

  final ReactiveYxGattClient client;

  @override
  Future<List<int>> readResponse() =>
      client._ble.readCharacteristic(client._responseCharacteristic!);

  @override
  Future<void> writeWithResponse(List<int> value) =>
      client._ble.writeCharacteristicWithResponse(
        client._commandCharacteristic!,
        value: value,
      );
}

bool _isBondingInProgressError(Object error) =>
    error.toString().toLowerCase().contains('bonding is in progress');

Object? _decodePlainJson(List<int> bytes, String name) {
  try {
    return jsonDecode(utf8.decode(bytes, allowMalformed: false));
  } on Object catch (error) {
    throw ProtocolException(
      ProtocolErrorCode.badJson,
      '$name 不是有效 UTF-8 JSON',
      cause: error,
    );
  }
}

Map<String, Object?> _map(Object? value, String name) {
  if (value is! Map) {
    throw ProtocolException(ProtocolErrorCode.malformedResponse, '$name 类型错误');
  }
  return value.map<String, Object?>(
    (key, item) => MapEntry(key.toString(), item),
  );
}

String _string(Object? value, String name) {
  if (value is! String) {
    throw ProtocolException(ProtocolErrorCode.malformedResponse, '$name 类型错误');
  }
  return value;
}

int _integer(Object? value, String name) {
  if (value is! int) {
    throw ProtocolException(ProtocolErrorCode.malformedResponse, '$name 类型错误');
  }
  return value;
}

bool _boolean(Object? value, String name) {
  if (value is! bool) {
    throw ProtocolException(ProtocolErrorCode.malformedResponse, '$name 类型错误');
  }
  return value;
}

Set<String> _features(Object? value) {
  if (value == null) return const <String>{};
  if (value is! List || value.any((item) => item is! String)) {
    throw const ProtocolException(
      ProtocolErrorCode.malformedResponse,
      'features 类型错误',
    );
  }
  return Set<String>.unmodifiable(value.cast<String>());
}
