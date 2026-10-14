import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:yx_gateway_timer/domain/protocol_error.dart';
import 'package:yx_gateway_timer/domain/timer_task.dart';
import 'package:yx_gateway_timer/presentation/design/app_design.dart';
import 'package:yx_gateway_timer/presentation/providers.dart';

class TaskEditorPage extends ConsumerStatefulWidget {
  const TaskEditorPage({
    required this.initialTask,
    required this.timeTrusted,
    this.supportedFeatures = const <String>{},
    this.onDelete,
    super.key,
  });

  final TimerTask initialTask;
  final bool timeTrusted;
  final Set<String> supportedFeatures;
  final Future<void> Function()? onDelete;

  @override
  ConsumerState<TaskEditorPage> createState() => _TaskEditorPageState();
}

class _TaskEditorPageState extends ConsumerState<TaskEditorPage> {
  late final TextEditingController _nameController;
  late final TextEditingController _durationController;
  late final TextEditingController _sceneController;
  late final TextEditingController _networkController;
  late final TextEditingController _targetController;
  late final FixedExtentScrollController _durationHourWheelController;
  late final FixedExtentScrollController _durationMinuteWheelController;
  late final FixedExtentScrollController _dailyHourWheelController;
  late final FixedExtentScrollController _dailyMinuteWheelController;
  late bool _enabled;
  late TimerType _type;
  late DurationUnit _unit;
  late TimerActionType _actionType;
  late LampTargetType _targetType;
  int _durationHours = 0;
  int _durationMinutePart = 30;
  int _hour = 7;
  int _minute = 30;
  bool _saving = false;
  bool _deleting = false;
  bool _testCommandBusy = false;
  String _testFeedback = '使用上方目标，经网关直接下发测试指令。';
  bool _testFeedbackIsError = false;

  @override
  void initState() {
    super.initState();
    final task = widget.initialTask;
    _nameController = TextEditingController(text: task.name);
    _enabled = task.enabled;
    _type = task.type;
    _actionType = task.action.type;
    _targetType = task.action.target.type;
    _sceneController = TextEditingController(
      text: (task.action.sceneNo ?? 1).toString(),
    );
    _networkController = TextEditingController(
      text: task.action.target.networkId.toString(),
    );
    _targetController = TextEditingController(
      text: _formatTargetInput(task.action.target),
    );
    switch (task.schedule) {
      case DurationSchedule schedule:
        final totalMinutes = schedule.totalMinutes;
        _durationHours = totalMinutes ~/ 60;
        _durationMinutePart = totalMinutes % 60;
        _durationController = TextEditingController(
          text: totalMinutes.toString(),
        );
        _unit = DurationUnit.minute;
      case DailySchedule schedule:
        _durationController = TextEditingController(text: '30');
        _unit = DurationUnit.minute;
        _hour = schedule.hour;
        _minute = schedule.minute;
      case IntervalSchedule schedule:
        _durationController = TextEditingController(
          text: schedule.value.toString(),
        );
        _unit = schedule.unit;
    }
    _durationHourWheelController = FixedExtentScrollController(
      initialItem: _durationHours,
    );
    _durationMinuteWheelController = FixedExtentScrollController(
      initialItem: _durationMinutePart,
    );
    _dailyHourWheelController = FixedExtentScrollController(initialItem: _hour);
    _dailyMinuteWheelController = FixedExtentScrollController(
      initialItem: _minute,
    );
    _nameController.addListener(_rebuild);
    _durationController.addListener(_rebuild);
    _sceneController.addListener(_rebuild);
    _networkController.addListener(_rebuild);
    _targetController.addListener(_rebuild);
  }

  void _rebuild() {
    if (mounted) setState(() {});
  }

  bool get _supportsInterval =>
      widget.supportedFeatures.contains('interval') ||
      widget.initialTask.type == TimerType.interval;

  bool get _supportsLight =>
      widget.supportedFeatures.contains('gs200_light') ||
      widget.initialTask.action.type == TimerActionType.lightOn ||
      widget.initialTask.action.type == TimerActionType.lightOff;

  bool get _supportsScene =>
      widget.supportedFeatures.contains('gs200_scene') ||
      widget.initialTask.action.type == TimerActionType.sceneSwitch;

  bool get _supportsLightTest =>
      widget.supportedFeatures.contains('gs200_light_test');

  bool get _supportsTargetedSceneTest =>
      widget.supportedFeatures.contains('gs200_scene_test') &&
      widget.supportedFeatures.contains('gs200_task_target');

  List<TimerActionType> get _availableActions => <TimerActionType>[
    if (_supportsLight) TimerActionType.lightOn,
    if (_supportsLight) TimerActionType.lightOff,
    if (_supportsScene) TimerActionType.sceneSwitch,
  ];

