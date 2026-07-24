import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:yx_gateway_timer/core/idempotency_key.dart';
import 'package:yx_gateway_timer/core/yx_constants.dart';
import 'package:yx_gateway_timer/data/ble/ble_scanner.dart';
import 'package:yx_gateway_timer/data/ble/yx_gatt_client.dart';
import 'package:yx_gateway_timer/data/local/device_remark_store.dart';
import 'package:yx_gateway_timer/domain/gateway_device.dart';
import 'package:yx_gateway_timer/domain/protocol_error.dart';
import 'package:yx_gateway_timer/domain/timer_task.dart';

class GatewayRepositoryState {
  const GatewayRepositoryState({
    this.phase = GatewayConnectionPhase.idle,
    this.hostState = BleHostState.unknown,
    this.devices = const <GatewayDevice>[],
    this.tasks = const <TimerTaskSummary>[],
    this.events = const <TimerFiredEvent>[],
    this.busyTaskIds = const <int>{},
    this.currentDeviceId,
    this.errorMessage,
    this.permissionPermanentlyDenied = false,
    this.initialized = false,
    this.identifyingDevice = false,
    this.lampCommandBusy = false,
    this.sceneCommandBusy = false,
  });

  final GatewayConnectionPhase phase;
  final BleHostState hostState;
  final List<GatewayDevice> devices;
  final List<TimerTaskSummary> tasks;
  final List<TimerFiredEvent> events;
  final Set<int> busyTaskIds;
  final String? currentDeviceId;
  final String? errorMessage;
  final bool permissionPermanentlyDenied;
  final bool initialized;
  final bool identifyingDevice;
  final bool lampCommandBusy;
  final bool sceneCommandBusy;

  GatewayDevice? get currentDevice => currentDeviceId == null
      ? null
      : devices.where((item) => item.deviceId == currentDeviceId).firstOrNull;

  GatewayRepositoryState copyWith({
    GatewayConnectionPhase? phase,
    BleHostState? hostState,
    List<GatewayDevice>? devices,
    List<TimerTaskSummary>? tasks,
    List<TimerFiredEvent>? events,
    Set<int>? busyTaskIds,
    String? currentDeviceId,
    bool clearCurrentDevice = false,
    String? errorMessage,
    bool clearError = false,
    bool? permissionPermanentlyDenied,
    bool? initialized,
    bool? identifyingDevice,
    bool? lampCommandBusy,
    bool? sceneCommandBusy,
  }) {
    return GatewayRepositoryState(
      phase: phase ?? this.phase,
      hostState: hostState ?? this.hostState,
      devices: devices ?? this.devices,
      tasks: tasks ?? this.tasks,
      events: events ?? this.events,
      busyTaskIds: busyTaskIds ?? this.busyTaskIds,
      currentDeviceId: clearCurrentDevice
          ? null
          : currentDeviceId ?? this.currentDeviceId,
      errorMessage: clearError ? null : errorMessage ?? this.errorMessage,
      permissionPermanentlyDenied:
          permissionPermanentlyDenied ?? this.permissionPermanentlyDenied,
      initialized: initialized ?? this.initialized,
      identifyingDevice: identifyingDevice ?? this.identifyingDevice,
      lampCommandBusy: lampCommandBusy ?? this.lampCommandBusy,
      sceneCommandBusy: sceneCommandBusy ?? this.sceneCommandBusy,
    );
  }
}

abstract interface class GatewayRepository {
  Stream<GatewayRepositoryState> get states;
  GatewayRepositoryState get state;

  Future<void> initialize();
  Future<void> startScanning();
  Future<void> openSettings();
  Future<void> saveRemark(String deviceId, String remark);
  Future<void> connect(String deviceId);
  Future<void> disconnect();
  Future<void> resyncTime({bool force = false});
  Future<void> refreshTasks();
  Future<TimerTask> getTask(int taskId);
  Future<TimerTask> saveTask(TimerTask task);
  Future<void> setTaskEnabled(TimerTaskSummary task, bool enabled);
  Future<void> deleteTask(TimerTaskSummary task);
  Future<void> setAlias(String alias);
  Future<DeviceIdentifyResult> identifyDevice();
  Future<LampTestResult> testLamp(LampTestAction action, LampTarget target);
  Future<SceneNumberResult> setCurrentScene(int sceneNo, {LampTarget? target});
  Future<SceneNumberResult> readCurrentScene({LampTarget? target});
  Future<void> onAppResumed();
  Future<void> onAppPaused();
  Future<void> dispose();
}

