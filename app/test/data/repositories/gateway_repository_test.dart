import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:yx_gateway_timer/data/ble/ble_scanner.dart';
import 'package:yx_gateway_timer/data/ble/yx_gatt_client.dart';
import 'package:yx_gateway_timer/data/local/device_remark_store.dart';
import 'package:yx_gateway_timer/data/repositories/gateway_repository.dart';
import 'package:yx_gateway_timer/domain/gateway_device.dart';
import 'package:yx_gateway_timer/domain/protocol_error.dart';
import 'package:yx_gateway_timer/domain/timer_task.dart';

void main() {
  test('权限永久拒绝时进入 permissionRequired 并保留设置入口状态', () async {
    final scanner = _FakeScanner()
      ..permission = const BlePermissionResult(
        granted: false,
        permanentlyDenied: true,
      );
    final repository = GatewayRepositoryImpl(
      scanner: scanner,
      client: _FakeClient(),
      remarkStore: _FakeRemarkStore(),
    );
    addTearDown(repository.dispose);
    await repository.initialize();

    expect(repository.state.phase, GatewayConnectionPhase.permissionRequired);
    expect(repository.state.permissionPermanentlyDenied, isTrue);
  });

  test('蓝牙关闭时进入 bluetoothOff 而不是空白扫描页', () async {
    final scanner = _FakeScanner()..currentHost = BleHostState.poweredOff;
    final repository = GatewayRepositoryImpl(
      scanner: scanner,
      client: _FakeClient(),
      remarkStore: _FakeRemarkStore(),
    );
    addTearDown(repository.dispose);
    await repository.initialize();

    expect(repository.state.phase, GatewayConnectionPhase.bluetoothOff);
    expect(repository.state.hostState, BleHostState.poweredOff);
  });

  test('按 device_id 去重且 Peripheral 变化后备注仍关联', () async {
    final scanner = _FakeScanner();
    final store = _FakeRemarkStore(<String, String>{'80B54EE92138': '仓库网关'});
    final repository = GatewayRepositoryImpl(
      scanner: scanner,
      client: _FakeClient(),
      remarkStore: store,
    );
    addTearDown(repository.dispose);
    await repository.initialize();

    scanner.results.add(
      const ScannedGateway(
        deviceId: '80B54EE92138',
        peripheralId: 'temporary-a',
        name: 'YX-2138',
        rssi: -80,
      ),
    );
    scanner.results.add(
      const ScannedGateway(
        deviceId: '80B54EE92138',
        peripheralId: 'temporary-b',
        name: 'YX-2138',
        rssi: -55,
      ),
    );
    await Future<void>.delayed(Duration.zero);

    expect(repository.state.devices, hasLength(1));
    expect(repository.state.devices.single.peripheralId, 'temporary-b');
    expect(repository.state.devices.single.remark, '仓库网关');
  });

  test('连接完成所有初始化阶段后才进入 ready', () async {
    final scanner = _FakeScanner();
    final client = _FakeClient();
    final repository = GatewayRepositoryImpl(
      scanner: scanner,
      client: client,
      remarkStore: _FakeRemarkStore(),
    );
    addTearDown(repository.dispose);
    await repository.initialize();
    scanner.results.add(
      const ScannedGateway(
        deviceId: '80B54EE92138',
        peripheralId: 'temporary-a',
        name: 'YX-2138',
        rssi: -50,
      ),
    );
    await Future<void>.delayed(Duration.zero);

    await repository.connect('80B54EE92138');

    expect(client.phases, <GatewayConnectionPhase>[
      GatewayConnectionPhase.discovering,
      GatewayConnectionPhase.subscribing,
      GatewayConnectionPhase.pairing,
      GatewayConnectionPhase.syncingTime,
      GatewayConnectionPhase.loadingTasks,
    ]);
    expect(repository.state.phase, GatewayConnectionPhase.ready);
    expect(repository.state.currentDevice?.utcTrusted, isTrue);
    expect(repository.state.tasks, hasLength(1));
  });

  test('主动断开后忽略意外断线事件，不进入重连', () async {
    final scanner = _FakeScanner();
    final client = _FakeClient();
    final repository = GatewayRepositoryImpl(
      scanner: scanner,
      client: client,
      remarkStore: _FakeRemarkStore(),
    );
    addTearDown(repository.dispose);
    await repository.initialize();
    scanner.results.add(
      const ScannedGateway(
        deviceId: '80B54EE92138',
        peripheralId: 'temporary-a',
        name: 'YX-2138',
        rssi: -50,
      ),
    );
    await Future<void>.delayed(Duration.zero);
    await repository.connect('80B54EE92138');
    await repository.disconnect();
    client.unexpectedController.add(null);
    await Future<void>.delayed(const Duration(milliseconds: 20));

    expect(client.connectCount, 1);
    expect(repository.state.currentDeviceId, isNull);
    expect(repository.state.phase, GatewayConnectionPhase.scanning);
  });

  test('设备未确认 enable 前任务 UI 状态不提前改变', () async {
    final scanner = _FakeScanner();
    final client = _FakeClient();
    final repository = GatewayRepositoryImpl(
      scanner: scanner,
      client: client,
      remarkStore: _FakeRemarkStore(),
    );
    addTearDown(repository.dispose);
    await repository.initialize();
    scanner.results.add(
      const ScannedGateway(
        deviceId: '80B54EE92138',
        peripheralId: 'temporary-a',
        name: 'YX-2138',
        rssi: -50,
      ),
    );
    await Future<void>.delayed(Duration.zero);
    await repository.connect('80B54EE92138');
    final original = repository.state.tasks.single;
    client.enableGate = Completer<void>();

    final operation = repository.setTaskEnabled(original, false);
    await Future<void>.delayed(Duration.zero);
    expect(repository.state.tasks.single.enabled, isTrue);
    expect(repository.state.busyTaskIds, contains(original.taskId));

    client.enableGate!.complete();
    await operation;
    expect(repository.state.tasks.single.enabled, isFalse);
    expect(repository.state.busyTaskIds, isEmpty);
  });

  test('任务下载到设备后立即强制同步一次时间', () async {
    final scanner = _FakeScanner();
    final client = _FakeClient();
    final repository = GatewayRepositoryImpl(
      scanner: scanner,
      client: client,
      remarkStore: _FakeRemarkStore(),
    );
    addTearDown(repository.dispose);
    await repository.initialize();
    scanner.results.add(
      const ScannedGateway(
        deviceId: '80B54EE92138',
        peripheralId: 'temporary-a',
        name: 'YX-2138',
        rssi: -50,
      ),
    );
    await Future<void>.delayed(Duration.zero);
    await repository.connect('80B54EE92138');

    final task = await repository.getTask(1);
    await repository.saveTask(task);

    expect(client.syncTimeCount, 1);
  });

  test('旧固件未声明能力时拒绝发送识别灯命令', () async {
    final scanner = _FakeScanner();
    final client = _FakeClient();
    final repository = GatewayRepositoryImpl(
      scanner: scanner,
      client: client,
      remarkStore: _FakeRemarkStore(),
    );
    addTearDown(repository.dispose);
    await repository.initialize();
    scanner.results.add(
      const ScannedGateway(
        deviceId: '80B54EE92138',
        peripheralId: 'temporary-a',
        name: 'YX-2138',
        rssi: -50,
      ),
    );
    await Future<void>.delayed(Duration.zero);
    await repository.connect('80B54EE92138');

    await expectLater(
      repository.identifyDevice(),
      throwsA(
        isA<ProtocolException>().having(
          (error) => error.code,
          'code',
          ProtocolErrorCode.unknownOp,
        ),
      ),
    );
    expect(client.identifyCount, 0);
  });

  test('识别灯请求期间保持禁用至少 500ms 并拒绝快速连点', () async {
    final scanner = _FakeScanner();
    final client = _FakeClient(features: const <String>{'device_identify'});
    final repository = GatewayRepositoryImpl(
      scanner: scanner,
      client: client,
      remarkStore: _FakeRemarkStore(),
    );
    addTearDown(repository.dispose);
    await repository.initialize();
    scanner.results.add(
      const ScannedGateway(
        deviceId: '80B54EE92138',
        peripheralId: 'temporary-a',
        name: 'YX-2138',
        rssi: -50,
      ),
    );
    await Future<void>.delayed(Duration.zero);
    await repository.connect('80B54EE92138');
    client.identifyGate = Completer<void>();

    final stopwatch = Stopwatch()..start();
    final operation = repository.identifyDevice();
    await Future<void>.delayed(Duration.zero);
    expect(repository.state.identifyingDevice, isTrue);
    await expectLater(
      repository.identifyDevice(),
      throwsA(isA<ProtocolException>()),
    );
    client.identifyGate!.complete();

    final result = await operation;
    stopwatch.stop();
    expect(result.gpio, 42);
    expect(client.identifyCount, 1);
    expect(stopwatch.elapsedMilliseconds, greaterThanOrEqualTo(490));
    expect(repository.state.identifyingDevice, isFalse);
  });

  test('灯具测试只向支持能力的网关发送结构化动作与业务目标', () async {
    final scanner = _FakeScanner();
    final client = _FakeClient(features: const <String>{'gs200_light_test'});
    final repository = GatewayRepositoryImpl(
      scanner: scanner,
      client: client,
      remarkStore: _FakeRemarkStore(),
    );
    addTearDown(repository.dispose);
    await repository.initialize();
    scanner.results.add(
      const ScannedGateway(
        deviceId: '80B54EE92138',
        peripheralId: 'temporary-a',
        name: 'YX-2138',
        rssi: -50,
      ),
    );
    await Future<void>.delayed(Duration.zero);
    await repository.connect('80B54EE92138');

    const target = LampTarget(
      networkId: 124,
      type: LampTargetType.virtualNumber,
      value: 1,
    );
    final result = await repository.testLamp(LampTestAction.flash, target);

    expect(client.testLampCount, 1);
    expect(client.lastLampAction, LampTestAction.flash);
    expect(client.lastLampTarget, same(target));
    expect(result.target.destinationAddress, 0x8001);
    expect(repository.state.lampCommandBusy, isFalse);
  });

  test('新建任务使用持久化 UUID 幂等键，失败重试复用原键', () async {
    final scanner = _FakeScanner();
    final client = _FakeClient();
    final store = _FakeRemarkStore();
    final repository = GatewayRepositoryImpl(
      scanner: scanner,
      client: client,
      remarkStore: store,
    );
    addTearDown(repository.dispose);
    await repository.initialize();
    scanner.results.add(
      const ScannedGateway(
        deviceId: '80B54EE92138',
        peripheralId: 'temporary-a',
        name: 'YX-2138',
        rssi: -50,
      ),
    );
    await Future<void>.delayed(Duration.zero);
    await repository.connect('80B54EE92138');

    const task = TimerTask(
      enabled: true,
      name: '幂等新建',
      type: TimerType.duration,
      schedule: DurationSchedule(value: 5, unit: DurationUnit.minute),
      action: TimerAction(type: TimerActionType.lightOn),
    );
    client.upsertError = const ProtocolException(
      ProtocolErrorCode.disconnected,
      '模拟响应丢失',
    );
    await expectLater(
      repository.saveTask(task),
      throwsA(isA<ProtocolException>()),
    );

    final pending = store.pendingCreates['80B54EE92138'];
    expect(pending, isNotNull);
    expect(
      pending!.idempotencyKey,
      matches(
        RegExp(
          r'^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$',
        ),
      ),
    );
    final firstKey = client.upsertKeys.single;

    client.upsertError = null;
    await repository.saveTask(task);

    expect(client.upsertKeys, <String?>[firstKey, firstKey]);
    expect(store.pendingCreates, isEmpty);
  });
}

