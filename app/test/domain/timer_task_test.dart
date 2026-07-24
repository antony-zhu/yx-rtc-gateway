import 'package:flutter_test/flutter_test.dart';
import 'package:yx_gateway_timer/domain/timer_task.dart';

void main() {
  group('TimerTask validation', () {
    test('限制名称 UTF-8 字节和 52 周上限', () {
      final task = TimerTask(
        enabled: true,
        name: '中文名称超过三十二字节中文名称',
        type: TimerType.duration,
        schedule: const DurationSchedule(value: 53, unit: DurationUnit.week),
        action: const TimerAction(type: TimerActionType.lightOn),
      );
      final errors = task.validate(timeTrusted: true);
      expect(errors, contains('任务名称最多 32 个 UTF-8 字节'));
      expect(errors, contains('倒计时范围为 1 分钟～52 周'));
    });

    test('时间不可信时禁止启用', () {
      final task = TimerTask(
        enabled: true,
        name: '早班',
        type: TimerType.daily,
        schedule: const DailySchedule(hour: 7, minute: 30),
        action: const TimerAction(type: TimerActionType.lightOff),
      );
      expect(task.validate(timeTrusted: false), contains('等待时间同步后才能启用任务'));
    });

    test('亮灯和灭灯动作发送结构化类型与灯具目标', () {
      const lightOn = TimerAction(type: TimerActionType.lightOn);
      const lightOff = TimerAction(type: TimerActionType.lightOff);

      const target = <String, Object>{
        'network_id': 0,
        'type': 'number',
        'value': 0x50bb,
      };
      expect(lightOn.toJson(), <String, Object>{
        'type': 'light_on',
        'target': target,
      });
      expect(lightOff.toJson(), <String, Object>{
        'type': 'light_off',
        'target': target,
      });
      expect(
        TimerAction.fromJson(lightOn.toJson()).type,
        TimerActionType.lightOn,
      );
      expect(
        TimerAction.fromJson(lightOff.toJson()).type,
        TimerActionType.lightOff,
      );
    });

    test('场景动作只允许 1 到 16 的场景号', () {
      const action = TimerAction(
        type: TimerActionType.sceneSwitch,
        sceneNo: 12,
      );
      expect(action.toJson(), <String, Object>{
        'type': 'scene_switch',
        'target': <String, Object>{
          'network_id': 0,
          'type': 'number',
          'value': 0x50bb,
        },
        'scene_no': 12,
      });
      expect(TimerAction.fromJson(action.toJson()).sceneNo, 12);

      for (final invalid in <int?>[null, 0, 17]) {
        final task = TimerTask(
          enabled: true,
          name: '场景测试',
          type: TimerType.interval,
          schedule: const IntervalSchedule(unit: DurationUnit.minute),
          action: TimerAction(
            type: TimerActionType.sceneSwitch,
            sceneNo: invalid,
          ),
        );
        expect(task.validate(timeTrusted: true), contains('场景号必须为 1～16'));
      }
    });

    test('每 N 分钟和每 N 小时循环任务可编解码结构化灯光动作', () {
      const task = TimerTask(
        enabled: true,
        name: '每五分钟亮灯',
        type: TimerType.interval,
        schedule: IntervalSchedule(value: 5, unit: DurationUnit.minute),
        action: TimerAction(type: TimerActionType.lightOn),
      );

      final json = task.toJson();
      expect(json['type'], 'interval');
      expect(json['schedule'], <String, Object>{'value': 5, 'unit': 'minute'});
      expect(json['action'], <String, Object>{
        'type': 'light_on',
        'target': <String, Object>{
          'network_id': 0,
          'type': 'number',
          'value': 0x50bb,
        },
      });
      expect(task.validate(timeTrusted: true), isEmpty);

      final decoded = TimerTask.fromJson(<String, Object?>{
        'task_id': 7,
        'revision': 2,
        ...json,
      });
      expect(decoded.type, TimerType.interval);
      expect((decoded.schedule as IntervalSchedule).unit, DurationUnit.minute);
      expect((decoded.schedule as IntervalSchedule).value, 5);
      expect(decoded.action.type, TimerActionType.lightOn);
    });

    test('区、号、标签、组和整区目标可编解码并校验', () {
      const targets = <LampTarget>[
        LampTarget(networkId: 124, type: LampTargetType.number, value: 23),
        LampTarget(
          networkId: 124,
          type: LampTargetType.virtualNumber,
          value: 1,
        ),
        LampTarget(networkId: 124, type: LampTargetType.group, value: 1),
        LampTarget(networkId: 124, type: LampTargetType.area),
      ];
      for (final target in targets) {
        expect(target.validate(), isEmpty);
        expect(LampTarget.fromJson(target.toJson()).toJson(), target.toJson());
      }
      expect(targets[0].destinationAddress, 23);
      expect(targets[1].destinationAddress, 0x8001);
      expect(targets[2].destinationAddress, 0xc001);
      expect(targets[3].destinationAddress, 0xffff);
    });

    test('循环任务拒绝天和周单位', () {
      const task = TimerTask(
        enabled: true,
        name: '非法循环',
        type: TimerType.interval,
        schedule: IntervalSchedule(unit: DurationUnit.day),
        action: TimerAction(type: TimerActionType.lightOff),
      );
      expect(task.validate(timeTrusted: true), contains('循环任务只支持每分钟或每小时'));
    });
  });
}