  @override
  void dispose() {
    _nameController
      ..removeListener(_rebuild)
      ..dispose();
    _durationController
      ..removeListener(_rebuild)
      ..dispose();
    _sceneController
      ..removeListener(_rebuild)
      ..dispose();
    _networkController
      ..removeListener(_rebuild)
      ..dispose();
    _targetController
      ..removeListener(_rebuild)
      ..dispose();
    _durationHourWheelController.dispose();
    _durationMinuteWheelController.dispose();
    _dailyHourWheelController.dispose();
    _dailyMinuteWheelController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final editing = widget.initialTask.taskId != null;
    final nameBytes = utf8.encode(_nameController.text).length;
    final canDelete = editing && widget.onDelete != null;
    final busy = _saving || _deleting || _testCommandBusy;
    return Scaffold(
      body: AppBackdrop(
        child: SafeArea(
          child: ListView(
            padding: const EdgeInsets.fromLTRB(20, 18, 20, 42),
            children: <Widget>[
              _EditorHeader(
                title: editing ? '编辑任务' : '新建任务',
                onBack: () => Navigator.maybePop(context),
              ),
              const SizedBox(height: 22),
              GlassCard(
                radius: 19,
                padding: const EdgeInsets.fromLTRB(15, 13, 15, 13),
                child: Row(
                  children: <Widget>[
                    Container(
                      width: 38,
                      height: 38,
                      decoration: BoxDecoration(
                        color: const Color(0xffe3f1ff),
                        borderRadius: BorderRadius.circular(12),
                      ),
                      child: const Icon(
                        Icons.bluetooth_connected_rounded,
                        color: AppColors.blue,
                        size: 20,
                      ),
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: <Widget>[
                          Text(
                            '当前连接设备',
                            style: Theme.of(context).textTheme.bodySmall,
                          ),
                          const SizedBox(height: 2),
                          Text(
                            '保存后立即同步',
                            style: Theme.of(context).textTheme.labelLarge,
                          ),
                        ],
                      ),
                    ),
                    const Icon(
                      Icons.check_circle_rounded,
                      color: AppColors.green,
                      size: 20,
                    ),
                  ],
                ),
              ),
              const SizedBox(height: 22),
              const _SectionLabel('任务名称'),
              const SizedBox(height: 9),
              GlassCard(
                radius: 19,
                padding: const EdgeInsets.fromLTRB(17, 10, 14, 8),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: <Widget>[
                    TextField(
                      controller: _nameController,
                      textInputAction: TextInputAction.done,
                      style: Theme.of(context).textTheme.titleMedium,
                      decoration: InputDecoration(
                        border: InputBorder.none,
                        enabledBorder: InputBorder.none,
                        focusedBorder: InputBorder.none,
                        filled: false,
                        hintText: '例如：睡前关灯',
                        errorText: nameBytes > 32 ? '名称最多 32 个字节' : null,
                        isDense: true,
                        contentPadding: const EdgeInsets.symmetric(vertical: 9),
                      ),
                    ),
                    Align(
                      alignment: Alignment.centerRight,
                      child: Text(
                        '$nameBytes / 32',
                        style: Theme.of(context).textTheme.labelSmall?.copyWith(
                          color: nameBytes > 32
                              ? AppColors.red
                              : AppColors.tertiary,
                        ),
                      ),
                    ),
                  ],
                ),
              ),
              const SizedBox(height: 24),
              const _SectionLabel('控制灯具'),
              const SizedBox(height: 9),
              _LampTargetCard(
                networkController: _networkController,
                targetController: _targetController,
                selectedType: _targetType,
                onTypeSelected: _selectTargetType,
              ),
              if (_supportsLightTest || _supportsTargetedSceneTest) ...<Widget>[
                const SizedBox(height: 10),
                _TaskControlTestCard(
                  sceneController: _sceneController,
                  lightTestEnabled: _supportsLightTest,
                  sceneTestEnabled: _supportsTargetedSceneTest,
                  busy: _testCommandBusy,
                  feedback: _testFeedback,
                  feedbackIsError: _testFeedbackIsError,
                  onLightOn: () => _runLightTest(LampTestAction.lightOn),
                  onLightOff: () => _runLightTest(LampTestAction.lightOff),
                  onFlash: () => _runLightTest(LampTestAction.flash),
                  onReadScene: _readSceneForTest,
                  onSetScene: _setSceneForTest,
                ),
              ],
              const SizedBox(height: 24),
              const _SectionLabel('执行动作'),
              const SizedBox(height: 9),
              GlassCard(
                radius: 20,
                padding: const EdgeInsets.all(10),
                child: AppSegment<TimerActionType>(
                  values: _availableActions,
                  selected: _actionType,
                  labelOf: _actionLabel,
                  iconOf: _actionIcon,
                  onSelected: (value) {
                    HapticFeedback.selectionClick();
                    setState(() {
                      _actionType = value;
                      if (value == TimerActionType.sceneSwitch &&
                          _sceneController.text.isEmpty) {
                        _sceneController.text = '1';
                      }
                    });
                  },
                ),
              ),
              AnimatedSize(
                duration: const Duration(milliseconds: 190),
                curve: Curves.easeOutCubic,
                alignment: Alignment.topCenter,
                child: _actionType == TimerActionType.sceneSwitch
                    ? Padding(
                        padding: const EdgeInsets.only(top: 10),
                        child: GlassCard(
                          radius: 19,
                          child: _SceneNumberRow(controller: _sceneController),
                        ),
                      )
                    : const SizedBox.shrink(),
              ),
              const SizedBox(height: 24),
              const _SectionLabel('执行计划'),
              const SizedBox(height: 9),
              GlassCard(
                radius: 20,
                padding: const EdgeInsets.all(10),
                child: AppSegment<TimerType>(
                  values: <TimerType>[
                    if (editing || _type == TimerType.duration)
                      TimerType.duration,
                    TimerType.daily,
                    if (_supportsInterval) TimerType.interval,
                  ],
                  selected: _type,
                  labelOf: (type) => switch (type) {
                    TimerType.duration => '倒计时',
                    TimerType.daily => '每天',
                    TimerType.interval => '循环',
                  },
                  onSelected: (type) {
                    HapticFeedback.selectionClick();
                    setState(() {
                      _type = type;
                      if (type == TimerType.interval &&
                          _unit != DurationUnit.minute &&
                          _unit != DurationUnit.hour) {
                        _unit = DurationUnit.minute;
                      }
                    });
                  },
                ),
              ),
              const SizedBox(height: 16),
              _SectionLabel(_schedulePrompt),
              const SizedBox(height: 9),
              AnimatedSwitcher(
                duration: const Duration(milliseconds: 180),
                switchInCurve: Curves.easeOutCubic,
                switchOutCurve: Curves.easeInCubic,
                child: switch (_type) {
                  TimerType.duration => _TimeWheelCard(
                    key: const ValueKey<String>('duration-wheel'),
                    hourController: _durationHourWheelController,
                    minuteController: _durationMinuteWheelController,
                    selectedHour: _durationHours,
                    selectedMinute: _durationMinutePart,
                    maxHour: 8736,
                    quickValue: (
                      hours: _durationHours,
                      minutes: _durationMinutePart,
                    ),
                    onHourChanged: (value) {
                      setState(() {
                        _durationHours = value;
                        _syncDurationController();
                      });
                    },
                    onMinuteChanged: (value) {
                      setState(() {
                        _durationMinutePart = value;
                        _syncDurationController();
                      });
                    },
                    onQuickSelected: _setDurationParts,
                  ),
                  TimerType.daily => _TimeWheelCard(
                    key: const ValueKey<String>('daily-wheel'),
                    hourController: _dailyHourWheelController,
                    minuteController: _dailyMinuteWheelController,
                    selectedHour: _hour,
                    selectedMinute: _minute,
                    maxHour: 23,
                    onHourChanged: (value) {
                      setState(() => _hour = value);
                    },
                    onMinuteChanged: (value) {
                      setState(() => _minute = value);
                    },
                  ),
                  TimerType.interval => GlassCard(
                    key: const ValueKey<String>('interval-picker'),
                    radius: 22,
                    padding: const EdgeInsets.all(12),
                    child: Column(
                      children: <Widget>[
                        TextField(
                          controller: _durationController,
                          keyboardType: TextInputType.number,
                          textInputAction: TextInputAction.done,
                          inputFormatters: <TextInputFormatter>[
                            FilteringTextInputFormatter.digitsOnly,
                            LengthLimitingTextInputFormatter(6),
                          ],
                          decoration: const InputDecoration(
                            labelText: '循环间隔',
                            hintText: '输入间隔数值',
                          ),
                        ),
                        const SizedBox(height: 12),
                        AppSegment<DurationUnit>(
                          values: const <DurationUnit>[
                            DurationUnit.minute,
                            DurationUnit.hour,
                          ],
                          selected: _unit,
                          labelOf: (unit) =>
                              unit == DurationUnit.minute ? '分钟' : '小时',
                          iconOf: (unit) => unit == DurationUnit.minute
                              ? Icons.timer_outlined
                              : Icons.schedule_rounded,
                          onSelected: (unit) {
                            HapticFeedback.selectionClick();
                            setState(() => _unit = unit);
                          },
                        ),
                      ],
                    ),
                  ),
                },
              ),
              const SizedBox(height: 14),
              _ExecutionPreview(
                preview: _executionPreview,
                action: _actionSummary,
              ),
              const SizedBox(height: 16),
              GlassCard(
                radius: 19,
                child: SwitchListTile(
                  contentPadding: const EdgeInsets.fromLTRB(16, 4, 11, 4),
                  secondary: Container(
                    width: 38,
                    height: 38,
                    decoration: BoxDecoration(
                      color: (_enabled ? AppColors.green : AppColors.tertiary)
                          .withValues(alpha: 0.12),
                      borderRadius: BorderRadius.circular(12),
                    ),
                    child: Icon(
                      _enabled ? Icons.play_arrow_rounded : Icons.pause_rounded,
                      color: _enabled ? AppColors.green : AppColors.tertiary,
                    ),
                  ),
                  title: const Text('启用任务'),
                  subtitle: Text(
                    widget.timeTrusted
                        ? (_enabled ? '保存后按计划执行' : '保持暂停，不会自动执行')
                        : '设备时间不可信，只能保持停用',
                  ),
                  value: _enabled,
                  onChanged: widget.timeTrusted || _enabled
                      ? (value) {
                          HapticFeedback.selectionClick();
                          setState(() => _enabled = value);
                        }
                      : null,
                ),
              ),
              if (!widget.timeTrusted) ...<Widget>[
                const SizedBox(height: 12),
                const _InlineNotice(
                  icon: Icons.schedule_rounded,
                  text: '等待设备网络校时后，才能启用这个任务。',
                ),
              ],
              const SizedBox(height: 22),
              GradientActionButton(
                label: '保存并同步',
                icon: Icons.sync_rounded,
                loading: _saving,
                onPressed: busy ? null : _save,
              ),
              const SizedBox(height: 11),
              Text(
                '保存后会立即同步时间，并重新读取设备中的任务列表。',
                textAlign: TextAlign.center,
                style: Theme.of(
                  context,
                ).textTheme.bodySmall?.copyWith(color: AppColors.tertiary),
              ),
              if (canDelete) ...<Widget>[
                const SizedBox(height: 16),
                SizedBox(
                  width: double.infinity,
                  child: TextButton.icon(
                    style: TextButton.styleFrom(
                      foregroundColor: AppColors.red,
                      minimumSize: const Size.fromHeight(52),
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(16),
                      ),
                    ),
                    onPressed: busy ? null : _confirmDelete,
                    icon: _deleting
                        ? const SizedBox.square(
                            dimension: 18,
                            child: CircularProgressIndicator(strokeWidth: 2),
                          )
                        : const Icon(Icons.delete_outline_rounded),
                    label: const Text('删除任务'),
                  ),
                ),
              ],
            ],
          ),
        ),
      ),
    );
  }

  String get _actionSummary => _actionType == TimerActionType.sceneSwitch
      ? '场景 ${_parseFlexibleInt(_sceneController.text) ?? '—'}'
      : _actionLabel(_actionType);

  String get _schedulePrompt => switch (_type) {
    TimerType.duration => '多久以后',
    TimerType.daily => '每天几点',
    TimerType.interval => '多久一次',
  };

  String get _executionPreview {
    if (_type == TimerType.daily) {
      return '每天 ${_hour.toString().padLeft(2, '0')}:'
          '${_minute.toString().padLeft(2, '0')}';
    }
    if (_type == TimerType.interval) {
      final value = int.tryParse(_durationController.text) ?? 0;
      return '每 $value ${_unit == DurationUnit.hour ? '小时' : '分钟'}执行';
    }
    final now = DateTime.now();
    final target = now.add(
      Duration(minutes: _durationHours * 60 + _durationMinutePart),
    );
    final sameDay =
        target.year == now.year &&
        target.month == now.month &&
        target.day == now.day;
    final tomorrow = now.add(const Duration(days: 1));
    final nextDay =
        target.year == tomorrow.year &&
        target.month == tomorrow.month &&
        target.day == tomorrow.day;
    final day = sameDay
        ? '今天'
        : nextDay
        ? '明天'
        : '${target.month}月${target.day}日';
    return '$day ${target.hour.toString().padLeft(2, '0')}:'
        '${target.minute.toString().padLeft(2, '0')}';
  }

  void _syncDurationController() {
    _unit = DurationUnit.minute;
    _durationController.text = (_durationHours * 60 + _durationMinutePart)
        .toString();
  }

  void _setDurationParts(int hours, int minutes) {
    HapticFeedback.selectionClick();
    setState(() {
      _durationHours = hours;
      _durationMinutePart = minutes;
      _syncDurationController();
    });
    if (_durationHourWheelController.hasClients) {
      _durationHourWheelController.jumpToItem(hours);
    }
    if (_durationMinuteWheelController.hasClients) {
      _durationMinuteWheelController.jumpToItem(minutes);
    }
  }

  TimerTask _taskFromForm() {
    final TimerSchedule schedule = switch (_type) {
      TimerType.duration => DurationSchedule(
        value: _durationHours * 60 + _durationMinutePart,
        unit: DurationUnit.minute,
      ),
      TimerType.daily => DailySchedule(hour: _hour, minute: _minute),
      TimerType.interval => IntervalSchedule(
        value: int.tryParse(_durationController.text) ?? 0,
        unit: _unit,
      ),
    };
    return TimerTask(
      taskId: widget.initialTask.taskId,
      revision: widget.initialTask.revision,
      enabled: _enabled,
      name: _nameController.text.trim(),
      type: _type,
      schedule: schedule,
      action: TimerAction(
        type: _actionType,
        sceneNo: _actionType == TimerActionType.sceneSwitch
            ? _parseFlexibleInt(_sceneController.text)
            : null,
        target: LampTarget(
          networkId: _parseFlexibleInt(_networkController.text) ?? -1,
          type: _targetType,
          value: _targetType == LampTargetType.area
              ? null
              : _parseFlexibleInt(_targetController.text),
        ),
      ),
    );
  }

  void _selectTargetType(LampTargetType type) {
    HapticFeedback.selectionClick();
    setState(() {
      _targetType = type;
      final current = _parseFlexibleInt(_targetController.text);
      final valid = LampTarget(
        networkId: 0,
        type: type,
        value: current,
      ).validate().isEmpty;
      if (!valid) {
        _targetController.text = switch (type) {
          LampTargetType.number => '0',
          LampTargetType.virtualNumber => '1',
          LampTargetType.group => '1',
          LampTargetType.area => '',
        };
      }
    });
  }

  LampTarget? _testTarget() {
    final target = LampTarget(
      networkId: _parseFlexibleInt(_networkController.text) ?? -1,
      type: _targetType,
      value: _targetType == LampTargetType.area
          ? null
          : _parseFlexibleInt(_targetController.text),
    );
    final errors = target.validate();
    if (errors.isNotEmpty) {
      setState(() {
        _testFeedback = errors.first;
        _testFeedbackIsError = true;
      });
      return null;
    }
    return target;
  }

  Future<void> _runLightTest(LampTestAction action) async {
    final target = _testTarget();
    if (target == null) return;
    FocusManager.instance.primaryFocus?.unfocus();
    setState(() {
      _testCommandBusy = true;
      _testFeedback = '正在把${action.label}指令写入网关…';
      _testFeedbackIsError = false;
    });
    try {
      final result = await ref
          .read(gatewayControllerProvider.notifier)
          .testLamp(action, target);
      if (!mounted) return;
      HapticFeedback.lightImpact();
      setState(() {
        _testFeedback =
            '${result.action.label}指令已写入网关（${result.target.displayLabel}），'
            '请观察灯具。';
        _testFeedbackIsError = false;
      });
    } on Object catch (error) {
      _setTestError(error);
    } finally {
      if (mounted) setState(() => _testCommandBusy = false);
    }
  }

  Future<void> _setSceneForTest() async {
    final target = _testTarget();
    if (target == null) return;
    final sceneNo = _parseFlexibleInt(_sceneController.text);
    if (sceneNo == null || sceneNo < 1 || sceneNo > 16) {
      setState(() {
        _testFeedback = '场景号必须为 1～16';
        _testFeedbackIsError = true;
      });
      return;
    }
    FocusManager.instance.primaryFocus?.unfocus();
    setState(() {
      _testCommandBusy = true;
      _testFeedback = '正在把场景 $sceneNo 设置指令写入网关…';
      _testFeedbackIsError = false;
    });
    try {
      final result = await ref
          .read(gatewayControllerProvider.notifier)
          .setCurrentScene(sceneNo, target: target);
      if (!mounted) return;
      HapticFeedback.lightImpact();
      setState(() {
        _testFeedback =
            '场景 ${result.sceneNo} 设置指令已写入网关（${target.displayLabel}），'
            '可读取确认。';
        _testFeedbackIsError = false;
      });
    } on Object catch (error) {
      _setTestError(error);
    } finally {
      if (mounted) setState(() => _testCommandBusy = false);
    }
  }

  Future<void> _readSceneForTest() async {
    final target = _testTarget();
    if (target == null) return;
    FocusManager.instance.primaryFocus?.unfocus();
    setState(() {
      _testCommandBusy = true;
      _testFeedback = '正在等待 ${target.displayLabel} 返回场景号…';
      _testFeedbackIsError = false;
    });
    try {
      final result = await ref
          .read(gatewayControllerProvider.notifier)
          .readCurrentScene(target: target);
      if (!mounted) return;
      HapticFeedback.lightImpact();
      _sceneController.text = result.sceneNo.toString();
      setState(() {
        _testFeedback = '${target.displayLabel} 当前场景号：${result.sceneNo}';
        _testFeedbackIsError = false;
      });
    } on Object catch (error) {
      _setTestError(error);
    } finally {
      if (mounted) setState(() => _testCommandBusy = false);
    }
  }

  void _setTestError(Object error) {
    if (!mounted) return;
    setState(() {
      _testFeedback = error is ProtocolException
          ? error.message
          : error.toString();
      _testFeedbackIsError = true;
    });
  }

  Future<void> _save() async {
    final task = _taskFromForm();
    final errors = task.validate(timeTrusted: widget.timeTrusted);
    if (errors.isNotEmpty) {
      _showError(errors.first);
      return;
    }
    setState(() => _saving = true);
    try {
      final saved = await ref
          .read(gatewayControllerProvider.notifier)
          .saveTask(task);
      HapticFeedback.lightImpact();
      if (mounted) Navigator.of(context).pop(saved);
    } on ProtocolException catch (error) {
      if (error.isRevisionConflict) {
        _showError('设备配置已变化，已重新读取，请返回后重新确认');
      } else {
        _showError(error.message);
      }
    } on Object catch (error) {
      _showError(error.toString());
    } finally {
      if (mounted) setState(() => _saving = false);
    }
  }

  Future<void> _confirmDelete() async {
    final onDelete = widget.onDelete;
    if (onDelete == null) return;
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (dialogContext) => AlertDialog(
        title: const Text('删除这个任务？'),
        content: Text('“${widget.initialTask.name}”删除后无法恢复。'),
        actions: <Widget>[
          TextButton(
            onPressed: () => Navigator.pop(dialogContext, false),
            child: const Text('取消'),
          ),
          FilledButton(
            style: FilledButton.styleFrom(
              backgroundColor: Theme.of(context).colorScheme.error,
              foregroundColor: Theme.of(context).colorScheme.onError,
            ),
            onPressed: () => Navigator.pop(dialogContext, true),
            child: const Text('删除任务'),
          ),
        ],
      ),
    );
    if (confirmed != true || !mounted) return;
    setState(() => _deleting = true);
    try {
      await onDelete();
      HapticFeedback.mediumImpact();
      if (mounted) Navigator.of(context).pop();
    } on ProtocolException catch (error) {
      _showError(
        error.isRevisionConflict ? '设备配置已变化，已重新读取，请返回后重新确认' : error.message,
      );
    } on Object catch (error) {
      _showError(error.toString());
    } finally {
      if (mounted) setState(() => _deleting = false);
    }
  }

  void _showError(String message) {
    if (!mounted) return;
    ScaffoldMessenger.of(
      context,
    ).showSnackBar(SnackBar(content: Text(message)));
  }
}

