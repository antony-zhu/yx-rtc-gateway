import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:yx_gateway_timer/data/ble/yx_gatt_client.dart';
import 'package:yx_gateway_timer/data/repositories/gateway_repository.dart';
import 'package:yx_gateway_timer/domain/gateway_device.dart';
import 'package:yx_gateway_timer/domain/protocol_error.dart';
import 'package:yx_gateway_timer/domain/timer_task.dart';
import 'package:yx_gateway_timer/presentation/device_scan/device_scan_page.dart';
import 'package:yx_gateway_timer/presentation/providers.dart';

void main() {
  testWidgets('保存本地备注时安全关闭对话框并刷新设备名', (tester) async {
    final repository = _FakeGatewayRepository();
    await tester.pumpWidget(
      ProviderScope(
        overrides: [gatewayRepositoryProvider.overrideWithValue(repository)],
        child: const MaterialApp(home: DeviceScanPage()),
      ),
    );
    await tester.pump();

    await tester.tap(find.byTooltip('编辑本地备注'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byType(TextFormField), 'PhoneTest');
    await tester.tap(find.text('保存'));
    await tester.pumpAndSettle();

    expect(tester.takeException(), isNull);
    expect(find.text('PhoneTest'), findsOneWidget);
    expect(repository.state.devices.single.remark, 'PhoneTest');

    await repository.dispose();
  });

  testWidgets('旧固件未声明 device_identify 时隐藏闪灯入口', (tester) async {
    final repository = _FakeGatewayRepository(connected: true);
    await tester.pumpWidget(
      ProviderScope(
        overrides: [gatewayRepositoryProvider.overrideWithValue(repository)],
        child: const MaterialApp(home: DeviceScanPage()),
      ),
    );
    await tester.pump();

    expect(find.byTooltip('闪灯找设备'), findsNothing);
    await repository.dispose();
  });

  testWidgets('设备声明能力后发送识别命令并只在成功后提示', (tester) async {
    final repository = _FakeGatewayRepository(
      connected: true,
      supportsIdentify: true,
    );
    await tester.pumpWidget(
      ProviderScope(
        overrides: [gatewayRepositoryProvider.overrideWithValue(repository)],
        child: const MaterialApp(home: DeviceScanPage()),
      ),
    );
    await tester.pump();

    await tester.tap(find.byTooltip('闪灯找设备'));
    await tester.pumpAndSettle();

    expect(repository.identifyCount, 1);
    expect(find.textContaining('板载识别灯已闪烁'), findsOneWidget);
    await repository.dispose();
  });

  testWidgets('设备声明能力后可从详情操作面板闪灯', (tester) async {
    final repository = _FakeGatewayRepository(
      connected: true,
      supportsIdentify: true,
    );
    await tester.pumpWidget(
      ProviderScope(
        overrides: [gatewayRepositoryProvider.overrideWithValue(repository)],
        child: const MaterialApp(home: DeviceScanPage()),
      ),
    );
    await tester.pump();

    expect(find.byTooltip('闪灯找设备'), findsOneWidget);
    await tester.tap(find.text('查看'));
    await tester.pumpAndSettle();

    expect(find.text('定时任务'), findsOneWidget);
    expect(find.byTooltip('设备信息与操作'), findsOneWidget);
    await tester.tap(find.byTooltip('设备信息与操作'));
    await tester.pumpAndSettle();
    expect(find.text('闪灯找设备'), findsOneWidget);
    await repository.dispose();
  });

  testWidgets('识别命令失败时显示错误且不显示成功提示', (tester) async {
    final repository = _FakeGatewayRepository(
      connected: true,
      supportsIdentify: true,
      identifyError: const ProtocolException(
        ProtocolErrorCode.internalError,
        '板载指示灯执行失败',
      ),
    );
    await tester.pumpWidget(
      ProviderScope(
        overrides: [gatewayRepositoryProvider.overrideWithValue(repository)],
        child: const MaterialApp(home: DeviceScanPage()),
      ),
    );
    await tester.pump();

    await tester.tap(find.byTooltip('闪灯找设备'));
    await tester.pumpAndSettle();

    expect(repository.identifyCount, 1);
    expect(find.text('板载指示灯执行失败'), findsOneWidget);
    expect(find.textContaining('已触发识别灯'), findsNothing);
    await repository.dispose();
  });

  testWidgets('设备详情用紧凑任务行直接提供删除入口', (tester) async {
    final repository = _FakeGatewayRepository(connected: true, withTask: true);
    await tester.pumpWidget(
      ProviderScope(
        overrides: [gatewayRepositoryProvider.overrideWithValue(repository)],
        child: const MaterialApp(home: DeviceScanPage()),
      ),
    );
    await tester.pump();

    await tester.tap(find.text('查看'));
    await tester.pumpAndSettle();
    expect(find.text('任务'), findsOneWidget);
    expect(find.text('记录'), findsOneWidget);
    expect(find.byTooltip('删除任务'), findsOneWidget);

    await tester.tap(find.byTooltip('删除任务'));
    await tester.pumpAndSettle();
    expect(find.text('删除任务？'), findsOneWidget);
    await tester.tap(find.widgetWithText(FilledButton, '删除任务'));
    await tester.pumpAndSettle();

    expect(repository.deleteCount, 1);
    expect(find.text('界面删除测试'), findsNothing);
    await repository.dispose();
  });

  testWidgets('空任务页只显示一个新建任务入口', (tester) async {
    final repository = _FakeGatewayRepository(connected: true);
    await tester.pumpWidget(
      ProviderScope(
        overrides: [gatewayRepositoryProvider.overrideWithValue(repository)],
        child: const MaterialApp(home: DeviceScanPage()),
      ),
    );
    await tester.pump();

    await tester.tap(find.text('查看'));
    await tester.pumpAndSettle();

    expect(find.text('还没有定时任务'), findsOneWidget);
    expect(find.text('新建任务'), findsOneWidget);
    await repository.dispose();
  });

  testWidgets('设备测试面板发送闪灯、设置场景和读取场景指令', (tester) async {
    final repository = _FakeGatewayRepository(
      connected: true,
      supportsIdentify: true,
      supportsScene: true,
    );
    await tester.pumpWidget(
      ProviderScope(
        overrides: [gatewayRepositoryProvider.overrideWithValue(repository)],
        child: const MaterialApp(home: DeviceScanPage()),
      ),
    );
    await tester.pump();

    await tester.tap(find.text('查看'));
    await tester.pumpAndSettle();
    await tester.tap(find.byTooltip('设备信息与操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('设备测试'));
    await tester.pumpAndSettle();

    expect(find.text('闪板载指示灯'), findsOneWidget);
    expect(find.text('读取场景'), findsOneWidget);
    expect(find.text('设置场景'), findsOneWidget);

    await tester.tap(find.text('闪板载指示灯'));
    await tester.pumpAndSettle();
    expect(repository.identifyCount, 1);

    await tester.tap(find.text('读取场景'));
    await tester.pumpAndSettle();
    expect(repository.readSceneCount, 1);
    expect(find.text('7'), findsWidgets);

    await tester.enterText(find.byType(TextField), '6');
    await tester.tap(find.text('设置场景'));
    await tester.pumpAndSettle();
    expect(repository.setSceneNumbers, <int>[6]);
    expect(find.textContaining('请读取确认'), findsOneWidget);
    await repository.dispose();
  });

  testWidgets('灯具控制测试页发送亮、灭、闪和区号目标', (tester) async {
    final repository = _FakeGatewayRepository(
      connected: true,
      supportsLight: true,
    );
    await tester.pumpWidget(
      ProviderScope(
        overrides: [gatewayRepositoryProvider.overrideWithValue(repository)],
        child: const MaterialApp(home: DeviceScanPage()),
      ),
    );
    await tester.pump();

    await tester.tap(find.text('查看'));
    await tester.pumpAndSettle();
    await tester.tap(find.byTooltip('设备信息与操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('设备测试'));
    await tester.pumpAndSettle();

    expect(find.text('灯具控制测试'), findsOneWidget);
    final fields = find.byType(TextField);
    expect(fields, findsNWidgets(2));
    expect(tester.widget<TextField>(fields.at(0)).controller?.text, '0');
    expect(tester.widget<TextField>(fields.at(1)).controller?.text, '0');
    await tester.enterText(fields.at(0), '124');
    await tester.enterText(fields.at(1), '1');
    await tester.tap(find.text('号'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('标签').last);
    await tester.pumpAndSettle();

    for (final label in <String>['亮灯', '灭灯', '闪一下']) {
      await tester.ensureVisible(find.text(label));
      await tester.tap(find.text(label));
      await tester.pumpAndSettle();
    }

    expect(repository.lampTests.map((item) => item.action), <LampTestAction>[
      LampTestAction.lightOn,
      LampTestAction.lightOff,
      LampTestAction.flash,
    ]);
    expect(repository.lampTests.last.target.networkId, 124);
    expect(repository.lampTests.last.target.type, LampTargetType.virtualNumber);
    expect(repository.lampTests.last.target.value, 1);
    expect(find.textContaining('指令已写入网关'), findsOneWidget);
    await repository.dispose();
  });
}

class _FakeGatewayRepository implements GatewayRepository {
  _FakeGatewayRepository({
    bool connected = false,
    bool supportsIdentify = false,
    bool supportsLight = false,
    bool supportsScene = false,
    bool withTask = false,
    this.identifyError,
  }) : _state = GatewayRepositoryState(
         initialized: true,
         phase: connected
             ? GatewayConnectionPhase.ready
             : GatewayConnectionPhase.idle,
         currentDeviceId: connected ? '80B54EE92138' : null,
         tasks: withTask
             ? const <TimerTaskSummary>[
                 TimerTaskSummary(
                   taskId: 1,
                   revision: 1,
                   enabled: true,
                   name: '界面删除测试',
                   type: TimerType.interval,
                 ),
               ]
             : const <TimerTaskSummary>[],
         devices: <GatewayDevice>[
           GatewayDevice(
             deviceId: '80B54EE92138',
             peripheralId: 'test-peripheral',
             rssi: -48,
             advertisedName: 'YXGW-80B54EE92138',
             phase: connected
                 ? GatewayConnectionPhase.ready
                 : GatewayConnectionPhase.idle,
             features: <String>{
               if (supportsIdentify) 'device_identify',
               if (supportsLight) 'gs200_light_test',
               if (supportsScene) 'gs200_scene_test',
             },
           ),
         ],
       );

  final Object? identifyError;
  int identifyCount = 0;
  final List<LampTestResult> lampTests = <LampTestResult>[];
  int readSceneCount = 0;
  final List<int> setSceneNumbers = <int>[];
  int deleteCount = 0;
  final StreamController<GatewayRepositoryState> _states =
      StreamController<GatewayRepositoryState>.broadcast();
  GatewayRepositoryState _state;

  @override
  GatewayRepositoryState get state => _state;

  @override
  Stream<GatewayRepositoryState> get states => _states.stream;

  @override
  Future<void> saveRemark(String deviceId, String remark) async {
    _state = _state.copyWith(
      devices: _state.devices
          .map(
            (device) => device.deviceId == deviceId
                ? device.copyWith(remark: remark.trim())
                : device,
          )
          .toList(growable: false),
    );
    _states.add(_state);
  }

  @override
  Future<void> initialize() async {}

  @override
  Future<DeviceIdentifyResult> identifyDevice() async {
    identifyCount++;
    if (identifyError case final error?) throw error;
    return const DeviceIdentifyResult(ledIndex: 2, gpio: 42, durationMs: 350);
  }

  @override
  Future<LampTestResult> testLamp(
    LampTestAction action,
    LampTarget target,
  ) async {
    final result = LampTestResult(action: action, target: target);
    lampTests.add(result);
    return result;
  }

  @override
  Future<SceneNumberResult> setCurrentScene(
    int sceneNo, {
    LampTarget? target,
  }) async {
    setSceneNumbers.add(sceneNo);
    return SceneNumberResult(sceneNo: sceneNo);
  }

  @override
  Future<SceneNumberResult> readCurrentScene({LampTarget? target}) async {
    readSceneCount++;
    return const SceneNumberResult(sceneNo: 7);
  }

  @override
  Future<void> startScanning() async {}

  @override
  Future<void> openSettings() async {}

  @override
  Future<void> connect(String deviceId) async {}

  @override
  Future<void> disconnect() async {}

  @override
  Future<void> resyncTime({bool force = false}) async {}

  @override
  Future<void> refreshTasks() async {}

  @override
  Future<TimerTask> getTask(int taskId) async => throw UnimplementedError();

  @override
  Future<TimerTask> saveTask(TimerTask task) async => task;

  @override
  Future<void> setTaskEnabled(TimerTaskSummary task, bool enabled) async {}

  @override
  Future<void> deleteTask(TimerTaskSummary task) async {
    deleteCount++;
    _state = _state.copyWith(
      tasks: _state.tasks
          .where((item) => item.taskId != task.taskId)
          .toList(growable: false),
    );
    _states.add(_state);
  }

  @override
  Future<void> setAlias(String alias) async {}

  @override
  Future<void> onAppResumed() async {}

  @override
  Future<void> onAppPaused() async {}

  @override
  Future<void> dispose() => _states.close();
}