class GatewayRepositoryImpl implements GatewayRepository {
  GatewayRepositoryImpl({
    required BleScanner scanner,
    required YxGattClient client,
    required DeviceRemarkStore remarkStore,
  }) : _scanner = scanner,
       _client = client,
       _remarkStore = remarkStore {
    _hostStateSubscription = _scanner.hostStateStream.listen(_onHostState);
    _disconnectSubscription = _client.unexpectedDisconnects.listen(
      (_) => _onUnexpectedDisconnect(),
    );
    _eventSubscription = _client.timerEvents.listen(_onTimerEvent);
  }

  final BleScanner _scanner;
  final YxGattClient _client;
  final DeviceRemarkStore _remarkStore;
  final StreamController<GatewayRepositoryState> _stateController =
      StreamController<GatewayRepositoryState>.broadcast();
  final Map<String, GatewayDevice> _devices = <String, GatewayDevice>{};
  final Map<String, String> _remarks = <String, String>{};

  StreamSubscription<ScannedGateway>? _scanSubscription;
  StreamSubscription<BleHostState>? _hostStateSubscription;
  StreamSubscription<void>? _disconnectSubscription;
  StreamSubscription<TimerFiredEvent>? _eventSubscription;
  GatewayRepositoryState _state = const GatewayRepositoryState();
  int _reconnectGeneration = 0;
  bool _userDisconnected = true;
  bool _connecting = false;
  DateTime? _lastTimeSync;
  int? _lastTimezoneOffset;

  @override
  Stream<GatewayRepositoryState> get states => _stateController.stream;

  @override
  GatewayRepositoryState get state => _state;

  void _emit(GatewayRepositoryState value) {
    _state = value;
    if (!_stateController.isClosed) _stateController.add(value);
  }

  @override
  Future<void> initialize() async {
    await _remarkStore.initialize();
    _remarks
      ..clear()
      ..addAll(await _remarkStore.loadAllRemarks());
    _emit(_state.copyWith(initialized: true));
    await startScanning();
  }

  @override
  Future<void> startScanning() async {
    if (_connecting || _client.isConnected) return;
    await _scanSubscription?.cancel();
    _scanSubscription = null;
    final permission = await _scanner.ensurePermissions();
    if (!permission.granted) {
      _emit(
        _state.copyWith(
          phase: GatewayConnectionPhase.permissionRequired,
          hostState: permission.locationServicesOff
              ? BleHostState.locationServicesOff
              : _state.hostState,
          permissionPermanentlyDenied: permission.permanentlyDenied,
          clearError: true,
        ),
      );
      return;
    }
    final host = _scanner.hostState;
    if (host != BleHostState.ready) {
      _onHostState(host);
      return;
    }

    _emit(
      _state.copyWith(
        phase: GatewayConnectionPhase.scanning,
        hostState: host,
        permissionPermanentlyDenied: false,
        clearError: true,
      ),
    );
    _scanSubscription = _scanner.scan().listen(
      _mergeScanResult,
      onError: (Object error) {
        final message = error.toString();
        final locationOff = message.toLowerCase().contains('location');
        _emit(
          _state.copyWith(
            phase: locationOff
                ? GatewayConnectionPhase.permissionRequired
                : GatewayConnectionPhase.error,
            hostState: locationOff
                ? BleHostState.locationServicesOff
                : _state.hostState,
            errorMessage: locationOff ? null : '扫描失败：$message',
            clearError: locationOff,
          ),
        );
      },
    );
  }

