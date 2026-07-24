import 'dart:convert';

import 'package:yx_gateway_timer/domain/protocol_error.dart';

enum TimerType {
  duration('duration'),
  daily('daily'),
  interval('interval');

  const TimerType(this.wireName);
  final String wireName;

  static TimerType parse(Object? value) => values.firstWhere(
    (item) => item.wireName == value,
    orElse: () => throw const ProtocolException(
      ProtocolErrorCode.malformedResponse,
      '未知的任务类型',
    ),
  );
}

enum DurationUnit {
  minute('minute', '分钟', 1),
  hour('hour', '小时', 60),
  day('day', '天', 1440),
  week('week', '周', 10080);

  const DurationUnit(this.wireName, this.label, this.minutes);
  final String wireName;
  final String label;
  final int minutes;

  static DurationUnit parse(Object? value) => values.firstWhere(
    (item) => item.wireName == value,
    orElse: () => throw const ProtocolException(
      ProtocolErrorCode.malformedResponse,
      '未知的倒计时单位',
    ),
  );
}

enum TimerActionType {
  lightOn('light_on'),
  lightOff('light_off'),
  sceneSwitch('scene_switch');

  const TimerActionType(this.wireName);
  final String wireName;

  static TimerActionType parse(Object? value) => values.firstWhere(
    (item) => item.wireName == value,
    orElse: () => throw const ProtocolException(
      ProtocolErrorCode.malformedResponse,
      '未知的动作类型',
    ),
  );
}

enum LampTestAction {
  lightOn('light_on', '亮灯'),
  lightOff('light_off', '灭灯'),
  flash('flash', '闪一下');

  const LampTestAction(this.wireName, this.label);
  final String wireName;
  final String label;

  static LampTestAction parse(Object? value) => values.firstWhere(
    (item) => item.wireName == value,
    orElse: () => throw const ProtocolException(
      ProtocolErrorCode.malformedResponse,
      '未知的灯具测试动作',
    ),
  );
}

enum LampTargetType {
  number('number'),
  virtualNumber('virtual'),
  group('group'),
  area('area');

  const LampTargetType(this.wireName);
  final String wireName;

  static LampTargetType parse(Object? value) => values.firstWhere(
    (item) => item.wireName == value,
    orElse: () => throw const ProtocolException(
      ProtocolErrorCode.malformedResponse,
      '未知的灯具目标类型',
    ),
  );
}

class LampTarget {
  const LampTarget({required this.networkId, required this.type, this.value});

  static const legacyDefault = LampTarget(
    networkId: 0,
    type: LampTargetType.number,
    value: 0x50bb,
  );

  final int networkId;
  final LampTargetType type;
  final int? value;

  int? get destinationAddress => switch (type) {
    LampTargetType.number => value,
    LampTargetType.virtualNumber => value == null ? null : 0x8000 | value!,
    LampTargetType.group => value == null ? null : 0xc000 | value!,
    LampTargetType.area => 0xffff,
  };

  Map<String, Object> toJson() {
    final result = <String, Object>{
      'network_id': networkId,
      'type': type.wireName,
    };
    if (type != LampTargetType.area) result['value'] = value ?? -1;
    return result;
  }

  static LampTarget fromJson(Object? value) {
    final map = _expectMap(value, 'target');
    final type = LampTargetType.parse(map['type']);
    final targetValue = map['value'];
    if (type != LampTargetType.area && targetValue is! int) {
      throw const ProtocolException(
        ProtocolErrorCode.malformedResponse,
        'target.value 类型错误',
      );
    }
    return LampTarget(
      networkId: _expectInt(map['network_id'], 'target.network_id'),
      type: type,
      value: type == LampTargetType.area ? null : targetValue as int,
    );
  }

  List<String> validate() {
    final errors = <String>[];
    if (networkId < 0 || networkId > 0xffff) {
      errors.add('灯具区必须为 0～65535');
    }
    switch (type) {
      case LampTargetType.number:
        if (value == null || value! < 0 || value! > 0x7fff) {
          errors.add('灯号必须为 0～32767');
        }
      case LampTargetType.virtualNumber:
        if (value == null || value! < 0 || value! > 0x3fff) {
          errors.add('标签必须为 0～16383');
        }
      case LampTargetType.group:
        if (value == null || value! < 0 || value! > 0x3ffe) {
          errors.add('组号必须为 0～16382');
        }
      case LampTargetType.area:
        break;
    }
    return errors;
  }