class _EditorHeader extends StatelessWidget {
  const _EditorHeader({required this.title, required this.onBack});

  final String title;
  final VoidCallback onBack;

  @override
  Widget build(BuildContext context) => SizedBox(
    height: 48,
    child: Stack(
      alignment: Alignment.center,
      children: <Widget>[
        Align(
          alignment: Alignment.centerLeft,
          child: GlassIconButton(
            icon: Icons.arrow_back_ios_new_rounded,
            tooltip: '返回',
            onPressed: onBack,
          ),
        ),
        Text(title, style: Theme.of(context).textTheme.titleLarge),
      ],
    ),
  );
}

class _TimeWheelCard extends StatelessWidget {
  const _TimeWheelCard({
    required this.hourController,
    required this.minuteController,
    required this.selectedHour,
    required this.selectedMinute,
    required this.maxHour,
    required this.onHourChanged,
    required this.onMinuteChanged,
    this.quickValue,
    this.onQuickSelected,
    super.key,
  });

  final FixedExtentScrollController hourController;
  final FixedExtentScrollController minuteController;
  final int selectedHour;
  final int selectedMinute;
  final int maxHour;
  final ValueChanged<int> onHourChanged;
  final ValueChanged<int> onMinuteChanged;
  final ({int hours, int minutes})? quickValue;
  final void Function(int hours, int minutes)? onQuickSelected;