  void _onHostState(BleHostState host) {
    final phase = switch (host) {
      BleHostState.ready =>
        _client.isConnected ? _state.phase : GatewayConnectionPhase.idle,
      BleHostState.poweredOff => GatewayConnectionPhase.bluetoothOff,
      BleHostState.unauthorized || BleHostState.locationServicesOff =>
        GatewayConnectionPhase.permissionRequired,
      BleHostState.unsupported => GatewayConnectionPhase.error,
      _ => _state.phase,
    };
    _emit(
      _state.copyWith(
        hostState: host,
        phase: phase,
        errorMessage: host == BleHostState.unsupported ? '此设备不支持 BLE' : null,
        clearError: host != BleHostState.unsupported,
        permissionPermanentlyDenied:
            host == BleHostState.unauthorized && Platform.isIOS
            ? true
            : _state.permissionPermanentlyDenied,
      ),
    );
    if (host == BleHostState.ready &&
        !_client.isConnected &&
        !_connecting &&
        _scanSubscription == null &&
        _state.initialized) {
      unawaited(startScanning());
    }
  }

  void _mergeScanResult(ScannedGateway scanned) {
    final existing = _devices[scanned.deviceId];
    _devices[scanned.deviceId] = GatewayDevice(
      deviceId: scanned.deviceId,
      peripheralId: scanned.peripheralId,
      rssi: scanned.rssi,
      advertisedName: scanned.name,
      remark: _remarks[scanned.deviceId] ?? existing?.remark ?? '',
      alias: existing?.alias ?? '',
      firmware: existing?.firmware,
      maxTasks: existing?.maxTasks ?? YxProtocol.maxTasks,
      taskCount: existing?.taskCount ?? 0,
      utcTrusted: existing?.utcTrusted ?? false,
      tzOffsetMin: existing?.tzOffsetMin,
      lastSeen: DateTime.now(),
      lastTimeSync: existing?.lastTimeSync,
      phase: existing?.phase ?? GatewayConnectionPhase.idle,
      features: existing?.features ?? const <String>{},
    );
    _publishDevices();
  }

  void _publishDevices({GatewayConnectionPhase? phase}) {
    final devices = _devices.values.toList()
      ..sort((a, b) => b.rssi.compareTo(a.rssi));
    _emit(
      _state.copyWith(
        devices: List<GatewayDevice>.unmodifiable(devices),
        phase: phase,
      ),
    );
  }

  @override
  Future<void> saveRemark(String deviceId, String remark) async {
    await _remarkStore.saveRemark(deviceId, remark.trim());
    _remarks[deviceId] = remark.trim();
    final device = _devices[deviceId];
    if (device != null) {
      _devices[deviceId] = device.copyWith(remark: remark.trim());
    }
    _publishDevices();
  }

  @override
  Future<void> connect(String deviceId) async {
    if (_connecting || _client.isConnected) return;
    final device = _devices[deviceId];
    if (device == null) {
      throw StateError('设备已离开扫描范围');
    }
    _userDisconnected = false;
    _reconnectGeneration++;
    await _connectDevice(device, reconnecting: false);
  }

