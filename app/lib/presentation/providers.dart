import 'dart:async';

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:yx_gateway_timer/data/ble/ble_scanner.dart';
import 'package:yx_gateway_timer/data/ble/yx_gatt_client.dart';
import 'package:yx_gateway_timer/data/local/device_remark_store.dart';
import 'package:yx_gateway_timer/data/repositories/gateway_repository.dart';
import 'package:yx_gateway_timer/domain/timer_task.dart';

final flutterReactiveBleProvider = Provider<FlutterReactiveBle>(
  (ref) => FlutterReactiveBle(),
);

final bleScannerProvider = Provider<BleScanner>(
  (ref) => ReactiveBleScanner(ref.watch(flutterReactiveBleProvider)),
);

final yxGattClientProvider = Provider<YxGattClient>(
  (ref) => ReactiveYxGattClient(ref.watch(flutterReactiveBleProvider)),
);

final deviceRemarkStoreProvider = Provider<DeviceRemarkStore>(
  (ref) => SqfliteDeviceRemarkStore(),
);

final gatewayRepositoryProvider = Provider<GatewayRepository>((ref) {
  final repository = GatewayRepositoryImpl(
    scanner: ref.watch(bleScannerProvider),
    client: ref.watch(yxGattClientProvider),
    remarkStore: ref.watch(deviceRemarkStoreProvider),
  );
  ref.onDispose(() => unawaited(repository.dispose()));
  return repository;
});

final gatewayControllerProvider =
    NotifierProvider<GatewayController, GatewayRepositoryState>(
      GatewayController.new,
    );

class GatewayController extends Notifier<GatewayRepositoryState> {
  late GatewayRepository _repository;
  StreamSubscription<GatewayRepositoryState>? _subscription;

  @override
  GatewayRepositoryState build() {
    _repository = ref.watch(gatewayRepositoryProvider);
    _subscription = _repository.states.listen((value) => state = value);
    ref.onDispose(() => unawaited(_subscription?.cancel()));
    unawaited(_initialize());
    return _repository.state;
  }

  Future<void> _initialize() async {
    try {
      await _repository.initialize();
    } on Object {
      // Repository 会把可恢复错误映射到页面状态；初始化异常由页面重试入口处理。
    }
  }

  Future<void> scan() => _repository.startScanning();
  Future<void> openSettings() => _repository.openSettings();
  Future<void> saveRemark(String deviceId, String value) =>
      _repository.saveRemark(deviceId, value);
  Future<void> connect(String deviceId) => _repository.connect(deviceId);
  Future<void> disconnect() => _repository.disconnect();
  Future<void> syncTime() => _repository.resyncTime(force: true);
  Future<void> onAppResumed() => _repository.onAppResumed();
  Future<void> onAppPaused() => _repository.onAppPaused();
  Future<TimerTask> getTask(int taskId) => _repository.getTask(taskId);
  Future<TimerTask> saveTask(TimerTask task) => _repository.saveTask(task);
  Future<void> setEnabled(TimerTaskSummary task, bool enabled) =>
      _repository.setTaskEnabled(task, enabled);
  Future<void> deleteTask(TimerTaskSummary task) =>
      _repository.deleteTask(task);
  Future<void> setAlias(String alias) => _repository.setAlias(alias);
  Future<DeviceIdentifyResult> identifyDevice() => _repository.identifyDevice();
  Future<LampTestResult> testLamp(LampTestAction action, LampTarget target) =>
      _repository.testLamp(action, target);
  Future<SceneNumberResult> setCurrentScene(
    int sceneNo, {
    LampTarget? target,
  }) => _repository.setCurrentScene(sceneNo, target: target);
  Future<SceneNumberResult> readCurrentScene({LampTarget? target}) =>
      _repository.readCurrentScene(target: target);
}