class _FakeScanner implements BleScanner {
  final StreamController<ScannedGateway> results =
      StreamController<ScannedGateway>.broadcast();
  final StreamController<BleHostState> hosts =
      StreamController<BleHostState>.broadcast();
  BleHostState currentHost = BleHostState.ready;
  BlePermissionResult permission = const BlePermissionResult(granted: true);

  @override
  BleHostState get hostState => currentHost;

  @override
  Stream<BleHostState> get hostStateStream => hosts.stream;

  @override
  Future<BlePermissionResult> ensurePermissions() async => permission;

  @override
  Future<void> openSettings() async {}

  @override
  Stream<ScannedGateway> scan() => results.stream;
}

class _FakeRemarkStore implements DeviceRemarkStore {
  _FakeRemarkStore([Map<String, String>? values])
    : values = values ?? <String, String>{};

  final Map<String, String> values;
  final Map<String, PendingTaskCreate> pendingCreates =
      <String, PendingTaskCreate>{};

  @override
  Future<void> close() async {}

  @override
  Future<String?> getRemark(String deviceId) async => values[deviceId];

  @override
  Future<void> initialize() async {}

  @override
  Future<Map<String, String>> loadAllRemarks() async => Map.of(values);

  @override
  Future<void> recordConnected(String deviceId, DateTime connectedAt) async {}

