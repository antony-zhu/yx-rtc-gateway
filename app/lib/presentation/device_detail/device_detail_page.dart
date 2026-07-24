import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:yx_gateway_timer/data/ble/yx_gatt_client.dart';
import 'package:yx_gateway_timer/domain/gateway_device.dart';
import 'package:yx_gateway_timer/domain/protocol_error.dart';
import 'package:yx_gateway_timer/domain/timer_task.dart';
import 'package:yx_gateway_timer/presentation/design/app_design.dart';
import 'package:yx_gateway_timer/presentation/providers.dart';
import 'package:yx_gateway_timer/presentation/task_editor/task_editor_page.dart';

class DeviceDetailPage extends ConsumerWidget {
  const DeviceDetailPage({required this.deviceId, super.key});

  final String deviceId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final state = ref.watch(gatewayControllerProvider);
    final device = state.devices
        .where((item) => item.deviceId == deviceId)
        .firstOrNull;
    if (device == null) {
      return Scaffold(
        body: AppBackdrop(
          child: SafeArea(
            child: Column(
              children: <Widget>[
                Align(
                  alignment: Alignment.centerLeft,
                  child: Padding(
                    padding: const EdgeInsets.all(20),
                    child: GlassIconButton(
                      icon: Icons.arrow_back_ios_new_rounded,
                      tooltip: '返回',
                      onPressed: () => Navigator.maybePop(context),
                    ),
                  ),
                ),
                const Expanded(child: Center(child: Text('设备记录不存在'))),
              ],
            ),
          ),
        ),
      );
    }