  @override
  Widget build(BuildContext context) => GlassCard(
    radius: 22,
    padding: const EdgeInsets.fromLTRB(13, 11, 13, 13),
    child: Column(
      children: <Widget>[
        SizedBox(
          height: 150,
          child: Stack(
            alignment: Alignment.center,
            children: <Widget>[
              Positioned(
                left: 1,
                right: 1,
                child: Container(
                  height: 49,
                  decoration: BoxDecoration(
                    color: AppColors.blue.withValues(alpha: 0.075),
                    borderRadius: BorderRadius.circular(15),
                  ),
                ),
              ),
              ShaderMask(
                blendMode: BlendMode.dstIn,
                shaderCallback: (bounds) => const LinearGradient(
                  begin: Alignment.topCenter,
                  end: Alignment.bottomCenter,
                  colors: <Color>[
                    Colors.transparent,
                    Colors.black,
                    Colors.black,
                    Colors.transparent,
                  ],
                  stops: <double>[0, 0.25, 0.75, 1],
                ).createShader(bounds),
                child: Row(
                  children: <Widget>[
                    Expanded(
                      child: _NumberWheel(
                        controller: hourController,
                        selected: selectedHour,
                        count: maxHour + 1,
                        unit: '小时',
                        semanticLabel: '小时',
                        onSelected: onHourChanged,
                      ),
                    ),
                    const SizedBox(width: 5),
                    Expanded(
                      child: _NumberWheel(
                        controller: minuteController,
                        selected: selectedMinute,
                        count: 60,
                        unit: '分钟',
                        semanticLabel: '分钟',
                        onSelected: onMinuteChanged,
                      ),
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
        if (quickValue != null && onQuickSelected != null) ...<Widget>[
          const SizedBox(height: 9),
          _QuickDurationChoices(
            hours: quickValue!.hours,
            minutes: quickValue!.minutes,
            onSelected: onQuickSelected!,
          ),
        ],
      ],
    ),
  );
}

class _NumberWheel extends StatelessWidget {
  const _NumberWheel({
    required this.controller,
    required this.selected,
    required this.count,
    required this.unit,
    required this.semanticLabel,
    required this.onSelected,
  });

  final FixedExtentScrollController controller;
  final int selected;
  final int count;
  final String unit;
  final String semanticLabel;
  final ValueChanged<int> onSelected;

  @override
  Widget build(BuildContext context) => Semantics(
    label: semanticLabel,
    value: '$selected $unit',
    child: Stack(
      children: <Widget>[
        ListWheelScrollView.useDelegate(
          controller: controller,
          itemExtent: 49,
          diameterRatio: 1.8,
          perspective: 0.0025,
          physics: const FixedExtentScrollPhysics(),
          onSelectedItemChanged: (value) {
            HapticFeedback.selectionClick();
            onSelected(value);
          },
          childDelegate: ListWheelChildBuilderDelegate(
            childCount: count,
            builder: (context, index) {
              final current = index == selected;
              return Align(
                alignment: Alignment.center,
                child: Padding(
                  padding: const EdgeInsets.only(right: 35),
                  child: AnimatedDefaultTextStyle(
                    duration: const Duration(milliseconds: 150),
                    curve: Curves.easeOutCubic,
                    style: TextStyle(
                      color: current ? AppColors.ink : AppColors.tertiary,
                      fontSize: current ? 30 : 20,
                      fontWeight: current ? FontWeight.w700 : FontWeight.w600,
                      letterSpacing: current ? -0.8 : 0,
                      fontFeatures: const <FontFeature>[
                        FontFeature.tabularFigures(),
                      ],
                    ),
                    child: Text('$index'),
                  ),
                ),
              );
            },
          ),
        ),
        Positioned(
          top: 0,
          right: 7,
          bottom: 0,
          child: Center(
            child: Text(
              unit,
              style: Theme.of(context).textTheme.labelSmall?.copyWith(
                color: AppColors.secondary,
                fontWeight: FontWeight.w600,
              ),
            ),
          ),
        ),
      ],
    ),
  );
}

class _QuickDurationChoices extends StatelessWidget {
  const _QuickDurationChoices({
    required this.hours,
    required this.minutes,
    required this.onSelected,
  });

  final int hours;
  final int minutes;
  final void Function(int hours, int minutes) onSelected;

  @override
  Widget build(BuildContext context) {
    const options = <(int, int, String)>[
      (0, 15, '15 分钟'),
      (0, 45, '45 分钟'),
      (1, 0, '1 小时'),
      (2, 0, '2 小时'),
    ];
    return Row(
      children: options
          .map(
            (option) => Expanded(
              child: Padding(
                padding: EdgeInsets.only(right: option == options.last ? 0 : 7),
                child: _QuickDurationChip(
                  label: option.$3,
                  selected: hours == option.$1 && minutes == option.$2,
                  onTap: () => onSelected(option.$1, option.$2),
                ),
              ),
            ),
          )
          .toList(growable: false),
    );
  }
}

class _QuickDurationChip extends StatelessWidget {
  const _QuickDurationChip({
    required this.label,
    required this.selected,
    required this.onTap,
  });

  final String label;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) => AnimatedContainer(
    duration: const Duration(milliseconds: 160),
    decoration: BoxDecoration(
      color: selected
          ? AppColors.blue.withValues(alpha: 0.11)
          : Colors.white.withValues(alpha: 0.7),
      borderRadius: BorderRadius.circular(13),
      border: Border.all(
        color: selected ? AppColors.blue.withValues(alpha: 0.4) : Colors.white,
      ),
    ),
    child: Material(
      color: Colors.transparent,
      borderRadius: BorderRadius.circular(13),
      clipBehavior: Clip.antiAlias,
      child: InkWell(
        onTap: onTap,
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 10),
          child: Text(
            label,
            maxLines: 1,
            textAlign: TextAlign.center,
            style: Theme.of(context).textTheme.labelSmall?.copyWith(
              color: selected ? AppColors.blue : AppColors.secondary,
              fontWeight: FontWeight.w700,
            ),
          ),
        ),
      ),
    ),
  );
}

class _ExecutionPreview extends StatelessWidget {
  const _ExecutionPreview({required this.preview, required this.action});

  final String preview;
  final String action;

  @override
  Widget build(BuildContext context) => GlassCard(
    radius: 19,
    color: Colors.white.withValues(alpha: 0.86),
    padding: const EdgeInsets.fromLTRB(15, 13, 14, 13),
    child: Row(
      children: <Widget>[
        Container(
          width: 38,
          height: 38,
          decoration: BoxDecoration(
            color: const Color(0xffe3f1ff),
            borderRadius: BorderRadius.circular(12),
          ),
          child: const Icon(
            Icons.schedule_rounded,
            color: AppColors.blue,
            size: 21,
          ),
        ),
        const SizedBox(width: 12),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              Text(
                '预计执行',
                style: Theme.of(context).textTheme.bodySmall?.copyWith(
                  color: AppColors.secondary,
                  fontWeight: FontWeight.w700,
                ),
              ),
              const SizedBox(height: 2),
              Text(
                '$preview · $action',
                maxLines: 2,
                overflow: TextOverflow.ellipsis,
                style: Theme.of(context).textTheme.labelLarge,
              ),
            ],
          ),
        ),
      ],
    ),
  );
}

class _ScheduleDraft {
  const _ScheduleDraft({
    required this.type,
    required this.value,
    required this.unit,
    required this.hour,
    required this.minute,
  });