  Future<void> _connectDevice(
    GatewayDevice device, {
    required bool reconnecting,
  }) async {
    if (_connecting) return;
    _connecting = true;
    var failed = false;
    await _scanSubscription?.cancel();
    _scanSubscription = null;
    try {
      _setDevicePhase(
        device.deviceId,
        reconnecting
            ? GatewayConnectionPhase.reconnecting
            : GatewayConnectionPhase.connecting,
      );
      final snapshot = await _client.connectAndInitialize(
        peripheralId: device.peripheralId,
        expectedDeviceId: device.deviceId,
        onPhase: (phase) => _setDevicePhase(device.deviceId, phase),
      );
      final current = _devices[device.deviceId] ?? device;
      _devices[device.deviceId] = current.copyWith(
        alias: snapshot.info.alias,
        firmware: snapshot.info.firmware,
        maxTasks: snapshot.info.maxTasks,
        taskCount: snapshot.tasks.length,
        utcTrusted: snapshot.info.utcTrusted,
        tzOffsetMin: snapshot.info.tzOffsetMin,
        lastTimeSync: snapshot.lastTimeSync,
        phase: GatewayConnectionPhase.ready,
        features: snapshot.info.features,
      );
      _lastTimeSync = snapshot.lastTimeSync;
      _lastTimezoneOffset = snapshot.info.tzOffsetMin;
      await _remarkStore.recordConnected(device.deviceId, DateTime.now());
      _emit(
        _state.copyWith(
          phase: GatewayConnectionPhase.ready,
          devices: List<GatewayDevice>.unmodifiable(_devices.values),
          tasks: snapshot.tasks,
          events: const <TimerFiredEvent>[],
          currentDeviceId: device.deviceId,
          clearError: true,
        ),
      );
    } on Object catch (error) {
      failed = true;
      if (!reconnecting) {
        _setDevicePhase(device.deviceId, GatewayConnectionPhase.error);
        _emit(
          _state.copyWith(
            errorMessage: _friendlyError(error),
            clearCurrentDevice: true,
          ),
        );
        rethrow;
      }
    } finally {
      _connecting = false;
      if (failed && !reconnecting && !_client.isConnected) {
        unawaited(startScanning());
      }
    }
  }

  void _setDevicePhase(String deviceId, GatewayConnectionPhase phase) {
    final device = _devices[deviceId];
    if (device != null) _devices[deviceId] = device.copyWith(phase: phase);
    _emit(
      _state.copyWith(
        phase: phase,
        devices: List<GatewayDevice>.unmodifiable(_devices.values),
        currentDeviceId: deviceId,
      ),
    );
  }

  void _onUnexpectedDisconnect() {
    if (_userDisconnected || _state.currentDeviceId == null) return;
    final targetId = _state.currentDeviceId!;
    _setDevicePhase(targetId, GatewayConnectionPhase.reconnecting);
    final generation = ++_reconnectGeneration;
    unawaited(_runReconnectLoop(targetId, generation));
  }

  Future<void> _runReconnectLoop(String deviceId, int generation) async {
    var attempt = 0;
    while (!_userDisconnected && generation == _reconnectGeneration) {
      final index = attempt.clamp(0, YxProtocol.reconnectBackoff.length - 1);
      await Future<void>.delayed(YxProtocol.reconnectBackoff[index]);
      if (_userDisconnected || generation != _reconnectGeneration) return;
      try {
        final scanned = await _scanner
            .scan()
            .firstWhere((device) => device.deviceId == deviceId)
            .timeout(const Duration(seconds: 8));
        _mergeScanResult(scanned);
        await _connectDevice(_devices[deviceId]!, reconnecting: true);
        if (_client.isConnected) return;
      } on Object {
        // 每轮都重新扫描稳定 device_id，临时 Peripheral 标识变化不会丢失目标。
      }
      attempt++;
    }
  }

  @override
  Future<void> disconnect() async {
    _userDisconnected = true;
    _reconnectGeneration++;
    final deviceId = _state.currentDeviceId;
    if (deviceId != null) {
      _setDevicePhase(deviceId, GatewayConnectionPhase.disconnecting);
    }
    await _client.disconnect();
    if (deviceId != null && _devices[deviceId] != null) {
      _devices[deviceId] = _devices[deviceId]!.copyWith(
        phase: GatewayConnectionPhase.idle,
      );
    }
    _emit(
      _state.copyWith(
        phase: GatewayConnectionPhase.idle,
        devices: List<GatewayDevice>.unmodifiable(_devices.values),
        tasks: const <TimerTaskSummary>[],
        identifyingDevice: false,
        clearCurrentDevice: true,
        clearError: true,
      ),
    );
    await startScanning();
  }