  String get displayLabel => switch (type) {
    LampTargetType.number => '区 $networkId · 号 ${value ?? '—'}',
    LampTargetType.virtualNumber => '区 $networkId · 标签 ${value ?? '—'}',
    LampTargetType.group => '区 $networkId · 组 ${value ?? '—'}',
    LampTargetType.area => '区 $networkId · 整区',
  };
}

sealed class TimerSchedule {
  const TimerSchedule();

  Map<String, Object> toJson();
}

class DurationSchedule extends TimerSchedule {
  const DurationSchedule({required this.value, required this.unit});

  final int value;
  final DurationUnit unit;

  int get totalMinutes => value * unit.minutes;

  @override
  Map<String, Object> toJson() => <String, Object>{
    'value': value,
    'unit': unit.wireName,
  };
}

class DailySchedule extends TimerSchedule {
  const DailySchedule({required this.hour, required this.minute});

  final int hour;
  final int minute;

  @override
  Map<String, Object> toJson() => <String, Object>{
    'hour': hour,
    'minute': minute,
  };
}

class IntervalSchedule extends TimerSchedule {
  const IntervalSchedule({this.value = 1, required this.unit});

  final int value;
  final DurationUnit unit;

  int get totalMinutes => value * unit.minutes;

  @override
  Map<String, Object> toJson() => <String, Object>{
    'value': value,
    'unit': unit.wireName,
  };
}

class TimerAction {
  const TimerAction({
    required this.type,
    this.sceneNo,
    this.target = LampTarget.legacyDefault,
  });

  final TimerActionType type;
  final int? sceneNo;
  final LampTarget target;

  Map<String, Object> toJson() {
    final result = <String, Object>{
      'type': type.wireName,
      'target': target.toJson(),
    };
    if (type == TimerActionType.sceneSwitch) {
      result['scene_no'] = sceneNo ?? -1;
    }
    return result;
  }

  static TimerAction fromJson(Object? value) {
    final map = _expectMap(value, 'action');
    final type = TimerActionType.parse(map['type']);
    final sceneNo = map['scene_no'];
    if (sceneNo != null && sceneNo is! int) {
      throw const ProtocolException(
        ProtocolErrorCode.malformedResponse,
        'action.scene_no 类型错误',
      );
    }
    final target = map['target'] == null
        ? LampTarget.legacyDefault
        : LampTarget.fromJson(map['target']);
    return TimerAction(type: type, sceneNo: sceneNo as int?, target: target);
  }
}

class TimerTask {
  const TimerTask({
    this.taskId,
    this.revision,
    required this.enabled,
    required this.name,
    required this.type,
    required this.schedule,
    required this.action,
  });

  final int? taskId;
  final int? revision;
  final bool enabled;
  final String name;
  final TimerType type;
  final TimerSchedule schedule;
  final TimerAction action;

  factory TimerTask.fromJson(Object? value) {
    final map = _expectMap(value, 'task');
    final type = TimerType.parse(map['type']);
    final scheduleMap = _expectMap(map['schedule'], 'schedule');
    final TimerSchedule schedule;
    switch (type) {
      case TimerType.duration:
        schedule = DurationSchedule(
          value: _expectInt(scheduleMap['value'], 'schedule.value'),
          unit: DurationUnit.parse(scheduleMap['unit']),
        );
      case TimerType.daily:
        schedule = DailySchedule(
          hour: _expectInt(scheduleMap['hour'], 'schedule.hour'),
          minute: _expectInt(scheduleMap['minute'], 'schedule.minute'),
        );
      case TimerType.interval:
        schedule = IntervalSchedule(
          value: scheduleMap['value'] == null
              ? 1
              : _expectInt(scheduleMap['value'], 'schedule.value'),
          unit: DurationUnit.parse(scheduleMap['unit']),
        );
    }

    return TimerTask(
      taskId: _optionalInt(map['task_id'], 'task_id'),
      revision: _optionalInt(map['revision'], 'revision'),
      enabled: _expectBool(map['enabled'], 'enabled'),
      name: _expectString(map['name'], 'name'),
      type: type,
      schedule: schedule,
      action: TimerAction.fromJson(map['action']),
    );
  }

  Map<String, Object> toJson() {
    final result = <String, Object>{
      'enabled': enabled,
      'name': name,
      'type': type.wireName,
      'schedule': schedule.toJson(),
      'action': action.toJson(),
    };
    if (taskId case final taskId?) result['task_id'] = taskId;
    if (revision case final revision?) result['revision'] = revision;
    return result;
  }