    final ready = device.phase == GatewayConnectionPhase.ready;
    final canAdd = ready && state.tasks.length < device.maxTasks;
    return DefaultTabController(
      length: 2,
      child: Scaffold(
        floatingActionButtonLocation: FloatingActionButtonLocation.centerFloat,
        floatingActionButton: canAdd && state.tasks.isNotEmpty
            ? _TaskFloatingButton(
                onPressed: () =>
                    _createTask(context, device.utcTrusted, device.features),
              )
            : null,
        body: AppBackdrop(
          child: SafeArea(
            child: Column(
              children: <Widget>[
                Padding(
                  padding: const EdgeInsets.fromLTRB(20, 18, 20, 0),
                  child: _DeviceHeader(
                    device: device,
                    ready: ready,
                    onBack: () => Navigator.maybePop(context),
                    onMore: () => _showDeviceSheet(context, ref, device),
                  ),
                ),
                Padding(
                  padding: const EdgeInsets.fromLTRB(20, 18, 20, 0),
                  child: _ConnectionStrip(device: device, ready: ready),
                ),
                if (!ready)
                  Padding(
                    padding: const EdgeInsets.fromLTRB(20, 10, 20, 0),
                    child: _ConnectionProgress(device: device),
                  ),
                Padding(
                  padding: const EdgeInsets.fromLTRB(20, 16, 20, 0),
                  child: Container(
                    height: 44,
                    padding: const EdgeInsets.all(3),
                    decoration: BoxDecoration(
                      color: const Color(0xb8ffffff),
                      borderRadius: BorderRadius.circular(15),
                      border: Border.all(color: Colors.white),
                    ),
                    child: const TabBar(
                      dividerHeight: 0,
                      indicatorSize: TabBarIndicatorSize.tab,
                      indicator: BoxDecoration(
                        color: Colors.white,
                        borderRadius: BorderRadius.all(Radius.circular(12)),
                        boxShadow: <BoxShadow>[
                          BoxShadow(
                            color: Color(0x14223446),
                            blurRadius: 8,
                            offset: Offset(0, 2),
                          ),
                        ],
                      ),
                      tabs: <Widget>[
                        Tab(text: '任务'),
                        Tab(text: '记录'),
                      ],
                    ),
                  ),
                ),
                Expanded(
                  child: TabBarView(
                    children: <Widget>[
                      _TaskList(
                        tasks: state.tasks,
                        maxTasks: device.maxTasks,
                        ready: ready,
                        timeTrusted: device.utcTrusted,
                        supportedFeatures: device.features,
                        busyTaskIds: state.busyTaskIds,
                        onCreate: canAdd
                            ? () => _createTask(
                                context,
                                device.utcTrusted,
                                device.features,
                              )
                            : null,
                      ),
                      _EventList(events: state.events),
                    ],
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Future<void> _createTask(
    BuildContext context,
    bool timeTrusted,
    Set<String> supportedFeatures,
  ) async {
    await Navigator.of(context).push<TimerTask>(
      MaterialPageRoute<TimerTask>(
        builder: (_) => TaskEditorPage(
          initialTask: TimerTask(
            enabled: timeTrusted,
            name: '',
            type: TimerType.daily,
            schedule: const DailySchedule(hour: 7, minute: 30),
            action: const TimerAction(
              type: TimerActionType.lightOn,
              target: LampTarget(
                networkId: 0,
                type: LampTargetType.number,
                value: 0,
              ),
            ),
          ),
          timeTrusted: timeTrusted,
          supportedFeatures: supportedFeatures,
        ),
      ),
    );
  }

  Future<void> _showDeviceSheet(
    BuildContext context,
    WidgetRef ref,
    GatewayDevice device,
  ) async {
    final ready = device.phase == GatewayConnectionPhase.ready;
    var openDeviceTest = false;
    await showModalBottomSheet<void>(
      context: context,
      showDragHandle: true,
      useSafeArea: true,
      isScrollControlled: true,
      builder: (sheetContext) => _DeviceSheet(
        device: device,
        identifying: ref.read(gatewayControllerProvider).identifyingDevice,
        onIdentify: device.supportsIdentify && ready
            ? () {
                Navigator.pop(sheetContext);
                _identify(context, ref);
              }
            : null,
        onSyncTime: ready
            ? () {
                Navigator.pop(sheetContext);
                _syncTime(context, ref);
              }
            : null,
        onEditAlias: ready
            ? () {
                Navigator.pop(sheetContext);
                _editAlias(context, ref, device.alias);
              }
            : null,
        onDeviceTest:
            (device.supportsLightTest || device.supportsSceneTest) && ready
            ? () {
                openDeviceTest = true;
                Navigator.pop(sheetContext);
              }
            : null,
        onDisconnect: () {
          Navigator.pop(sheetContext);
          _disconnect(context, ref);
        },
      ),
    );
    if (openDeviceTest && context.mounted) {
      await _showDeviceTestSheet(context, device);
    }
  }

  Future<void> _showDeviceTestSheet(
    BuildContext context,
    GatewayDevice device,
  ) => showModalBottomSheet<void>(
    context: context,
    useSafeArea: true,
    isScrollControlled: true,
    builder: (_) => _DeviceTestSheet(device: device),
  );

  Future<void> _syncTime(BuildContext context, WidgetRef ref) async {
    try {
      await ref.read(gatewayControllerProvider.notifier).syncTime();
      if (context.mounted) {
        HapticFeedback.lightImpact();
        _message(context, '时间同步成功');
      }
    } on Object catch (error) {
      if (context.mounted) _error(context, error);
    }
  }

  Future<void> _editAlias(
    BuildContext context,
    WidgetRef ref,
    String current,
  ) async {
    var draft = current;
    var bytes = utf8.encode(current).length;
    final value = await showDialog<String>(
      context: context,
      builder: (dialogContext) => StatefulBuilder(
        builder: (context, setState) => AlertDialog(
          title: const Text('设备名称'),
          content: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              TextFormField(
                initialValue: current,
                onChanged: (value) => setState(() {
                  draft = value;
                  bytes = utf8.encode(value).length;
                }),
                decoration: const InputDecoration(
                  labelText: '共享名称',
                  hintText: '留空可清除',
                ),
              ),
              const SizedBox(height: 8),
              Text(
                '$bytes / 31 UTF-8 字节',
                style: TextStyle(
                  color: bytes > 31
                      ? Theme.of(context).colorScheme.error
                      : Theme.of(context).colorScheme.onSurfaceVariant,
                ),
              ),
            ],
          ),
          actions: <Widget>[
            TextButton(
              onPressed: () => Navigator.pop(dialogContext),
              child: const Text('取消'),
            ),
            FilledButton(
              onPressed: bytes <= 31
                  ? () => Navigator.pop(dialogContext, draft)
                  : null,
              child: const Text('保存'),
            ),
          ],
        ),
      ),
    );
    if (value == null) return;
    try {
      await ref.read(gatewayControllerProvider.notifier).setAlias(value);
      if (context.mounted) _message(context, '设备名称已更新');
    } on Object catch (error) {
      if (context.mounted) _error(context, error);
    }
  }

  Future<void> _disconnect(BuildContext context, WidgetRef ref) async {
    await ref.read(gatewayControllerProvider.notifier).disconnect();
    if (context.mounted) Navigator.of(context).pop();
  }

  Future<void> _identify(BuildContext context, WidgetRef ref) async {
    try {
      final result = await ref
          .read(gatewayControllerProvider.notifier)
          .identifyDevice();
      if (context.mounted) {
        HapticFeedback.lightImpact();
        _message(context, '板载识别灯已闪烁 ${result.durationMs} ms');
      }
    } on Object catch (error) {
      if (context.mounted) _error(context, error);
    }
  }
}

class _DeviceHeader extends StatelessWidget {
  const _DeviceHeader({
    required this.device,
    required this.ready,
    required this.onBack,
    required this.onMore,
  });

  final GatewayDevice device;
  final bool ready;
  final VoidCallback onBack;
  final VoidCallback onMore;

  @override
  Widget build(BuildContext context) => Row(
    children: <Widget>[
      GlassIconButton(
        icon: Icons.arrow_back_ios_new_rounded,
        tooltip: '返回',
        onPressed: onBack,
      ),
      const SizedBox(width: 14),
      Expanded(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Text(
              '我的设备',
              style: Theme.of(context).textTheme.labelSmall?.copyWith(
                color: AppColors.blue,
                fontWeight: FontWeight.w800,
                letterSpacing: 1.6,
              ),
            ),
            const SizedBox(height: 4),
            Text('定时任务', style: Theme.of(context).textTheme.headlineMedium),
          ],
        ),
      ),
      Stack(
        clipBehavior: Clip.none,
        children: <Widget>[
          GlassIconButton(
            icon: Icons.more_horiz_rounded,
            tooltip: '设备信息与操作',
            onPressed: onMore,
          ),
          Positioned(
            right: 1,
            top: 1,
            child: Container(
              width: 10,
              height: 10,
              decoration: BoxDecoration(
                color: ready ? AppColors.green : AppColors.tertiary,
                shape: BoxShape.circle,
                border: Border.all(color: Colors.white, width: 2),
              ),
            ),
          ),
        ],
      ),
    ],
  );
}

class _ConnectionStrip extends StatelessWidget {
  const _ConnectionStrip({required this.device, required this.ready});

  final GatewayDevice device;
  final bool ready;

  @override
  Widget build(BuildContext context) => GlassCard(
    radius: 17,
    padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 11),
    child: Row(
      children: <Widget>[
        Container(
          width: 34,
          height: 34,
          decoration: BoxDecoration(
            color: const Color(0xffe3f1ff),
            borderRadius: BorderRadius.circular(11),
          ),
          child: const Icon(
            Icons.bluetooth_connected_rounded,
            color: AppColors.blue,
            size: 19,
          ),
        ),
        const SizedBox(width: 10),
        Expanded(
          child: Text(
            device.displayName,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: Theme.of(context).textTheme.labelLarge,
          ),
        ),
        Container(
          width: 7,
          height: 7,
          decoration: BoxDecoration(
            color: ready ? AppColors.green : AppColors.orange,
            shape: BoxShape.circle,
          ),
        ),
        const SizedBox(width: 6),
        Text(
          ready
              ? device.utcTrusted
                    ? '已同步'
                    : '已连接'
              : '准备中',
          style: Theme.of(context).textTheme.bodySmall?.copyWith(
            color: ready ? AppColors.green : AppColors.orange,
            fontWeight: FontWeight.w700,
          ),
        ),
      ],
    ),
  );
}

class _TaskFloatingButton extends StatelessWidget {
  const _TaskFloatingButton({required this.onPressed});

  final VoidCallback onPressed;

  @override
  Widget build(BuildContext context) => DecoratedBox(
    decoration: BoxDecoration(
      gradient: const LinearGradient(
        colors: <Color>[AppColors.blue, AppColors.blueDeep],
      ),
      borderRadius: BorderRadius.circular(22),
      boxShadow: const <BoxShadow>[
        BoxShadow(
          color: Color(0x421687f8),
          blurRadius: 22,
          offset: Offset(0, 10),
        ),
      ],
    ),
    child: FloatingActionButton.extended(
      heroTag: 'new-timer-task',
      elevation: 0,
      backgroundColor: Colors.transparent,
      foregroundColor: Colors.white,
      onPressed: onPressed,
      icon: const Icon(Icons.add_rounded),
      label: const Text('新建任务', style: TextStyle(fontWeight: FontWeight.w700)),
    ),
  );
}

class _TaskList extends StatelessWidget {
  const _TaskList({
    required this.tasks,
    required this.maxTasks,
    required this.ready,
    required this.timeTrusted,
    required this.supportedFeatures,
    required this.busyTaskIds,
    required this.onCreate,
  });

  final List<TimerTaskSummary> tasks;
  final int maxTasks;
  final bool ready;
  final bool timeTrusted;
  final Set<String> supportedFeatures;
  final Set<int> busyTaskIds;
  final VoidCallback? onCreate;

  @override
  Widget build(BuildContext context) {
    if (!ready) {
      return const _EmptyState(
        icon: Icons.lock_clock_outlined,
        title: '正在准备设备',
        message: '连接、配对和时间同步完成后即可管理任务。',
      );
    }
    if (tasks.isEmpty) {
      return _EmptyState(
        icon: Icons.schedule_rounded,
        title: '还没有定时任务',
        message: '创建一个任务，让设备在指定时间自动执行。',
        action: FilledButton.icon(
          onPressed: onCreate,
          icon: const Icon(Icons.add_rounded),
          label: const Text('新建任务'),
        ),
      );
    }

    final nextTask = tasks.where((task) => task.enabled).firstOrNull;
    return ListView(
      padding: const EdgeInsets.fromLTRB(20, 18, 20, 104),
      children: <Widget>[
        _NextTaskHero(task: nextTask),
        const SizedBox(height: 24),
        SectionHeading(title: '全部任务', action: '${tasks.length} / $maxTasks'),
        const SizedBox(height: 8),
        GlassCard(
          child: Column(
            children: <Widget>[
              for (var index = 0; index < tasks.length; index++) ...<Widget>[
                if (index > 0)
                  const Divider(height: 1, indent: 68, endIndent: 16),
                _TaskRow(
                  task: tasks[index],
                  busy: busyTaskIds.contains(tasks[index].taskId),
                  timeTrusted: timeTrusted,
                  supportedFeatures: supportedFeatures,
                ),
              ],
            ],
          ),
        ),
      ],
    );
  }
}

class _NextTaskHero extends StatelessWidget {
  const _NextTaskHero({required this.task});

  final TimerTaskSummary? task;

  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.fromLTRB(20, 18, 18, 19),
    decoration: BoxDecoration(
      gradient: const LinearGradient(
        begin: Alignment.topLeft,
        end: Alignment.bottomRight,
        colors: <Color>[Color(0xff42a7ff), AppColors.blue, AppColors.blueDeep],
        stops: <double>[0, 0.52, 1],
      ),
      borderRadius: BorderRadius.circular(25),
      boxShadow: const <BoxShadow>[
        BoxShadow(
          color: Color(0x3d1687f8),
          blurRadius: 25,
          offset: Offset(0, 12),
        ),
      ],
    ),
    child: Row(
      children: <Widget>[
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              Text(
                task == null ? '当前没有运行中的任务' : '下一个任务',
                style: Theme.of(context).textTheme.labelLarge?.copyWith(
                  color: Colors.white.withValues(alpha: 0.78),
                ),
              ),
              const SizedBox(height: 10),
              Text(
                task == null
                    ? '全部已暂停'
                    : task!.name.isEmpty
                    ? '未命名任务'
                    : task!.name,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: Theme.of(context).textTheme.headlineMedium?.copyWith(
                  color: Colors.white,
                  fontWeight: FontWeight.w800,
                ),
              ),
              const SizedBox(height: 7),
              Text(
                task == null ? '打开任一任务即可重新启用' : _taskTypeLabel(task!.type),
                style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                  color: Colors.white.withValues(alpha: 0.82),
                ),
              ),
            ],
          ),
        ),
        const SizedBox(width: 12),
        Container(
          width: 52,
          height: 52,
          decoration: BoxDecoration(
            color: Colors.white.withValues(alpha: 0.16),
            shape: BoxShape.circle,
            border: Border.all(color: Colors.white.withValues(alpha: 0.2)),
          ),
          child: Icon(
            task == null ? Icons.pause_rounded : Icons.schedule_rounded,
            color: Colors.white,
            size: 25,
          ),
        ),
      ],
    ),
  );
}