  final TimerType type;
  final int value;
  final DurationUnit unit;
  final int hour;
  final int minute;
}

class _ScheduleEditorSheet extends StatefulWidget {
  const _ScheduleEditorSheet({
    required this.initial,
    required this.supportsInterval,
  });

  final _ScheduleDraft initial;
  final bool supportsInterval;

  @override
  State<_ScheduleEditorSheet> createState() => _ScheduleEditorSheetState();
}

class _ScheduleEditorSheetState extends State<_ScheduleEditorSheet> {
  late final TextEditingController _valueController;
  late TimerType _type;
  late DurationUnit _unit;
  late int _hour;
  late int _minute;

  @override
  void initState() {
    super.initState();
    _type = widget.initial.type;
    _unit = widget.initial.unit;
    _hour = widget.initial.hour;
    _minute = widget.initial.minute;
    _valueController = TextEditingController(
      text: widget.initial.value.toString(),
    )..addListener(_rebuild);
  }

  void _rebuild() {
    if (mounted) setState(() {});
  }

  @override
  void dispose() {
    _valueController
      ..removeListener(_rebuild)
      ..dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final value = int.tryParse(_valueController.text);
    final validValue = value != null && value > 0;
    return AnimatedPadding(
      duration: const Duration(milliseconds: 180),
      curve: Curves.easeOutCubic,
      padding: EdgeInsets.only(
        bottom: MediaQuery.of(context).viewInsets.bottom,
      ),
      child: SingleChildScrollView(
        padding: const EdgeInsets.fromLTRB(16, 0, 16, 24),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            _SheetHeader(
              title: '何时执行',
              onDone: _type == TimerType.duration && !validValue
                  ? null
                  : () => Navigator.pop(
                      context,
                      _ScheduleDraft(
                        type: _type,
                        value: value ?? 1,
                        unit: _unit,
                        hour: _hour,
                        minute: _minute,
                      ),
                    ),
            ),
            const SizedBox(height: 12),
            SizedBox(
              width: double.infinity,
              child: SegmentedButton<TimerType>(
                showSelectedIcon: false,
                segments: <ButtonSegment<TimerType>>[
                  const ButtonSegment<TimerType>(
                    value: TimerType.duration,
                    label: Text('倒计时'),
                  ),
                  const ButtonSegment<TimerType>(
                    value: TimerType.daily,
                    label: Text('每天'),
                  ),
                  if (widget.supportsInterval)
                    const ButtonSegment<TimerType>(
                      value: TimerType.interval,
                      label: Text('循环'),
                    ),
                ],
                selected: <TimerType>{_type},
                onSelectionChanged: (values) {
                  HapticFeedback.selectionClick();
                  setState(() {
                    _type = values.single;
                    if (_type == TimerType.interval &&
                        _unit != DurationUnit.minute &&
                        _unit != DurationUnit.hour) {
                      _unit = DurationUnit.minute;
                    }
                  });
                },
              ),
            ),
            const SizedBox(height: 20),
            AnimatedSwitcher(
              duration: const Duration(milliseconds: 180),
              switchInCurve: Curves.easeOutCubic,
              switchOutCurve: Curves.easeInCubic,
              child: switch (_type) {
                TimerType.duration => _DurationSettings(
                  key: const ValueKey<String>('duration'),
                  controller: _valueController,
                  unit: _unit,
                  onUnitChanged: (value) => setState(() => _unit = value),
                ),
                TimerType.daily => _DailySettings(
                  key: const ValueKey<String>('daily'),
                  hour: _hour,
                  minute: _minute,
                  onPick: _pickDailyTime,
                ),
                TimerType.interval => _IntervalSettings(
                  key: const ValueKey<String>('interval'),
                  unit: _unit,
                  onChanged: (value) {
                    HapticFeedback.selectionClick();
                    setState(() => _unit = value);
                  },
                ),
              },
            ),
          ],
        ),
      ),
    );
  }

  Future<void> _pickDailyTime() async {
    final selected = await showTimePicker(
      context: context,
      initialTime: TimeOfDay(hour: _hour, minute: _minute),
    );
    if (selected == null) return;
    setState(() {
      _hour = selected.hour;
      _minute = selected.minute;
    });
  }
}