  @override
  Future<void> resyncTime({bool force = false}) async {
    if (!_client.isConnected) return;
    final now = DateTime.now();
    if (!force &&
        _lastTimeSync != null &&
        now.difference(_lastTimeSync!) < const Duration(seconds: 60)) {
      return;
    }
    final syncedAt = await _client.syncTime();
    _lastTimeSync = syncedAt;
    _lastTimezoneOffset = now.timeZoneOffset.inMinutes;
    final id = _state.currentDeviceId;
    if (id != null && _devices[id] != null) {
      _devices[id] = _devices[id]!.copyWith(
        utcTrusted: true,
        tzOffsetMin: _lastTimezoneOffset,
        lastTimeSync: syncedAt,
      );
      _publishDevices();
    }
  }

  @override
  Future<void> onAppResumed() async {
    if (!_client.isConnected ||
        _state.phase != GatewayConnectionPhase.ready ||
        _state.currentDeviceId == null) {
      if (_connecting || _client.isConnected) return;
      await startScanning();
      return;
    }
    final offsetChanged =
        DateTime.now().timeZoneOffset.inMinutes != _lastTimezoneOffset;
    try {
      await resyncTime(force: offsetChanged);
    } on Object catch (error) {
      _emit(
        _state.copyWith(errorMessage: '恢复前台后的时间同步失败：${_friendlyError(error)}'),
      );
    }
  }

  @override
  Future<void> onAppPaused() async {
    if (_client.isConnected) return;
    await _scanSubscription?.cancel();
    _scanSubscription = null;
    if (_state.phase == GatewayConnectionPhase.scanning) {
      _emit(_state.copyWith(phase: GatewayConnectionPhase.idle));
    }
  }

  @override
  Future<void> refreshTasks() async {
    if (!_client.isConnected) return;
    final tasks = await _client.listAllTasks();
    final id = _state.currentDeviceId;
    if (id != null && _devices[id] != null) {
      _devices[id] = _devices[id]!.copyWith(taskCount: tasks.length);
    }
    _emit(
      _state.copyWith(
        tasks: tasks,
        devices: List<GatewayDevice>.unmodifiable(_devices.values),
      ),
    );
  }

  @override
  Future<TimerTask> getTask(int taskId) => _client.getTask(taskId);

  @override
  Future<TimerTask> saveTask(TimerTask task) async {
    _validateTaskCapacity(task);
    final trusted = _state.currentDevice?.utcTrusted ?? false;
    final errors = task.validate(timeTrusted: trusted);
    if (errors.isNotEmpty) {
      throw ProtocolException(ProtocolErrorCode.invalidParam, errors.first);
    }
    try {
      String? idempotencyKey;
      final deviceId = _state.currentDeviceId;
      if (task.taskId == null) {
        if (deviceId == null) {
          throw const ProtocolException(
            ProtocolErrorCode.disconnected,
            '设备尚未完成安全连接',
          );
        }
        final taskJson = jsonEncode(task.toJson());
        final pending = await _remarkStore.getPendingTaskCreate(deviceId);
        if (pending != null && pending.taskJson != taskJson) {
          throw const ProtocolException(
            ProtocolErrorCode.idempotencyConflict,
            '上一笔新建任务结果尚未确认，请先重试原任务',
          );
        }
        idempotencyKey = pending?.idempotencyKey ?? createIdempotencyKey();
        if (pending == null) {
          await _remarkStore.savePendingTaskCreate(
            deviceId,
            PendingTaskCreate(
              idempotencyKey: idempotencyKey,
              taskJson: taskJson,
            ),
          );
        }
      }

      final saved = await _client.upsertTask(
        task,
        idempotencyKey: idempotencyKey,
      );
      if (task.taskId == null && deviceId != null) {
        await _remarkStore.clearPendingTaskCreate(deviceId);
      }
      // 任务写入设备后立即强制校时，避免长时间停留在详情页时的时钟漂移
      // 影响刚创建任务的首次触发边界。
      try {
        await resyncTime(force: true);
        await refreshTasks();
      } on Object catch (error) {
        /*
         * 任务已经由设备确认保存，此后校时或刷新失败不能把页面留在“未保存”
         * 状态，否则用户再次点击会用新键创建重复任务。连接恢复后会自动刷新。
         */
        _emit(
          _state.copyWith(
            errorMessage: '任务已保存，后续同步失败：${_friendlyError(error)}',
          ),
        );
      }
      return saved;
    } on ProtocolException catch (error) {
      if (error.isRevisionConflict && task.taskId != null) {
        await _refreshConflictedTask(task.taskId!);
      }
      rethrow;
    }
  }