class _TaskRow extends ConsumerWidget {
  const _TaskRow({
    required this.task,
    required this.busy,
    required this.timeTrusted,
    required this.supportedFeatures,
  });

  final TimerTaskSummary task;
  final bool busy;
  final bool timeTrusted;
  final Set<String> supportedFeatures;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    return InkWell(
      onTap: busy ? null : () => _edit(context, ref),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(13, 11, 4, 11),
        child: Row(
          children: <Widget>[
            _TaskIcon(type: task.type, enabled: task.enabled),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: <Widget>[
                  Text(
                    task.name.isEmpty ? '未命名任务' : task.name,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: Theme.of(context).textTheme.titleSmall?.copyWith(
                      color: task.enabled ? AppColors.ink : AppColors.secondary,
                    ),
                  ),
                  const SizedBox(height: 3),
                  Text(
                    task.target == null
                        ? _taskTypeLabel(task.type)
                        : '${_taskTypeLabel(task.type)} · ${task.target!.displayLabel}',
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: Theme.of(
                      context,
                    ).textTheme.bodySmall?.copyWith(color: AppColors.secondary),
                  ),
                ],
              ),
            ),
            if (busy)
              const Padding(
                padding: EdgeInsets.symmetric(horizontal: 18),
                child: SizedBox.square(
                  dimension: 20,
                  child: CircularProgressIndicator(strokeWidth: 2),
                ),
              )
            else ...<Widget>[
              Switch(
                value: task.enabled,
                onChanged: !timeTrusted && !task.enabled
                    ? null
                    : (value) => _toggle(context, ref, value),
              ),
              IconButton(
                tooltip: '删除任务',
                visualDensity: VisualDensity.compact,
                color: AppColors.red,
                onPressed: () => _delete(context, ref),
                icon: const Icon(Icons.delete_outline_rounded, size: 21),
              ),
            ],
          ],
        ),
      ),
    );
  }

  Future<void> _edit(BuildContext context, WidgetRef ref) async {
    try {
      final full = await ref
          .read(gatewayControllerProvider.notifier)
          .getTask(task.taskId);
      if (!context.mounted) return;
      await Navigator.of(context).push<TimerTask>(
        MaterialPageRoute<TimerTask>(
          builder: (_) => TaskEditorPage(
            initialTask: full,
            timeTrusted: timeTrusted,
            supportedFeatures: supportedFeatures,
            onDelete: () =>
                ref.read(gatewayControllerProvider.notifier).deleteTask(task),
          ),
        ),
      );
    } on Object catch (error) {
      if (context.mounted) _error(context, error);
    }
  }

  Future<void> _toggle(BuildContext context, WidgetRef ref, bool value) async {
    HapticFeedback.selectionClick();
    try {
      await ref
          .read(gatewayControllerProvider.notifier)
          .setEnabled(task, value);
    } on ProtocolException catch (error) {
      if (context.mounted) {
        _error(
          context,
          error.isRevisionConflict ? '设备配置已变化，已重新读取，请重新确认' : error,
        );
      }
    } on Object catch (error) {
      if (context.mounted) _error(context, error);
    }
  }

  Future<void> _delete(BuildContext context, WidgetRef ref) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (dialogContext) => AlertDialog(
        title: const Text('删除任务？'),
        content: Text('“${task.name}”删除后无法恢复。'),
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
    if (confirmed != true) return;
    try {
      await ref.read(gatewayControllerProvider.notifier).deleteTask(task);
      HapticFeedback.mediumImpact();
    } on ProtocolException catch (error) {
      if (context.mounted) {
        _error(
          context,
          error.isRevisionConflict ? '设备配置已变化，已重新读取，请重新确认' : error,
        );
      }
    } on Object catch (error) {
      if (context.mounted) _error(context, error);
    }
  }
}