class _DurationSettings extends StatelessWidget {
  const _DurationSettings({
    required this.controller,
    required this.unit,
    required this.onUnitChanged,
    super.key,
  });

  final TextEditingController controller;
  final DurationUnit unit;
  final ValueChanged<DurationUnit> onUnitChanged;

  @override
  Widget build(BuildContext context) => Column(
    crossAxisAlignment: CrossAxisAlignment.start,
    children: <Widget>[
      Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Expanded(
            child: TextField(
              controller: controller,
              keyboardType: TextInputType.number,
              inputFormatters: <TextInputFormatter>[
                FilteringTextInputFormatter.digitsOnly,
              ],
              decoration: const InputDecoration(labelText: '时长'),
            ),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: DropdownButtonFormField<DurationUnit>(
              initialValue: unit,
              decoration: const InputDecoration(labelText: '单位'),
              items: DurationUnit.values
                  .map(
                    (item) => DropdownMenuItem<DurationUnit>(
                      value: item,
                      child: Text(item.label),
                    ),
                  )
                  .toList(growable: false),
              onChanged: (value) {
                if (value != null) onUnitChanged(value);
              },
            ),
          ),
        ],
      ),
      const SizedBox(height: 14),
      const _SheetHint(text: '保存并启用后开始倒计时；到期执行一次并自动停用。'),
    ],
  );
}

class _DailySettings extends StatelessWidget {
  const _DailySettings({
    required this.hour,
    required this.minute,
    required this.onPick,
    super.key,
  });

  final int hour;
  final int minute;
  final VoidCallback onPick;

  @override
  Widget build(BuildContext context) => Column(
    crossAxisAlignment: CrossAxisAlignment.start,
    children: <Widget>[
      Card(
        color: Theme.of(context).colorScheme.surfaceContainerLow,
        child: InkWell(
          borderRadius: BorderRadius.circular(16),
          onTap: onPick,
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 18),
            child: Row(
              children: <Widget>[
                Text(
                  '${hour.toString().padLeft(2, '0')}:'
                  '${minute.toString().padLeft(2, '0')}',
                  style: Theme.of(context).textTheme.headlineMedium,
                ),
                const Spacer(),
                const Text('选择时间'),
                const SizedBox(width: 4),
                const Icon(Icons.chevron_right_rounded),
              ],
            ),
          ),
        ),
      ),
      const SizedBox(height: 14),
      const _SheetHint(text: '按照设备当前时区每天执行。'),
    ],
  );
}

class _IntervalSettings extends StatelessWidget {
  const _IntervalSettings({
    required this.unit,
    required this.onChanged,
    super.key,
  });

  final DurationUnit unit;
  final ValueChanged<DurationUnit> onChanged;

  @override
  Widget build(BuildContext context) => Column(
    crossAxisAlignment: CrossAxisAlignment.start,
    children: <Widget>[
      Card(
        color: Theme.of(context).colorScheme.surfaceContainerLow,
        clipBehavior: Clip.antiAlias,
        child: RadioGroup<DurationUnit>(
          groupValue: unit,
          onChanged: (value) {
            if (value != null) onChanged(value);
          },
          child: const Column(
            children: <Widget>[
              RadioListTile<DurationUnit>(
                title: Text('每分钟'),
                value: DurationUnit.minute,
              ),
              Divider(height: 1, indent: 16),
              RadioListTile<DurationUnit>(
                title: Text('每小时'),
                value: DurationUnit.hour,
              ),
            ],
          ),
        ),
      ),
      const SizedBox(height: 14),
      const _SheetHint(text: '从保存或重新启用时开始循环；断电期间错过的次数不会补执行。'),
    ],
  );
}

class _SheetHeader extends StatelessWidget {
  const _SheetHeader({required this.title, this.onDone});

  final String title;
  final VoidCallback? onDone;

  @override
  Widget build(BuildContext context) => Row(
    children: <Widget>[
      Expanded(
        child: Text(title, style: Theme.of(context).textTheme.titleLarge),
      ),
      if (onDone != null)
        TextButton(onPressed: onDone, child: const Text('完成')),
    ],
  );
}

// ignore: unused_element
class _ScheduleHero extends StatelessWidget {
  const _ScheduleHero({
    required this.summary,
    required this.caption,
    required this.onTap,
  });

  final String summary;
  final String caption;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return GlassCard(
      radius: 24,
      onTap: onTap,
      padding: const EdgeInsets.fromLTRB(20, 17, 18, 19),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Row(
            children: <Widget>[
              Container(
                width: 34,
                height: 34,
                decoration: BoxDecoration(
                  color: const Color(0xffe3f1ff),
                  borderRadius: BorderRadius.circular(11),
                ),
                child: const Icon(
                  Icons.schedule_rounded,
                  size: 19,
                  color: AppColors.blue,
                ),
              ),
              const SizedBox(width: 10),
              Text(
                '定时时间',
                style: Theme.of(
                  context,
                ).textTheme.labelLarge?.copyWith(color: AppColors.secondary),
              ),
              const Spacer(),
              Text(
                '修改',
                style: Theme.of(
                  context,
                ).textTheme.labelLarge?.copyWith(color: AppColors.blue),
              ),
              const SizedBox(width: 2),
              const Icon(
                Icons.chevron_right_rounded,
                color: AppColors.blue,
                size: 20,
              ),
            ],
          ),
          const SizedBox(height: 20),
          Center(
            child: Text(
              summary,
              textAlign: TextAlign.center,
              style: Theme.of(context).textTheme.headlineMedium?.copyWith(
                color: AppColors.ink,
                fontWeight: FontWeight.w800,
                letterSpacing: -0.7,
                height: 1.08,
              ),
            ),
          ),
          const SizedBox(height: 8),
          Center(
            child: Text(
              caption,
              textAlign: TextAlign.center,
              style: Theme.of(
                context,
              ).textTheme.bodyMedium?.copyWith(color: AppColors.secondary),
            ),
          ),
        ],
      ),
    );
  }
}

class _SectionLabel extends StatelessWidget {
  const _SectionLabel(this.text);