  List<String> validate({required bool timeTrusted}) {
    final errors = <String>[];
    final nameLength = utf8.encode(name).length;
    if (name.trim().isEmpty) errors.add('请输入任务名称');
    if (nameLength > 32) errors.add('任务名称最多 32 个 UTF-8 字节');
    if (enabled && !timeTrusted) errors.add('等待时间同步后才能启用任务');

    switch (schedule) {
      case DurationSchedule schedule:
        if (type != TimerType.duration) errors.add('任务类型与倒计时参数不一致');
        if (schedule.value <= 0 || schedule.totalMinutes > 52 * 7 * 24 * 60) {
          errors.add('倒计时范围为 1 分钟～52 周');
        }
      case DailySchedule schedule:
        if (type != TimerType.daily) errors.add('任务类型与每日参数不一致');
        if (schedule.hour < 0 || schedule.hour > 23) {
          errors.add('小时必须为 0～23');
        }
        if (schedule.minute < 0 || schedule.minute > 59) {
          errors.add('分钟必须为 0～59');
        }
      case IntervalSchedule schedule:
        if (type != TimerType.interval) errors.add('任务类型与循环参数不一致');
        if (schedule.unit != DurationUnit.minute &&
            schedule.unit != DurationUnit.hour) {
          errors.add('循环任务只支持每分钟或每小时');
        }
        if (schedule.value <= 0 || schedule.totalMinutes > 52 * 7 * 24 * 60) {
          errors.add('循环间隔范围为 1 分钟～52 周');
        }
    }

    if (action.type == TimerActionType.sceneSwitch &&
        (action.sceneNo == null ||
            action.sceneNo! < 1 ||
            action.sceneNo! > 16)) {
      errors.add('场景号必须为 1～16');
    }
    errors.addAll(action.target.validate());
    return errors;
  }

  TimerTask copyWith({
    int? taskId,
    int? revision,
    bool? enabled,
    String? name,
    TimerType? type,
    TimerSchedule? schedule,
    TimerAction? action,
  }) {
    return TimerTask(
      taskId: taskId ?? this.taskId,
      revision: revision ?? this.revision,
      enabled: enabled ?? this.enabled,
      name: name ?? this.name,
      type: type ?? this.type,
      schedule: schedule ?? this.schedule,
      action: action ?? this.action,
    );
  }
}

class TimerTaskSummary {
  const TimerTaskSummary({
    required this.taskId,
    required this.revision,
    required this.enabled,
    required this.name,
    required this.type,
    this.target,
  });

  final int taskId;
  final int revision;
  final bool enabled;
  final String name;
  final TimerType type;
  final LampTarget? target;

  factory TimerTaskSummary.fromJson(Object? value) {
    final map = _expectMap(value, 'timer.list item');
    return TimerTaskSummary(
      taskId: _expectInt(map['task_id'], 'task_id'),
      revision: _expectInt(map['revision'], 'revision'),
      enabled: _expectBool(map['enabled'], 'enabled'),
      name: _expectString(map['name'], 'name'),
      type: TimerType.parse(map['type']),
      target: map['target'] == null ? null : LampTarget.fromJson(map['target']),
    );
  }

  TimerTaskSummary copyWith({
    int? revision,
    bool? enabled,
    String? name,
    LampTarget? target,
  }) {
    return TimerTaskSummary(
      taskId: taskId,
      revision: revision ?? this.revision,
      enabled: enabled ?? this.enabled,
      name: name ?? this.name,
      type: type,
      target: target ?? this.target,
    );
  }
}

Map<String, Object?> _expectMap(Object? value, String name) {
  if (value is! Map) {
    throw ProtocolException(ProtocolErrorCode.malformedResponse, '$name 类型错误');
  }
  return value.map((key, item) => MapEntry(key.toString(), item));
}

int _expectInt(Object? value, String name) {
  if (value is! int) {
    throw ProtocolException(ProtocolErrorCode.malformedResponse, '$name 类型错误');
  }
  return value;
}

int? _optionalInt(Object? value, String name) =>
    value == null ? null : _expectInt(value, name);

bool _expectBool(Object? value, String name) {
  if (value is! bool) {
    throw ProtocolException(ProtocolErrorCode.malformedResponse, '$name 类型错误');
  }
  return value;
}

String _expectString(Object? value, String name) {
  if (value is! String) {
    throw ProtocolException(ProtocolErrorCode.malformedResponse, '$name 类型错误');
  }
  return value;
}