class _TaskIcon extends StatelessWidget {
  const _TaskIcon({required this.type, required this.enabled});

  final TimerType type;
  final bool enabled;

  @override
  Widget build(BuildContext context) {
    final color = switch (type) {
      TimerType.duration => AppColors.orange,
      TimerType.daily => AppColors.blue,
      TimerType.interval => AppColors.green,
    };
    return Container(
      width: 42,
      height: 42,
      decoration: BoxDecoration(
        color: enabled
            ? color.withValues(alpha: 0.13)
            : const Color(0xffedf1f4),
        borderRadius: BorderRadius.circular(13),
      ),
      child: Icon(
        switch (type) {
          TimerType.duration => Icons.timer_outlined,
          TimerType.daily => Icons.today_outlined,
          TimerType.interval => Icons.repeat_rounded,
        },
        size: 21,
        color: enabled ? color : AppColors.tertiary,
      ),
    );
  }
}

class _EventList extends StatelessWidget {
  const _EventList({required this.events});

  final List<TimerFiredEvent> events;

  @override
  Widget build(BuildContext context) {
    if (events.isEmpty) {
      return const _EmptyState(
        icon: Icons.history_rounded,
        title: '暂无执行记录',
        message: '设备触发任务后，结果会显示在这里。',
      );
    }
    return ListView(
      padding: const EdgeInsets.fromLTRB(20, 18, 20, 36),
      children: <Widget>[
        Padding(
          padding: const EdgeInsets.fromLTRB(4, 0, 4, 10),
          child: Text('本次连接', style: Theme.of(context).textTheme.titleMedium),
        ),
        GlassCard(
          child: Column(
            children: <Widget>[
              for (var index = 0; index < events.length; index++) ...<Widget>[
                if (index > 0)
                  const Divider(height: 1, indent: 60, endIndent: 16),
                _EventTile(event: events[index]),
              ],
            ],
          ),
        ),
      ],
    );
  }
}

class _EventTile extends StatelessWidget {
  const _EventTile({required this.event});

  final TimerFiredEvent event;

  @override
  Widget build(BuildContext context) => ListTile(
    contentPadding: const EdgeInsets.symmetric(horizontal: 16, vertical: 4),
    leading: Icon(
      event.success ? Icons.check_circle_rounded : Icons.error_rounded,
      color: event.success
          ? Theme.of(context).colorScheme.primary
          : Theme.of(context).colorScheme.error,
    ),
    title: Text('任务 #${event.taskId} ${event.success ? '执行成功' : '执行失败'}'),
    subtitle: Text(
      '${_formatDateTime(event.receivedAt)} · 第 ${event.occurrenceId} 次'
      '${event.success ? '' : ' · ESP_ERR ${event.espError}'}',
    ),
  );
}