  void _validateTaskCapacity(TimerTask task) {
    if (task.taskId == null && _state.tasks.length >= YxProtocol.maxTasks) {
      throw const ProtocolException(
        ProtocolErrorCode.limitReached,
        '设备最多保存 16 条任务',
      );
    }
  }

  @override
  Future<void> setTaskEnabled(TimerTaskSummary task, bool enabled) =>
      _withTaskBusy(task.taskId, () async {
        if (enabled && !(_state.currentDevice?.utcTrusted ?? false)) {
          throw const ProtocolException(
            ProtocolErrorCode.timeUntrusted,
            '等待时间同步后才能启用任务',
          );
        }
        try {
          await _client.setTaskEnabled(task, enabled);
          await refreshTasks();
        } on ProtocolException catch (error) {
          if (error.isRevisionConflict) {
            await _refreshConflictedTask(task.taskId);
          }
          rethrow;
        }
      });

  @override
  Future<void> deleteTask(TimerTaskSummary task) =>
      _withTaskBusy(task.taskId, () async {
        try {
          await _client.deleteTask(task);
          await refreshTasks();
        } on ProtocolException catch (error) {
          if (error.isRevisionConflict) {
            await _refreshConflictedTask(task.taskId);
          }
          rethrow;
        }
      });

  Future<void> _withTaskBusy(int taskId, Future<void> Function() action) async {
    if (_state.busyTaskIds.contains(taskId)) return;
    _emit(_state.copyWith(busyTaskIds: <int>{..._state.busyTaskIds, taskId}));
    try {
      await action();
    } finally {
      _emit(
        _state.copyWith(
          busyTaskIds: <int>{..._state.busyTaskIds}..remove(taskId),
        ),
      );
    }
  }

  Future<void> _refreshConflictedTask(int taskId) async {
    final latest = await _client.getTask(taskId);
    await refreshTasks();
    final summary = _state.tasks
        .where((item) => item.taskId == taskId)
        .firstOrNull;
    if (summary == null && latest.taskId != null) await refreshTasks();
  }

  @override
  Future<void> setAlias(String alias) async {
    final saved = await _client.setAlias(alias.trim());
    final id = _state.currentDeviceId;
    if (id != null && _devices[id] != null) {
      _devices[id] = _devices[id]!.copyWith(alias: saved);
      _publishDevices();
    }
  }

  @override
  Future<DeviceIdentifyResult> identifyDevice() async {
    final device = _state.currentDevice;
    if (device == null ||
        device.phase != GatewayConnectionPhase.ready ||
        !_client.isConnected) {
      throw const ProtocolException(
        ProtocolErrorCode.disconnected,
        '设备尚未完成安全连接',
      );
    }
    if (!device.supportsIdentify) {
      throw const ProtocolException(
        ProtocolErrorCode.unknownOp,
        '当前网关固件不支持闪灯找设备',
      );
    }
    if (_state.identifyingDevice) {
      throw const ProtocolException(
        ProtocolErrorCode.invalidParam,
        '识别灯请求正在执行，请稍候',
      );
    }

    final startedAt = DateTime.now();
    _emit(_state.copyWith(identifyingDevice: true));
    try {
      return await _client.identifyDevice();
    } finally {
      final elapsed = DateTime.now().difference(startedAt);
      const minimumBusyTime = Duration(milliseconds: 500);
      if (elapsed < minimumBusyTime) {
        await Future<void>.delayed(minimumBusyTime - elapsed);
      }
      _emit(_state.copyWith(identifyingDevice: false));
    }
  }