  @override
  Future<PendingTaskCreate?> getPendingTaskCreate(String deviceId) async =>
      pendingCreates[deviceId];

  @override
  Future<void> savePendingTaskCreate(
    String deviceId,
    PendingTaskCreate pending,
  ) async {
    pendingCreates[deviceId] = pending;
  }

  @override
  Future<void> clearPendingTaskCreate(String deviceId) async {
    pendingCreates.remove(deviceId);
  }

  @override
  Future<void> saveRemark(String deviceId, String remark) async {
    values[deviceId] = remark;
  }
}

class _FakeClient implements YxGattClient {
  _FakeClient({this.features = const <String>{}});

  final Set<String> features;
  final StreamController<void> unexpectedController =
      StreamController<void>.broadcast();
  final StreamController<TimerFiredEvent> eventController =
      StreamController<TimerFiredEvent>.broadcast();
  final List<GatewayConnectionPhase> phases = <GatewayConnectionPhase>[];
  bool connected = false;
  int connectCount = 0;
  int identifyCount = 0;
  int testLampCount = 0;
  LampTestAction? lastLampAction;
  LampTarget? lastLampTarget;
  int setSceneCount = 0;
  int readSceneCount = 0;
  int currentScene = 1;
  int syncTimeCount = 0;
  final List<String?> upsertKeys = <String?>[];
  ProtocolException? upsertError;
  Completer<void>? enableGate;
  Completer<void>? identifyGate;
  TimerTaskSummary summary = const TimerTaskSummary(
    taskId: 1,
    revision: 1,
    enabled: true,
    name: '早班任务',
    type: TimerType.daily,
  );