  final String text;

  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.only(left: 3),
    child: Text(
      text,
      style: Theme.of(context).textTheme.labelLarge?.copyWith(
        color: Theme.of(context).colorScheme.onSurfaceVariant,
        fontWeight: FontWeight.w600,
      ),
    ),
  );
}

class _LampTargetCard extends StatelessWidget {
  const _LampTargetCard({
    required this.networkController,
    required this.targetController,
    required this.selectedType,
    required this.onTypeSelected,
  });

  final TextEditingController networkController;
  final TextEditingController targetController;
  final LampTargetType selectedType;
  final ValueChanged<LampTargetType> onTypeSelected;

  @override
  Widget build(BuildContext context) {
    final visibleTypes = selectedType == LampTargetType.area
        ? const <LampTargetType>[
            LampTargetType.number,
            LampTargetType.group,
            LampTargetType.virtualNumber,
            LampTargetType.area,
          ]
        : const <LampTargetType>[
            LampTargetType.number,
            LampTargetType.group,
            LampTargetType.virtualNumber,
          ];
    return GlassCard(
      radius: 20,
      padding: const EdgeInsets.all(12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Row(
            crossAxisAlignment: CrossAxisAlignment.center,
            children: <Widget>[
              const Text('区', style: TextStyle(fontWeight: FontWeight.w700)),
              const SizedBox(width: 8),
              Expanded(
                flex: 3,
                child: TextField(
                  controller: networkController,
                  keyboardType: TextInputType.text,
                  textInputAction: selectedType == LampTargetType.area
                      ? TextInputAction.done
                      : TextInputAction.next,
                  inputFormatters: _addressInputFormatters,
                  decoration: const InputDecoration(
                    hintText: '区号',
                    isDense: true,
                  ),
                ),
              ),
              const SizedBox(width: 8),
              SizedBox(
                width: selectedType == LampTargetType.area ? 96 : 88,
                child: DropdownButtonFormField<LampTargetType>(
                  key: ValueKey<String>(
                    'lamp-target-type-${selectedType.wireName}',
                  ),
                  initialValue: selectedType,
                  isExpanded: true,
                  icon: const Icon(Icons.keyboard_arrow_down_rounded, size: 20),
                  decoration: const InputDecoration(
                    isDense: true,
                    contentPadding: EdgeInsets.fromLTRB(12, 12, 8, 12),
                  ),
                  items: visibleTypes
                      .map(
                        (type) => DropdownMenuItem<LampTargetType>(
                          value: type,
                          child: Text(
                            switch (type) {
                              LampTargetType.number => '号',
                              LampTargetType.virtualNumber => '标签',
                              LampTargetType.group => '组',
                              LampTargetType.area => '整区',
                            },
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                          ),
                        ),
                      )
                      .toList(growable: false),
                  onChanged: (type) {
                    if (type != null) onTypeSelected(type);
                  },
                ),
              ),
              if (selectedType != LampTargetType.area) ...<Widget>[
                const SizedBox(width: 8),
                Expanded(
                  flex: 3,
                  child: TextField(
                    controller: targetController,
                    keyboardType: TextInputType.text,
                    textInputAction: TextInputAction.done,
                    inputFormatters: _addressInputFormatters,
                    decoration: InputDecoration(
                      hintText: switch (selectedType) {
                        LampTargetType.number => '灯号',
                        LampTargetType.virtualNumber => '标签',
                        LampTargetType.group => '组号',
                        LampTargetType.area => '',
                      },
                      isDense: true,
                    ),
                  ),
                ),
              ],
            ],
          ),
          const SizedBox(height: 8),
          Text(
            switch (selectedType) {
              LampTargetType.number => '区号 0～65535 · 灯号 0～32767',
              LampTargetType.virtualNumber => '区号 0～65535 · 标签 0～16383',
              LampTargetType.group => '区号 0～65535 · 组号 0～16382',
              LampTargetType.area => '兼容已有整区任务',
            },
            style: Theme.of(context).textTheme.bodySmall?.copyWith(
              color: Theme.of(context).colorScheme.onSurfaceVariant,
            ),
          ),
        ],
      ),
    );
  }
}

class _TaskControlTestCard extends StatelessWidget {
  const _TaskControlTestCard({
    required this.sceneController,
    required this.lightTestEnabled,
    required this.sceneTestEnabled,
    required this.busy,
    required this.feedback,
    required this.feedbackIsError,
    required this.onLightOn,
    required this.onLightOff,
    required this.onFlash,
    required this.onReadScene,
    required this.onSetScene,
  });

  final TextEditingController sceneController;
  final bool lightTestEnabled;
  final bool sceneTestEnabled;
  final bool busy;
  final String feedback;
  final bool feedbackIsError;
  final VoidCallback onLightOn;
  final VoidCallback onLightOff;
  final VoidCallback onFlash;
  final VoidCallback onReadScene;
  final VoidCallback onSetScene;