class _DeviceSheet extends StatelessWidget {
  const _DeviceSheet({
    required this.device,
    required this.identifying,
    required this.onIdentify,
    required this.onSyncTime,
    required this.onEditAlias,
    required this.onDeviceTest,
    required this.onDisconnect,
  });

  final GatewayDevice device;
  final bool identifying;
  final VoidCallback? onIdentify;
  final VoidCallback? onSyncTime;
  final VoidCallback? onEditAlias;
  final VoidCallback? onDeviceTest;
  final VoidCallback onDisconnect;

  @override
  Widget build(BuildContext context) {
    final colors = Theme.of(context).colorScheme;
    return SingleChildScrollView(
      padding: const EdgeInsets.fromLTRB(16, 0, 16, 24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Padding(
            padding: const EdgeInsets.fromLTRB(4, 0, 4, 16),
            child: Row(
              children: <Widget>[
                Container(
                  width: 44,
                  height: 44,
                  decoration: BoxDecoration(
                    color: colors.primaryContainer,
                    borderRadius: BorderRadius.circular(14),
                  ),
                  child: Icon(
                    Icons.router_rounded,
                    color: colors.onPrimaryContainer,
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      Text(
                        device.displayName,
                        style: Theme.of(context).textTheme.titleMedium,
                      ),
                      const SizedBox(height: 2),
                      Text(
                        '${device.deviceId} · ${device.isConnected ? '已连接' : '未连接'}',
                        style: Theme.of(context).textTheme.bodySmall?.copyWith(
                          color: colors.onSurfaceVariant,
                        ),
                      ),
                    ],
                  ),
                ),
              ],
            ),
          ),
          Card(
            clipBehavior: Clip.antiAlias,
            child: Column(
              children: <Widget>[
                _InfoRow(label: '固件', value: device.firmware ?? '—'),
                const Divider(height: 1, indent: 16),
                _InfoRow(
                  label: '时间',
                  value: device.utcTrusted ? '已同步' : '等待同步',
                ),
                const Divider(height: 1, indent: 16),
                _InfoRow(
                  label: '时区',
                  value: device.tzOffsetMin == null
                      ? '—'
                      : _formatOffset(device.tzOffsetMin!),
                ),
                const Divider(height: 1, indent: 16),
                _InfoRow(
                  label: '上次校时',
                  value: device.lastTimeSync == null
                      ? '—'
                      : _formatDateTime(device.lastTimeSync!),
                ),
              ],
            ),
          ),
          const SizedBox(height: 14),
          Card(
            clipBehavior: Clip.antiAlias,
            child: Column(
              children: <Widget>[
                if (device.supportsIdentify) ...<Widget>[
                  ListTile(
                    leading: const Icon(Icons.flashlight_on_rounded),
                    title: Text(identifying ? '正在闪灯…' : '闪灯找设备'),
                    trailing: identifying
                        ? const SizedBox.square(
                            dimension: 20,
                            child: CircularProgressIndicator(strokeWidth: 2),
                          )
                        : const Icon(Icons.chevron_right_rounded),
                    onTap: identifying ? null : onIdentify,
                  ),
                  const Divider(height: 1, indent: 56),
                ],
                ListTile(
                  leading: const Icon(Icons.schedule_send_rounded),
                  title: const Text('立即校时'),
                  trailing: const Icon(Icons.chevron_right_rounded),
                  onTap: onSyncTime,
                ),
                const Divider(height: 1, indent: 56),
                ListTile(
                  leading: const Icon(Icons.badge_outlined),
                  title: const Text('修改设备名称'),
                  trailing: const Icon(Icons.chevron_right_rounded),
                  onTap: onEditAlias,
                ),
                if (device.supportsLightTest ||
                    device.supportsSceneTest) ...<Widget>[
                  const Divider(height: 1, indent: 56),
                  ListTile(
                    leading: const Icon(Icons.science_outlined),
                    title: const Text('设备测试'),
                    subtitle: const Text('灯具亮、灭、闪与场景号'),
                    trailing: const Icon(Icons.chevron_right_rounded),
                    onTap: onDeviceTest,
                  ),
                ],
              ],
            ),
          ),
          const SizedBox(height: 14),
          Card(
            clipBehavior: Clip.antiAlias,
            child: ListTile(
              textColor: colors.error,
              iconColor: colors.error,
              leading: const Icon(Icons.link_off_rounded),
              title: const Text('断开连接'),
              onTap: onDisconnect,
            ),
          ),
        ],
      ),
    );
  }
}

class _DeviceTestSheet extends ConsumerStatefulWidget {
  const _DeviceTestSheet({required this.device});

  final GatewayDevice device;

  @override
  ConsumerState<_DeviceTestSheet> createState() => _DeviceTestSheetState();
}

class _DeviceTestSheetState extends ConsumerState<_DeviceTestSheet> {
  final TextEditingController _networkController = TextEditingController(
    text: '0',
  );
  final TextEditingController _targetController = TextEditingController(
    text: '0',
  );
  final TextEditingController _sceneController = TextEditingController(
    text: '1',
  );
  LampTargetType _targetType = LampTargetType.number;
  int? _currentScene;
  String _feedback = '尚未执行测试指令';
  bool _feedbackIsError = false;

  @override
  void dispose() {
    _networkController.dispose();
    _targetController.dispose();
    _sceneController.dispose();
    super.dispose();
  }

  LampTarget? _readLampTarget() {
    final networkId = int.tryParse(_networkController.text.trim());
    final value = int.tryParse(_targetController.text.trim());
    if (networkId == null || value == null) {
      setState(() {
        _feedback = '请输入有效的区号和目标编号';
        _feedbackIsError = true;
      });
      return null;
    }
    final target = LampTarget(
      networkId: networkId,
      type: _targetType,
      value: value,
    );
    final errors = target.validate();
    if (errors.isNotEmpty) {
      setState(() {
        _feedback = errors.first;
        _feedbackIsError = true;
      });
      return null;
    }
    return target;
  }