  @override
  bool get isConnected => connected;

  @override
  Stream<TimerFiredEvent> get timerEvents => eventController.stream;

  @override
  Stream<void> get unexpectedDisconnects => unexpectedController.stream;

  @override
  Future<GatewaySessionSnapshot> connectAndInitialize({
    required String peripheralId,
    required String expectedDeviceId,
    required void Function(GatewayConnectionPhase phase) onPhase,
  }) async {
    connectCount++;
    connected = true;
    for (final phase in <GatewayConnectionPhase>[
      GatewayConnectionPhase.discovering,
      GatewayConnectionPhase.subscribing,
      GatewayConnectionPhase.pairing,
      GatewayConnectionPhase.syncingTime,
      GatewayConnectionPhase.loadingTasks,
    ]) {
      phases.add(phase);
      onPhase(phase);
    }
    return GatewaySessionSnapshot(
      info: GatewayInfo(
        deviceId: '80B54EE92138',
        alias: '共享名称',
        firmware: '1.0.0',
        maxTasks: 16,
        taskCount: 1,
        utcTrusted: true,
        tzOffsetMin: 480,
        tableRevision: 1,
        features: features,
      ),
      tasks: <TimerTaskSummary>[summary],
      lastTimeSync: DateTime(2026, 7, 22),
    );
  }

  @override
  Future<void> deleteTask(TimerTaskSummary task) async {}

  @override
  Future<void> disconnect() async => connected = false;

  @override
  Future<void> dispose() async {
    connected = false;
    await unexpectedController.close();
    await eventController.close();
  }

  @override
  Future<String> getAlias() async => '共享名称';

  @override
  Future<DeviceIdentifyResult> identifyDevice() async {
    identifyCount++;
    await identifyGate?.future;
    return const DeviceIdentifyResult(ledIndex: 2, gpio: 42, durationMs: 350);
  }

  @override
  Future<LampTestResult> testLamp(
    LampTestAction action,
    LampTarget target,
  ) async {
    testLampCount++;
    lastLampAction = action;
    lastLampTarget = target;
    return LampTestResult(action: action, target: target);
  }

  @override
  Future<SceneNumberResult> setCurrentScene(
    int sceneNo, {
    LampTarget? target,
  }) async {
    setSceneCount++;
    currentScene = sceneNo;
    return SceneNumberResult(sceneNo: sceneNo);
  }

  @override
  Future<SceneNumberResult> readCurrentScene({LampTarget? target}) async {
    readSceneCount++;
    return SceneNumberResult(sceneNo: currentScene);
  }

  @override
  Future<TimerTask> getTask(int taskId) async => TimerTask(
    taskId: taskId,
    revision: summary.revision,
    enabled: summary.enabled,
    name: summary.name,
    type: TimerType.daily,
    schedule: const DailySchedule(hour: 7, minute: 30),
    action: const TimerAction(type: TimerActionType.lightOn),
  );

  @override
  Future<List<TimerTaskSummary>> listAllTasks() async => <TimerTaskSummary>[
    summary,
  ];

  @override
  Future<String> setAlias(String alias) async => alias;

  @override
  Future<TimerTask> setTaskEnabled(TimerTaskSummary task, bool enabled) async {
    await enableGate?.future;
    summary = task.copyWith(revision: task.revision + 1, enabled: enabled);
    return getTask(task.taskId);
  }

  @override
  Future<DateTime> syncTime() async {
    syncTimeCount++;
    return DateTime.now();
  }

  @override
  Future<TimerTask> upsertTask(TimerTask task, {String? idempotencyKey}) async {
    upsertKeys.add(idempotencyKey);
    if (upsertError case final error?) throw error;
    return task;
  }
}