  GatewayDevice _requireLightTestDevice() {
    final device = _state.currentDevice;
    if (device == null ||
        device.phase != GatewayConnectionPhase.ready ||
        !_client.isConnected) {
      throw const ProtocolException(
        ProtocolErrorCode.disconnected,
        '设备尚未完成安全连接',
      );
    }
    if (!device.supportsLightTest) {
      throw const ProtocolException(
        ProtocolErrorCode.unknownOp,
        '当前网关固件不支持灯具控制测试',
      );
    }
    if (_state.lampCommandBusy) {
      throw const ProtocolException(
        ProtocolErrorCode.invalidParam,
        '灯具测试指令正在发送，请稍候',
      );
    }
    return device;
  }

  @override
  Future<LampTestResult> testLamp(
    LampTestAction action,
    LampTarget target,
  ) async {
    final errors = target.validate();
    if (errors.isNotEmpty) {
      throw ProtocolException(ProtocolErrorCode.invalidParam, errors.first);
    }
    _requireLightTestDevice();
    _emit(_state.copyWith(lampCommandBusy: true));
    try {
      return await _client.testLamp(action, target);
    } finally {
      _emit(_state.copyWith(lampCommandBusy: false));
    }
  }

  GatewayDevice _requireSceneTestDevice() {
    final device = _state.currentDevice;
    if (device == null ||
        device.phase != GatewayConnectionPhase.ready ||
        !_client.isConnected) {
      throw const ProtocolException(
        ProtocolErrorCode.disconnected,
        '设备尚未完成安全连接',
      );
    }
    if (!device.supportsSceneTest) {
      throw const ProtocolException(
        ProtocolErrorCode.unknownOp,
        '当前网关固件不支持场景号测试',
      );
    }
    if (_state.sceneCommandBusy) {
      throw const ProtocolException(
        ProtocolErrorCode.invalidParam,
        '场景号指令正在执行，请稍候',
      );
    }
    return device;
  }

  Future<SceneNumberResult> _runSceneCommand(
    Future<SceneNumberResult> Function() command,
  ) async {
    _requireSceneTestDevice();
    _emit(_state.copyWith(sceneCommandBusy: true));
    try {
      return await command();
    } finally {
      _emit(_state.copyWith(sceneCommandBusy: false));
    }
  }

  @override
  Future<SceneNumberResult> setCurrentScene(int sceneNo, {LampTarget? target}) {
    if (sceneNo < 1 || sceneNo > 16) {
      throw const ProtocolException(
        ProtocolErrorCode.invalidParam,
        '场景号必须在 1～16 之间',
      );
    }
    if (target != null) {
      final errors = target.validate();
      if (errors.isNotEmpty) {
        throw ProtocolException(ProtocolErrorCode.invalidParam, errors.first);
      }
    }
    return _runSceneCommand(
      () => _client.setCurrentScene(sceneNo, target: target),
    );
  }

  @override
  Future<SceneNumberResult> readCurrentScene({LampTarget? target}) {
    if (target != null) {
      final errors = target.validate();
      if (errors.isNotEmpty) {
        throw ProtocolException(ProtocolErrorCode.invalidParam, errors.first);
      }
    }
    return _runSceneCommand(() => _client.readCurrentScene(target: target));
  }

  void _onTimerEvent(TimerFiredEvent event) {
    final events = <TimerFiredEvent>[event, ..._state.events].take(20).toList();
    _emit(_state.copyWith(events: List<TimerFiredEvent>.unmodifiable(events)));
    unawaited(refreshTasks());
  }

  @override
  Future<void> openSettings() => _scanner.openSettings();

  String _friendlyError(Object error) {
    if (error is ProtocolException) {
      if (error.code == ProtocolErrorCode.revisionConflict) {
        return '设备配置已变化，请重新确认';
      }
      return error.message;
    }
    return error.toString();
  }

  @override
  Future<void> dispose() async {
    _userDisconnected = true;
    _reconnectGeneration++;
    await _scanSubscription?.cancel();
    await _hostStateSubscription?.cancel();
    await _disconnectSubscription?.cancel();
    await _eventSubscription?.cancel();
    await _client.dispose();
    await _remarkStore.close();
    await _stateController.close();
  }
}