  Future<void> _testLamp(LampTestAction action) async {
    final target = _readLampTarget();
    if (target == null) return;
    FocusManager.instance.primaryFocus?.unfocus();
    try {
      final result = await ref
          .read(gatewayControllerProvider.notifier)
          .testLamp(action, target);
      if (!mounted) return;
      HapticFeedback.lightImpact();
      setState(() {
        _feedback =
            '${result.action.label}指令已写入网关（${result.target.displayLabel}）。'
            '灯具控制无 ACK，请直接观察灯具。';
        _feedbackIsError = false;
      });
    } on Object catch (error) {
      _showError(error);
    }
  }

  Future<void> _flashBoard() async {
    try {
      final result = await ref
          .read(gatewayControllerProvider.notifier)
          .identifyDevice();
      if (!mounted) return;
      HapticFeedback.lightImpact();
      setState(() {
        _feedback = '板载指示灯已闪烁 ${result.durationMs} ms';
        _feedbackIsError = false;
      });
    } on Object catch (error) {
      _showError(error);
    }
  }

  Future<void> _setScene() async {
    final sceneNo = int.tryParse(_sceneController.text.trim());
    if (sceneNo == null || sceneNo < 1 || sceneNo > 16) {
      setState(() {
        _feedback = '请输入 1～16 之间的场景号';
        _feedbackIsError = true;
      });
      return;
    }
    FocusManager.instance.primaryFocus?.unfocus();
    try {
      final result = await ref
          .read(gatewayControllerProvider.notifier)
          .setCurrentScene(sceneNo);
      if (!mounted) return;
      HapticFeedback.lightImpact();
      setState(() {
        _feedback = '场景号 ${result.sceneNo} 设置指令已发送，请读取确认';
        _feedbackIsError = false;
      });
    } on Object catch (error) {
      _showError(error);
    }
  }

  Future<void> _readScene() async {
    FocusManager.instance.primaryFocus?.unfocus();
    try {
      final result = await ref
          .read(gatewayControllerProvider.notifier)
          .readCurrentScene();
      if (!mounted) return;
      HapticFeedback.lightImpact();
      setState(() {
        _currentScene = result.sceneNo;
        _sceneController.text = result.sceneNo.toString();
        _feedback = '已读取灯具返回的当前场景号';
        _feedbackIsError = false;
      });
    } on Object catch (error) {
      _showError(error);
    }
  }

  void _showError(Object error) {
    if (!mounted) return;
    setState(() {
      _feedback = error is ProtocolException ? error.message : error.toString();
      _feedbackIsError = true;
    });
  }