  @override
  Widget build(BuildContext context) {
    final colors = Theme.of(context).colorScheme;
    return GlassCard(
      radius: 20,
      padding: const EdgeInsets.all(14),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Row(
            children: <Widget>[
              Container(
                width: 36,
                height: 36,
                decoration: BoxDecoration(
                  color: const Color(0xffe3f1ff),
                  borderRadius: BorderRadius.circular(11),
                ),
                child: const Icon(
                  Icons.science_outlined,
                  color: AppColors.blue,
                  size: 20,
                ),
              ),
              const SizedBox(width: 10),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: <Widget>[
                    Text(
                      '快速测试',
                      style: Theme.of(context).textTheme.titleSmall?.copyWith(
                        fontWeight: FontWeight.w800,
                      ),
                    ),
                    Text(
                      '使用上方目标，不会保存为定时任务',
                      style: Theme.of(context).textTheme.bodySmall,
                    ),
                  ],
                ),
              ),
            ],
          ),
          if (lightTestEnabled) ...<Widget>[
            const SizedBox(height: 14),
            Row(
              children: <Widget>[
                Expanded(
                  child: _CompactTestButton(
                    controlKey: const ValueKey<String>('task_test_light_on'),
                    label: '亮灯',
                    icon: Icons.lightbulb_rounded,
                    onPressed: busy ? null : onLightOn,
                  ),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: _CompactTestButton(
                    controlKey: const ValueKey<String>('task_test_light_off'),
                    label: '灭灯',
                    icon: Icons.lightbulb_outline_rounded,
                    onPressed: busy ? null : onLightOff,
                  ),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: _CompactTestButton(
                    controlKey: const ValueKey<String>('task_test_flash'),
                    label: '闪一下',
                    icon: Icons.bolt_rounded,
                    emphasized: true,
                    onPressed: busy ? null : onFlash,
                  ),
                ),
              ],
            ),
          ],
          if (sceneTestEnabled) ...<Widget>[
            const SizedBox(height: 14),
            Divider(height: 1, color: colors.outlineVariant),
            const SizedBox(height: 13),
            Row(
              children: <Widget>[
                Expanded(
                  child: Text(
                    '场景号',
                    style: Theme.of(context).textTheme.labelLarge?.copyWith(
                      fontWeight: FontWeight.w700,
                    ),
                  ),
                ),
                SizedBox(
                  width: 92,
                  child: TextField(
                    controller: sceneController,
                    enabled: !busy,
                    keyboardType: TextInputType.number,
                    textInputAction: TextInputAction.done,
                    textAlign: TextAlign.center,
                    inputFormatters: <TextInputFormatter>[
                      FilteringTextInputFormatter.digitsOnly,
                      LengthLimitingTextInputFormatter(2),
                    ],
                    decoration: const InputDecoration(
                      hintText: '1～16',
                      isDense: true,
                      contentPadding: EdgeInsets.symmetric(
                        horizontal: 10,
                        vertical: 11,
                      ),
                    ),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 10),
            Row(
              children: <Widget>[
                Expanded(
                  child: OutlinedButton.icon(
                    key: const ValueKey<String>('task_test_scene_read'),
                    onPressed: busy ? null : onReadScene,
                    icon: const Icon(Icons.download_rounded, size: 20),
                    label: const Text('读取场景', maxLines: 1, softWrap: false),
                    style: OutlinedButton.styleFrom(
                      minimumSize: const Size(0, 48),
                      padding: const EdgeInsets.symmetric(horizontal: 8),
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(15),
                      ),
                    ),
                  ),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: FilledButton.tonalIcon(
                    key: const ValueKey<String>('task_test_scene_set'),
                    onPressed: busy ? null : onSetScene,
                    icon: const Icon(Icons.upload_rounded, size: 20),
                    label: const Text('设置场景', maxLines: 1, softWrap: false),
                    style: FilledButton.styleFrom(
                      minimumSize: const Size(0, 48),
                      padding: const EdgeInsets.symmetric(horizontal: 8),
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(15),
                      ),
                    ),
                  ),
                ),
              ],
            ),
          ],
          const SizedBox(height: 12),
          AnimatedContainer(
            duration: const Duration(milliseconds: 180),
            width: double.infinity,
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
            decoration: BoxDecoration(
              color: feedbackIsError
                  ? colors.errorContainer
                  : const Color(0xffe8f5ff),
              borderRadius: BorderRadius.circular(14),
            ),
            child: Row(
              children: <Widget>[
                if (busy)
                  const Padding(
                    padding: EdgeInsets.only(right: 9),
                    child: SizedBox.square(
                      dimension: 17,
                      child: CircularProgressIndicator(strokeWidth: 2),
                    ),
                  )
                else
                  Padding(
                    padding: const EdgeInsets.only(right: 9),
                    child: Icon(
                      feedbackIsError
                          ? Icons.error_outline_rounded
                          : Icons.info_outline_rounded,
                      size: 19,
                      color: feedbackIsError
                          ? colors.onErrorContainer
                          : AppColors.blueDeep,
                    ),
                  ),
                Expanded(
                  child: Text(
                    feedback,
                    style: Theme.of(context).textTheme.bodySmall?.copyWith(
                      color: feedbackIsError
                          ? colors.onErrorContainer
                          : AppColors.blueDeep,
                    ),
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _CompactTestButton extends StatelessWidget {
  const _CompactTestButton({
    required this.controlKey,
    required this.label,
    required this.icon,
    required this.onPressed,
    this.emphasized = false,
  });

  final Key controlKey;
  final String label;
  final IconData icon;
  final VoidCallback? onPressed;
  final bool emphasized;

  @override
  Widget build(BuildContext context) {
    final style = FilledButton.styleFrom(
      minimumSize: const Size(0, 48),
      padding: const EdgeInsets.symmetric(horizontal: 5),
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(15)),
    );
    final labelWidget = Text(label, maxLines: 1, softWrap: false);
    return emphasized
        ? FilledButton.icon(
            key: controlKey,
            onPressed: onPressed,
            icon: Icon(icon, size: 19),
            label: labelWidget,
            style: style,
          )
        : FilledButton.tonalIcon(
            key: controlKey,
            onPressed: onPressed,
            icon: Icon(icon, size: 19),
            label: labelWidget,
            style: style,
          );
  }
}

class _SceneNumberRow extends StatelessWidget {
  const _SceneNumberRow({required this.controller});

  final TextEditingController controller;

  @override
  Widget build(BuildContext context) => ListTile(
    contentPadding: const EdgeInsets.fromLTRB(18, 7, 16, 7),
    leading: const _SettingIcon(icon: Icons.tag_rounded),
    title: const Text('场景号'),
    trailing: SizedBox(
      width: 88,
      child: TextField(
        controller: controller,
        keyboardType: TextInputType.number,
        textInputAction: TextInputAction.done,
        textAlign: TextAlign.center,
        inputFormatters: <TextInputFormatter>[
          FilteringTextInputFormatter.digitsOnly,
          LengthLimitingTextInputFormatter(2),
        ],
        decoration: const InputDecoration(
          hintText: '1～16',
          isDense: true,
          contentPadding: EdgeInsets.symmetric(horizontal: 10, vertical: 11),
        ),
      ),
    ),
  );
}

class _SettingIcon extends StatelessWidget {
  const _SettingIcon({required this.icon});

  final IconData icon;

  @override
  Widget build(BuildContext context) => Container(
    width: 36,
    height: 36,
    decoration: BoxDecoration(
      color: Theme.of(context).colorScheme.primaryContainer,
      borderRadius: BorderRadius.circular(11),
    ),
    child: Icon(
      icon,
      size: 20,
      color: Theme.of(context).colorScheme.onPrimaryContainer,
    ),
  );
}

class _InlineNotice extends StatelessWidget {
  const _InlineNotice({required this.icon, required this.text});

  final IconData icon;
  final String text;

  @override
  Widget build(BuildContext context) {
    final colors = Theme.of(context).colorScheme;
    return DecoratedBox(
      decoration: BoxDecoration(
        color: colors.secondaryContainer.withValues(alpha: 0.6),
        borderRadius: BorderRadius.circular(16),
      ),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
        child: Row(
          children: <Widget>[
            Icon(icon, size: 19, color: colors.onSecondaryContainer),
            const SizedBox(width: 10),
            Expanded(
              child: Text(
                text,
                style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                  color: colors.onSecondaryContainer,
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _SheetHint extends StatelessWidget {
  const _SheetHint({required this.text});

  final String text;

  @override
  Widget build(BuildContext context) => Row(
    crossAxisAlignment: CrossAxisAlignment.start,
    children: <Widget>[
      Icon(
        Icons.info_outline_rounded,
        size: 18,
        color: Theme.of(context).colorScheme.onSurfaceVariant,
      ),
      const SizedBox(width: 8),
      Expanded(
        child: Text(
          text,
          style: Theme.of(context).textTheme.bodyMedium?.copyWith(
            color: Theme.of(context).colorScheme.onSurfaceVariant,
          ),
        ),
      ),
    ],
  );
}

String _actionLabel(TimerActionType type) => switch (type) {
  TimerActionType.lightOn => '亮灯',
  TimerActionType.lightOff => '灭灯',
  TimerActionType.sceneSwitch => '设置场景',
};

IconData _actionIcon(TimerActionType type) => switch (type) {
  TimerActionType.lightOn => Icons.lightbulb_rounded,
  TimerActionType.lightOff => Icons.lightbulb_outline_rounded,
  TimerActionType.sceneSwitch => Icons.auto_awesome_rounded,
};

final List<TextInputFormatter> _addressInputFormatters = <TextInputFormatter>[
  FilteringTextInputFormatter.allow(RegExp('[0-9a-fA-FxX]')),
  LengthLimitingTextInputFormatter(6),
];

int? _parseFlexibleInt(String text) {
  final value = text.trim();
  if (value.isEmpty) return null;
  return int.tryParse(
    value.substring(value.startsWith(RegExp('0[xX]')) ? 2 : 0),
    radix: value.startsWith(RegExp('0[xX]')) ? 16 : 10,
  );
}

String _formatTargetInput(LampTarget target) {
  final value = target.value;
  if (value == null) return '';
  return value.toString();
}
