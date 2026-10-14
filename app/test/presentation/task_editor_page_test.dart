import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:yx_gateway_timer/domain/timer_task.dart';
import 'package:yx_gateway_timer/presentation/task_editor/task_editor_page.dart';

void main() {
  testWidgets('新建任务不显示倒计时并要求配置灯具目标', (tester) async {
    await tester.pumpWidget(
      const ProviderScope(
        child: MaterialApp(
          home: TaskEditorPage(
            initialTask: TimerTask(
              enabled: false,
              name: '测试任务',
              type: TimerType.daily,
              schedule: DailySchedule(hour: 7, minute: 30),
              action: TimerAction(type: TimerActionType.lightOn),
            ),
            timeTrusted: true,
            supportedFeatures: <String>{
              'gs200_light',
              'interval',
              'gs200_light_test',
              'gs200_scene_test',
              'gs200_task_target',
            },
          ),
        ),
      ),
    );
    expect(find.text('编辑任务'), findsNothing);
    expect(find.text('新建任务'), findsOneWidget);
    expect(find.text('测试任务'), findsOneWidget);
    expect(find.text('亮灯'), findsWidgets);
    expect(find.text('控制灯具'), findsOneWidget);
    expect(find.text('快速测试'), findsOneWidget);
    expect(find.text('灭灯'), findsWidgets);
    expect(find.text('闪一下'), findsOneWidget);
    expect(find.text('读取场景'), findsOneWidget);
    expect(find.text('设置场景'), findsOneWidget);
    expect(find.byKey(const ValueKey('task_test_light_on')), findsOneWidget);
    expect(find.byKey(const ValueKey('task_test_light_off')), findsOneWidget);
    expect(find.byKey(const ValueKey('task_test_flash')), findsOneWidget);
    expect(find.byKey(const ValueKey('task_test_scene_read')), findsOneWidget);
    expect(find.byKey(const ValueKey('task_test_scene_set')), findsOneWidget);
    expect(find.text('区'), findsOneWidget);
    expect(find.text('号'), findsOneWidget);
    expect(find.text('整区'), findsNothing);
    expect(
      find.byType(DropdownButtonFormField<LampTargetType>),
      findsOneWidget,
    );

    await tester.tap(find.byType(DropdownButtonFormField<LampTargetType>));
    await tester.pumpAndSettle();
    expect(find.text('组'), findsOneWidget);
    expect(find.text('标签'), findsOneWidget);
    await tester.tap(find.text('标签'));
    await tester.pumpAndSettle();
    expect(
      tester
          .widgetList<TextField>(find.byType(TextField))
          .any((field) => field.controller?.text == '1'),
      isTrue,
    );

    await tester.dragUntilVisible(
      find.text('每天几点'),
      find.byType(ListView),
      const Offset(0, -240),
    );
    await tester.pumpAndSettle();
    expect(find.text('倒计时'), findsNothing);
    expect(find.text('每天'), findsOneWidget);
    expect(find.text('循环'), findsOneWidget);
    expect(find.byType(ListWheelScrollView), findsNWidgets(2));
  });

  testWidgets('循环和结构化灯光选项直接在页面展示', (tester) async {
    await tester.pumpWidget(
      const ProviderScope(
        child: MaterialApp(
          home: TaskEditorPage(
            initialTask: TimerTask(
              enabled: true,
              name: '循环场景测试',
              type: TimerType.interval,
              schedule: IntervalSchedule(unit: DurationUnit.minute),
              action: TimerAction(
                type: TimerActionType.sceneSwitch,
                sceneNo: 2,
              ),
            ),
            timeTrusted: true,
            supportedFeatures: <String>{
              'interval',
              'gs200_light',
              'gs200_scene',
            },
          ),
        ),
      ),
    );

    expect(find.text('亮灯'), findsOneWidget);
    expect(find.text('灭灯'), findsOneWidget);
    expect(find.text('设置场景'), findsOneWidget);
    expect(find.text('设备内已保存的编号'), findsNothing);
    expect(find.textContaining('十六进制'), findsNothing);

    await tester.dragUntilVisible(
      find.text('多久一次'),
      find.byType(ListView),
      const Offset(0, -240),
    );
    expect(find.text('循环'), findsOneWidget);
    await tester.drag(find.byType(ListView), const Offset(0, -260));
    await tester.pumpAndSettle();
    expect(find.text('循环间隔'), findsOneWidget);
    expect(find.text('分钟'), findsOneWidget);
    expect(find.text('小时'), findsOneWidget);

    await tester.dragUntilVisible(
      find.text('场景号'),
      find.byType(ListView),
      const Offset(0, 240),
    );
    await tester.pumpAndSettle();
    expect(find.text('场景号'), findsOneWidget);
    expect(
      tester
          .widgetList<TextField>(find.byType(TextField))
          .any((field) => field.controller?.text == '2'),
      isTrue,
    );
  });

  testWidgets('编辑任务时保存固定且删除入口清晰可见', (tester) async {
    var deleted = false;
    await tester.pumpWidget(
      ProviderScope(
        child: MaterialApp(
          home: Builder(
            builder: (context) => Scaffold(
              body: FilledButton(
                onPressed: () => Navigator.of(context).push<void>(
                  MaterialPageRoute<void>(
                    builder: (_) => TaskEditorPage(
                      initialTask: const TimerTask(
                        taskId: 7,
                        revision: 3,
                        enabled: true,
                        name: '可删除任务',
                        type: TimerType.interval,
                        schedule: IntervalSchedule(unit: DurationUnit.minute),
                        action: TimerAction(type: TimerActionType.lightOff),
                      ),
                      timeTrusted: true,
                      supportedFeatures: const <String>{
                        'interval',
                        'gs200_light',
                      },
                      onDelete: () async => deleted = true,
                    ),
                  ),
                ),
                child: const Text('打开编辑器'),
              ),
            ),
          ),
        ),
      ),
    );

    await tester.tap(find.text('打开编辑器'));
    await tester.pumpAndSettle();
    await tester.dragUntilVisible(
      find.text('保存并同步'),
      find.byType(ListView),
      const Offset(0, -260),
    );
    expect(find.text('保存并同步'), findsOneWidget);
    await tester.dragUntilVisible(
      find.text('删除任务'),
      find.byType(ListView),
      const Offset(0, -180),
    );
    expect(find.text('删除任务'), findsOneWidget);

    await tester.tap(find.text('删除任务'));
    await tester.pumpAndSettle();
    expect(find.text('删除这个任务？'), findsOneWidget);
    await tester.tap(find.widgetWithText(FilledButton, '删除任务'));
    await tester.pumpAndSettle();

    expect(deleted, isTrue);
    expect(find.text('打开编辑器'), findsOneWidget);
  });
}