  @override
  Widget build(BuildContext context) {
    final state = ref.watch(gatewayControllerProvider);
    final busy =
        state.identifyingDevice ||
        state.lampCommandBusy ||
        state.sceneCommandBusy;
    final colors = Theme.of(context).colorScheme;
    return Padding(
      padding: EdgeInsets.fromLTRB(
        20,
        0,
        20,
        24 + MediaQuery.viewInsetsOf(context).bottom,
      ),
      child: SingleChildScrollView(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            Text('设备测试', style: Theme.of(context).textTheme.titleLarge),
            const SizedBox(height: 4),
            Text(
              widget.device.displayName,
              style: Theme.of(context).textTheme.bodySmall,
            ),
            const SizedBox(height: 18),
            if (widget.device.supportsLightTest) ...<Widget>[
              Card(
                child: Padding(
                  padding: const EdgeInsets.all(18),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      Text(
                        '灯具控制测试',
                        style: Theme.of(context).textTheme.titleMedium,
                      ),
                      const SizedBox(height: 4),
                      Text(
                        'APP 发送区和目标，网关组装最终指令并下发。',
                        style: Theme.of(context).textTheme.bodySmall,
                      ),
                      const SizedBox(height: 16),
                      Row(
                        crossAxisAlignment: CrossAxisAlignment.center,
                        children: <Widget>[
                          const Text(
                            '区',
                            style: TextStyle(fontWeight: FontWeight.w700),
                          ),
                          const SizedBox(width: 8),
                          Expanded(
                            flex: 3,
                            child: TextField(
                              controller: _networkController,
                              enabled: !busy,
                              keyboardType: TextInputType.number,
                              textInputAction: TextInputAction.next,
                              inputFormatters: <TextInputFormatter>[
                                FilteringTextInputFormatter.digitsOnly,
                                LengthLimitingTextInputFormatter(5),
                              ],
                              decoration: const InputDecoration(
                                hintText: '区号',
                                isDense: true,
                              ),
                            ),
                          ),
                          const SizedBox(width: 8),
                          SizedBox(
                            width: 88,
                            child: DropdownButtonFormField<LampTargetType>(
                              key: ValueKey<String>(
                                'test-target-${_targetType.wireName}',
                              ),
                              initialValue: _targetType,
                              isExpanded: true,
                              icon: const Icon(
                                Icons.keyboard_arrow_down_rounded,
                                size: 20,
                              ),
                              decoration: const InputDecoration(
                                isDense: true,
                                contentPadding: EdgeInsets.fromLTRB(
                                  12,
                                  12,
                                  8,
                                  12,
                                ),
                              ),
                              items:
                                  const <LampTargetType>[
                                        LampTargetType.number,
                                        LampTargetType.group,
                                        LampTargetType.virtualNumber,
                                      ]
                                      .map(
                                        (type) =>
                                            DropdownMenuItem<LampTargetType>(
                                              value: type,
                                              child: Text(switch (type) {
                                                LampTargetType.number => '号',
                                                LampTargetType.group => '组',
                                                LampTargetType.virtualNumber =>
                                                  '标签',
                                                LampTargetType.area => '整区',
                                              }, maxLines: 1),
                                            ),
                                      )
                                      .toList(growable: false),
                              onChanged: busy
                                  ? null
                                  : (type) {
                                      if (type == null) return;
                                      setState(() => _targetType = type);
                                    },
                            ),
                          ),
                          const SizedBox(width: 8),
                          Expanded(
                            flex: 3,
                            child: TextField(
                              controller: _targetController,
                              enabled: !busy,
                              keyboardType: TextInputType.number,
                              textInputAction: TextInputAction.done,
                              inputFormatters: <TextInputFormatter>[
                                FilteringTextInputFormatter.digitsOnly,
                                LengthLimitingTextInputFormatter(5),
                              ],
                              decoration: InputDecoration(
                                hintText: switch (_targetType) {
                                  LampTargetType.number => '灯号',
                                  LampTargetType.group => '组号',
                                  LampTargetType.virtualNumber => '标签',
                                  LampTargetType.area => '',
                                },
                                isDense: true,
                              ),
                            ),
                          ),
                        ],
                      ),
                      const SizedBox(height: 16),
                      Row(
                        children: <Widget>[
                          Expanded(
                            child: FilledButton.tonalIcon(
                              onPressed: busy
                                  ? null
                                  : () => _testLamp(LampTestAction.lightOn),
                              icon: const Icon(Icons.lightbulb_rounded),
                              label: const Text(
                                '亮灯',
                                maxLines: 1,
                                softWrap: false,
                              ),
                              style: FilledButton.styleFrom(
                                padding: const EdgeInsets.symmetric(
                                  horizontal: 6,
                                ),
                              ),
                            ),
                          ),
                          const SizedBox(width: 8),
                          Expanded(
                            child: FilledButton.tonalIcon(
                              onPressed: busy
                                  ? null
                                  : () => _testLamp(LampTestAction.lightOff),
                              icon: const Icon(Icons.lightbulb_outline_rounded),
                              label: const Text(
                                '灭灯',
                                maxLines: 1,
                                softWrap: false,
                              ),
                              style: FilledButton.styleFrom(
                                padding: const EdgeInsets.symmetric(
                                  horizontal: 6,
                                ),
                              ),
                            ),
                          ),
                          const SizedBox(width: 8),
                          Expanded(
                            child: FilledButton.icon(
                              onPressed: busy
                                  ? null
                                  : () => _testLamp(LampTestAction.flash),
                              icon: const Icon(Icons.bolt_rounded),
                              label: const Text(
                                '闪一下',
                                maxLines: 1,
                                softWrap: false,
                              ),
                              style: FilledButton.styleFrom(
                                padding: const EdgeInsets.symmetric(
                                  horizontal: 6,
                                ),
                              ),
                            ),
                          ),
                        ],
                      ),
                    ],
                  ),
                ),
              ),
              const SizedBox(height: 14),
            ],
            if (widget.device.supportsIdentify) ...<Widget>[
              Card(
                clipBehavior: Clip.antiAlias,
                child: ListTile(
                  contentPadding: const EdgeInsets.symmetric(
                    horizontal: 16,
                    vertical: 5,
                  ),
                  leading: Container(
                    width: 42,
                    height: 42,
                    decoration: BoxDecoration(
                      color: colors.primaryContainer,
                      borderRadius: BorderRadius.circular(13),
                    ),
                    child: Icon(
                      Icons.flashlight_on_rounded,
                      color: colors.onPrimaryContainer,
                    ),
                  ),
                  title: const Text('闪板载指示灯'),
                  subtitle: const Text('用于确认当前连接的是这块网关板'),
                  trailing: state.identifyingDevice
                      ? const SizedBox.square(
                          dimension: 20,
                          child: CircularProgressIndicator(strokeWidth: 2),
                        )
                      : const Icon(Icons.chevron_right_rounded),
                  onTap: busy ? null : _flashBoard,
                ),
              ),
              const SizedBox(height: 14),
            ],
            if (widget.device.supportsSceneTest) ...<Widget>[
              Card(
                child: Padding(
                  padding: const EdgeInsets.all(18),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      Text(
                        '灯具场景号',
                        style: Theme.of(context).textTheme.titleMedium,
                      ),
                      const SizedBox(height: 4),
                      Text(
                        '设置指令不等待 ACK；读取会显示灯具实际返回值。',
                        style: Theme.of(context).textTheme.bodySmall,
                      ),
                      const SizedBox(height: 18),
                      Row(
                        crossAxisAlignment: CrossAxisAlignment.end,
                        children: <Widget>[
                          Expanded(
                            child: TextField(
                              controller: _sceneController,
                              enabled: !busy,
                              keyboardType: TextInputType.number,
                              inputFormatters: <TextInputFormatter>[
                                FilteringTextInputFormatter.digitsOnly,
                                LengthLimitingTextInputFormatter(2),
                              ],
                              decoration: const InputDecoration(
                                labelText: '要设置的场景号',
                                hintText: '1～16',
                                counterText: '',
                              ),
                            ),
                          ),
                          const SizedBox(width: 14),
                          Container(
                            width: 88,
                            height: 58,
                            decoration: BoxDecoration(
                              color: colors.primaryContainer,
                              borderRadius: BorderRadius.circular(16),
                            ),
                            child: Column(
                              mainAxisAlignment: MainAxisAlignment.center,
                              children: <Widget>[
                                Text(
                                  '当前',
                                  style: Theme.of(context).textTheme.bodySmall,
                                ),
                                Text(
                                  _currentScene?.toString() ?? '—',
                                  style: Theme.of(context).textTheme.titleLarge
                                      ?.copyWith(
                                        color: colors.onPrimaryContainer,
                                      ),
                                ),
                              ],
                            ),
                          ),
                        ],
                      ),
                      const SizedBox(height: 14),
                      Row(
                        children: <Widget>[
                          Expanded(
                            child: OutlinedButton.icon(
                              onPressed: busy ? null : _readScene,
                              icon: const Icon(Icons.download_rounded),
                              label: const Text('读取场景'),
                              style: OutlinedButton.styleFrom(
                                minimumSize: const Size(0, 50),
                                shape: RoundedRectangleBorder(
                                  borderRadius: BorderRadius.circular(16),
                                ),
                              ),
                            ),
                          ),
                          const SizedBox(width: 12),
                          Expanded(
                            child: FilledButton.icon(
                              onPressed: busy ? null : _setScene,
                              icon: const Icon(Icons.upload_rounded),
                              label: const Text('设置场景'),
                            ),
                          ),
                        ],
                      ),
                    ],
                  ),
                ),
              ),
              const SizedBox(height: 14),
            ],
            AnimatedContainer(
              duration: const Duration(milliseconds: 180),
              width: double.infinity,
              padding: const EdgeInsets.symmetric(horizontal: 15, vertical: 13),
              decoration: BoxDecoration(
                color: _feedbackIsError
                    ? colors.errorContainer
                    : colors.secondaryContainer,
                borderRadius: BorderRadius.circular(16),
              ),
              child: Row(
                children: <Widget>[
                  if (state.sceneCommandBusy || state.lampCommandBusy)
                    const Padding(
                      padding: EdgeInsets.only(right: 10),
                      child: SizedBox.square(
                        dimension: 18,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      ),
                    )
                  else
                    Padding(
                      padding: const EdgeInsets.only(right: 10),
                      child: Icon(
                        _feedbackIsError
                            ? Icons.error_outline_rounded
                            : Icons.info_outline_rounded,
                        size: 20,
                      ),
                    ),
                  Expanded(
                    child: Text(
                      state.sceneCommandBusy
                          ? '正在等待灯具返回…'
                          : state.lampCommandBusy
                          ? '正在把灯具指令写入网关…'
                          : _feedback,
                      style: Theme.of(context).textTheme.bodySmall?.copyWith(
                        color: _feedbackIsError
                            ? colors.onErrorContainer
                            : colors.onSecondaryContainer,
                      ),
                    ),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _InfoRow extends StatelessWidget {
  const _InfoRow({required this.label, required this.value});

  final String label;
  final String value;

  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 13),
    child: Row(
      children: <Widget>[
        Text(label),
        const Spacer(),
        Flexible(
          child: Text(
            value,
            textAlign: TextAlign.end,
            style: TextStyle(
              color: Theme.of(context).colorScheme.onSurfaceVariant,
            ),
          ),
        ),
      ],
    ),
  );
}

class _ConnectionProgress extends StatelessWidget {
  const _ConnectionProgress({required this.device});

  final GatewayDevice device;

  @override
  Widget build(BuildContext context) {
    final text = switch (device.phase) {
      GatewayConnectionPhase.connecting => '正在连接…',
      GatewayConnectionPhase.discovering => '正在发现服务…',
      GatewayConnectionPhase.subscribing => '正在订阅设备通知…',
      GatewayConnectionPhase.pairing => '请完成系统配对…',
      GatewayConnectionPhase.syncingTime => '正在同步时间…',
      GatewayConnectionPhase.loadingTasks => '正在读取任务…',
      GatewayConnectionPhase.reconnecting => '连接中断，正在重连…',
      GatewayConnectionPhase.error => '连接初始化失败',
      _ => '设备未连接',
    };
    return _InlineStatus(text: text);
  }
}

class _InlineStatus extends StatelessWidget {
  const _InlineStatus({required this.text});

  final String text;

  @override
  Widget build(BuildContext context) => DecoratedBox(
    decoration: BoxDecoration(
      color: Theme.of(context).colorScheme.surfaceContainer,
      borderRadius: BorderRadius.circular(14),
    ),
    child: Padding(
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 11),
      child: Row(
        children: <Widget>[
          const SizedBox.square(
            dimension: 17,
            child: CircularProgressIndicator(strokeWidth: 2),
          ),
          const SizedBox(width: 10),
          Expanded(child: Text(text)),
        ],
      ),
    ),
  );
}

class _EmptyState extends StatelessWidget {
  const _EmptyState({
    required this.icon,
    required this.title,
    required this.message,
    this.action,
  });

  final IconData icon;
  final String title;
  final String message;
  final Widget? action;

  @override
  Widget build(BuildContext context) => Center(
    child: SingleChildScrollView(
      padding: const EdgeInsets.all(32),
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 320),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            Container(
              width: 64,
              height: 64,
              decoration: BoxDecoration(
                color: Theme.of(context).colorScheme.primaryContainer,
                borderRadius: BorderRadius.circular(20),
              ),
              child: Icon(
                icon,
                size: 30,
                color: Theme.of(context).colorScheme.onPrimaryContainer,
              ),
            ),
            const SizedBox(height: 18),
            Text(
              title,
              style: Theme.of(context).textTheme.titleLarge,
              textAlign: TextAlign.center,
            ),
            const SizedBox(height: 6),
            Text(
              message,
              textAlign: TextAlign.center,
              style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                color: Theme.of(context).colorScheme.onSurfaceVariant,
              ),
            ),
            if (action != null) ...<Widget>[
              const SizedBox(height: 20),
              action!,
            ],
          ],
        ),
      ),
    ),
  );
}

String _taskTypeLabel(TimerType type) => switch (type) {
  TimerType.duration => '单次倒计时',
  TimerType.daily => '每天执行',
  TimerType.interval => '按分钟或小时循环',
};

String _formatDateTime(DateTime value) {
  String two(int value) => value.toString().padLeft(2, '0');
  final local = value.toLocal();
  return '${local.year}-${two(local.month)}-${two(local.day)} '
      '${two(local.hour)}:${two(local.minute)}:${two(local.second)}';
}

String _formatOffset(int minutes) {
  final sign = minutes >= 0 ? '+' : '-';
  final absolute = minutes.abs();
  return 'UTC$sign${(absolute ~/ 60).toString().padLeft(2, '0')}:'
      '${(absolute % 60).toString().padLeft(2, '0')}';
}

void _message(BuildContext context, String message) {
  ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(message)));
}

void _error(BuildContext context, Object error) {
  final text = error is ProtocolException ? error.message : error.toString();
  ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(text)));
}
