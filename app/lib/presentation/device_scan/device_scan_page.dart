import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:yx_gateway_timer/data/ble/ble_scanner.dart';
import 'package:yx_gateway_timer/data/repositories/gateway_repository.dart';
import 'package:yx_gateway_timer/domain/gateway_device.dart';
import 'package:yx_gateway_timer/domain/protocol_error.dart';
import 'package:yx_gateway_timer/presentation/design/app_design.dart';
import 'package:yx_gateway_timer/presentation/device_detail/device_detail_page.dart';
import 'package:yx_gateway_timer/presentation/providers.dart';

class DeviceScanPage extends ConsumerWidget {
  const DeviceScanPage({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final state = ref.watch(gatewayControllerProvider);
    final controller = ref.read(gatewayControllerProvider.notifier);
    final scanning = state.phase == GatewayConnectionPhase.scanning;
    return Scaffold(
      body: AppBackdrop(
        child: SafeArea(
          child: RefreshIndicator(
            onRefresh: controller.scan,
            child: ListView(
              physics: const AlwaysScrollableScrollPhysics(),
              padding: const EdgeInsets.fromLTRB(20, 24, 20, 36),
              children: <Widget>[
                PageHeading(
                  eyebrow: '轻时 Timer',
                  title: '发现设备',
                  trailing: GlassIconButton(
                    icon: Icons.refresh_rounded,
                    tooltip: '重新扫描',
                    onPressed: _canScan(state) ? controller.scan : null,
                  ),
                ),
                const SizedBox(height: 28),
                Center(
                  child: _ScanOrb(
                    scanning: scanning,
                    enabled: _canScan(state),
                    onTap: controller.scan,
                  ),
                ),
                const SizedBox(height: 18),
                Text(
                  scanning
                      ? '正在发现附近的蓝牙设备…'
                      : state.devices.isEmpty
                      ? '点击上方按钮开始扫描'
                      : '发现 ${state.devices.length} 台设备 · 点击设备连接',
                  textAlign: TextAlign.center,
                  style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                    color: AppColors.secondary,
                    fontWeight: FontWeight.w600,
                  ),
                ),
                if (_showHostBanner(state)) ...<Widget>[
                  const SizedBox(height: 22),
                  _HostBanner(state: state),
                ],
                const SizedBox(height: 30),
                SectionHeading(
                  title: '附近设备',
                  action: state.hostState == BleHostState.ready
                      ? '蓝牙已开启'
                      : null,
                ),
                const SizedBox(height: 10),
                if (state.devices.isEmpty)
                  _EmptyState(state: state)
                else
                  for (final device in state.devices) ...<Widget>[
                    _GatewayRow(device: device, state: state),
                    const SizedBox(height: 12),
                  ],
                const SizedBox(height: 12),
                Row(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: <Widget>[
                    const Icon(
                      Icons.lock_outline_rounded,
                      size: 15,
                      color: AppColors.tertiary,
                    ),
                    const SizedBox(width: 6),
                    Text(
                      '设备信息仅保存在当前手机',
                      style: Theme.of(context).textTheme.bodySmall?.copyWith(
                        color: AppColors.tertiary,
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  bool _canScan(GatewayRepositoryState state) =>
      state.currentDeviceId == null &&
      state.phase != GatewayConnectionPhase.connecting &&
      state.phase != GatewayConnectionPhase.disconnecting;

  bool _showHostBanner(GatewayRepositoryState state) =>
      state.phase == GatewayConnectionPhase.permissionRequired ||
      state.phase == GatewayConnectionPhase.bluetoothOff ||
      state.phase == GatewayConnectionPhase.error ||
      state.phase == GatewayConnectionPhase.reconnecting;
}

class _ScanOrb extends StatelessWidget {
  const _ScanOrb({
    required this.scanning,
    required this.enabled,
    required this.onTap,
  });

  final bool scanning;
  final bool enabled;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) => SizedBox.square(
    dimension: 190,
    child: Stack(
      alignment: Alignment.center,
      children: <Widget>[
        for (final size in <double>[190, 154])
          AnimatedContainer(
            duration: const Duration(milliseconds: 500),
            width: scanning ? size : size - 8,
            height: scanning ? size : size - 8,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              border: Border.all(
                color: AppColors.blue.withValues(
                  alpha: size == 190 ? 0.09 : 0.14,
                ),
              ),
            ),
          ),
        DecoratedBox(
          decoration: const BoxDecoration(
            shape: BoxShape.circle,
            gradient: LinearGradient(
              begin: Alignment.topLeft,
              end: Alignment.bottomRight,
              colors: <Color>[
                Color(0xff56b8ff),
                AppColors.blue,
                AppColors.blueDeep,
              ],
            ),
            boxShadow: <BoxShadow>[
              BoxShadow(
                color: Color(0x4d1687f8),
                blurRadius: 32,
                offset: Offset(0, 14),
              ),
              BoxShadow(
                color: Color(0x2effffff),
                blurRadius: 8,
                offset: Offset(-4, -4),
              ),
            ],
          ),
          child: Material(
            color: Colors.transparent,
            shape: const CircleBorder(),
            clipBehavior: Clip.antiAlias,
            child: InkWell(
              customBorder: const CircleBorder(),
              onTap: enabled ? onTap : null,
              child: const SizedBox.square(
                dimension: 112,
                child: Icon(
                  Icons.bluetooth_searching_rounded,
                  color: Colors.white,
                  size: 50,
                ),
              ),
            ),
          ),
        ),
        if (scanning)
          const SizedBox.square(
            dimension: 132,
            child: CircularProgressIndicator(
              color: Color(0x99ffffff),
              strokeWidth: 2,
            ),
          ),
      ],
    ),
  );
}

class _HostBanner extends ConsumerWidget {
  const _HostBanner({required this.state});

  final GatewayRepositoryState state;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final controller = ref.read(gatewayControllerProvider.notifier);
    final (icon, title, detail) = switch (state.phase) {
      GatewayConnectionPhase.permissionRequired => (
        Icons.admin_panel_settings_outlined,
        state.hostState == BleHostState.locationServicesOff
            ? '请开启定位服务'
            : '需要蓝牙权限',
        state.permissionPermanentlyDenied
            ? '请到系统设置中允许附近设备权限。'
            : state.hostState == BleHostState.locationServicesOff
            ? 'Android 11 及以下扫描蓝牙需要定位服务。'
            : '允许扫描和连接附近的网关。',
      ),
      GatewayConnectionPhase.bluetoothOff => (
        Icons.bluetooth_disabled_rounded,
        '蓝牙已关闭',
        '开启蓝牙后即可搜索附近设备。',
      ),
      GatewayConnectionPhase.reconnecting => (
        Icons.sync_rounded,
        '正在重新连接',
        '信号恢复后将自动继续。',
      ),
      _ => (
        Icons.error_outline_rounded,
        '连接未完成',
        state.errorMessage ?? '请确认设备在附近后重试。',
      ),
    };
    final canAct = state.phase != GatewayConnectionPhase.reconnecting;
    return GlassCard(
      color: const Color(0xf2fff4e8),
      borderColor: const Color(0x55ff9f3e),
      padding: const EdgeInsets.fromLTRB(15, 13, 8, 13),
      child: Row(
        children: <Widget>[
          Container(
            width: 38,
            height: 38,
            decoration: BoxDecoration(
              color: AppColors.orange.withValues(alpha: 0.13),
              borderRadius: BorderRadius.circular(12),
            ),
            child: Icon(icon, size: 21, color: AppColors.orange),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text(title, style: Theme.of(context).textTheme.titleSmall),
                const SizedBox(height: 2),
                Text(detail, style: Theme.of(context).textTheme.bodySmall),
              ],
            ),
          ),
          if (canAct)
            TextButton(
              onPressed: state.permissionPermanentlyDenied
                  ? controller.openSettings
                  : controller.scan,
              child: Text(state.permissionPermanentlyDenied ? '设置' : '重试'),
            ),
        ],
      ),
    );
  }
}

class _EmptyState extends StatelessWidget {
  const _EmptyState({required this.state});

  final GatewayRepositoryState state;

  @override
  Widget build(BuildContext context) {
    final scanning = state.phase == GatewayConnectionPhase.scanning;
    final blocked =
        state.phase == GatewayConnectionPhase.permissionRequired ||
        state.phase == GatewayConnectionPhase.bluetoothOff ||
        state.phase == GatewayConnectionPhase.error;
    return GlassCard(
      padding: const EdgeInsets.fromLTRB(20, 19, 20, 20),
      child: Row(
        children: <Widget>[
          Container(
            width: 48,
            height: 48,
            decoration: BoxDecoration(
              color: const Color(0xffe3f1ff),
              borderRadius: BorderRadius.circular(15),
            ),
            child: scanning
                ? const Padding(
                    padding: EdgeInsets.all(14),
                    child: CircularProgressIndicator(strokeWidth: 2.2),
                  )
                : Icon(
                    blocked
                        ? Icons.portable_wifi_off_rounded
                        : Icons.bluetooth_searching_rounded,
                    color: AppColors.blue,
                  ),
          ),
          const SizedBox(width: 14),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text(
                  scanning
                      ? '正在搜索附近设备'
                      : blocked
                      ? '等待系统状态恢复'
                      : '还没有发现网关',
                  style: Theme.of(context).textTheme.titleSmall,
                ),
                const SizedBox(height: 4),
                Text(
                  scanning ? '通常几秒钟即可出现' : '请确认网关已上电并在手机附近',
                  style: Theme.of(context).textTheme.bodySmall,
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _GatewayRow extends ConsumerWidget {
  const _GatewayRow({required this.device, required this.state});

  final GatewayDevice device;
  final GatewayRepositoryState state;

  bool get _busy => switch (device.phase) {
    GatewayConnectionPhase.connecting ||
    GatewayConnectionPhase.discovering ||
    GatewayConnectionPhase.pairing ||
    GatewayConnectionPhase.subscribing ||
    GatewayConnectionPhase.syncingTime ||
    GatewayConnectionPhase.loadingTasks ||
    GatewayConnectionPhase.reconnecting ||
    GatewayConnectionPhase.disconnecting => true,
    _ => false,
  };

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final colors = Theme.of(context).colorScheme;
    final isCurrent = state.currentDeviceId == device.deviceId;
    final statusColor = device.phase == GatewayConnectionPhase.error
        ? colors.error
        : device.isConnected
        ? colors.primary
        : colors.onSurfaceVariant;
    return GlassCard(
      onTap: device.isConnected ? () => _openDetails(context) : null,
      padding: const EdgeInsets.fromLTRB(14, 13, 8, 13),
      child: Row(
        children: <Widget>[
          Container(
            width: 50,
            height: 50,
            decoration: BoxDecoration(
              gradient: device.isConnected
                  ? const LinearGradient(
                      begin: Alignment.topLeft,
                      end: Alignment.bottomRight,
                      colors: <Color>[AppColors.cyan, AppColors.blue],
                    )
                  : null,
              color: device.isConnected ? null : const Color(0xffe8f3fd),
              borderRadius: BorderRadius.circular(16),
            ),
            child: Icon(
              Icons.bluetooth_rounded,
              color: device.isConnected ? Colors.white : AppColors.blue,
              size: 27,
            ),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text(
                  device.displayName,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: Theme.of(context).textTheme.titleSmall,
                ),
                const SizedBox(height: 5),
                Row(
                  children: <Widget>[
                    Container(
                      width: 6,
                      height: 6,
                      decoration: BoxDecoration(
                        color: statusColor,
                        shape: BoxShape.circle,
                      ),
                    ),
                    const SizedBox(width: 6),
                    Flexible(
                      child: Text(
                        '${_phaseLabel(device.phase)} · ${device.rssi} dBm',
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: Theme.of(
                          context,
                        ).textTheme.bodySmall?.copyWith(color: statusColor),
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
          _SignalIcon(bars: device.signalBars),
          if (device.isConnected && device.supportsIdentify)
            IconButton(
              tooltip: '闪灯找设备',
              visualDensity: VisualDensity.compact,
              onPressed: state.identifyingDevice
                  ? null
                  : () => _identify(context, ref),
              icon: state.identifyingDevice
                  ? const SizedBox.square(
                      dimension: 18,
                      child: CircularProgressIndicator(strokeWidth: 2),
                    )
                  : const Icon(Icons.flashlight_on_rounded, size: 20),
            ),
          IconButton(
            tooltip: '编辑本地备注',
            visualDensity: VisualDensity.compact,
            onPressed: () => _editRemark(context, ref),
            icon: const Icon(Icons.edit_outlined, size: 20),
          ),
          FilledButton(
            style: FilledButton.styleFrom(
              minimumSize: const Size(58, 40),
              padding: const EdgeInsets.symmetric(horizontal: 13),
              shape: RoundedRectangleBorder(
                borderRadius: BorderRadius.circular(13),
              ),
            ),
            onPressed: _busy || (!isCurrent && state.currentDeviceId != null)
                ? null
                : () => _primaryAction(context, ref),
            child: _busy
                ? const SizedBox.square(
                    dimension: 17,
                    child: CircularProgressIndicator(
                      strokeWidth: 2,
                      color: Colors.white,
                    ),
                  )
                : Text(device.isConnected ? '查看' : '连接'),
          ),
        ],
      ),
    );
  }

  Future<void> _primaryAction(BuildContext context, WidgetRef ref) async {
    if (device.isConnected) {
      _openDetails(context);
      return;
    }
    try {
      await ref
          .read(gatewayControllerProvider.notifier)
          .connect(device.deviceId);
      if (context.mounted) _openDetails(context);
    } on Object catch (error) {
      if (context.mounted) _showError(context, error);
    }
  }

  void _openDetails(BuildContext context) {
    Navigator.of(context).push(
      MaterialPageRoute<void>(
        builder: (_) => DeviceDetailPage(deviceId: device.deviceId),
      ),
    );
  }

  Future<void> _editRemark(BuildContext context, WidgetRef ref) async {
    var draft = device.remark;
    final value = await showDialog<String>(
      context: context,
      builder: (dialogContext) => AlertDialog(
        title: const Text('本地备注'),
        content: TextFormField(
          initialValue: device.remark,
          maxLength: 64,
          autofocus: true,
          onChanged: (value) => draft = value,
          decoration: const InputDecoration(
            labelText: '仅保存在当前手机',
            hintText: '例如：一楼配电间',
          ),
        ),
        actions: <Widget>[
          TextButton(
            onPressed: () => Navigator.pop(dialogContext),
            child: const Text('取消'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(dialogContext, draft),
            child: const Text('保存'),
          ),
        ],
      ),
    );
    if (value != null) {
      await ref
          .read(gatewayControllerProvider.notifier)
          .saveRemark(device.deviceId, value);
    }
  }

  Future<void> _identify(BuildContext context, WidgetRef ref) async {
    try {
      final result = await ref
          .read(gatewayControllerProvider.notifier)
          .identifyDevice();
      if (context.mounted) {
        HapticFeedback.lightImpact();
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('板载识别灯已闪烁 ${result.durationMs} ms')),
        );
      }
    } on Object catch (error) {
      if (context.mounted) _showError(context, error);
    }
  }
}

class _SignalIcon extends StatelessWidget {
  const _SignalIcon({required this.bars});

  final int bars;

  @override
  Widget build(BuildContext context) => Icon(
    switch (bars) {
      4 => Icons.signal_cellular_alt_rounded,
      3 => Icons.signal_cellular_alt_2_bar_rounded,
      _ => Icons.signal_cellular_alt_1_bar_rounded,
    },
    size: 20,
    color: AppColors.blue,
  );
}

String _phaseLabel(GatewayConnectionPhase phase) => switch (phase) {
  GatewayConnectionPhase.connecting => '正在连接',
  GatewayConnectionPhase.discovering => '正在准备',
  GatewayConnectionPhase.pairing => '正在配对',
  GatewayConnectionPhase.subscribing => '正在订阅',
  GatewayConnectionPhase.syncingTime => '正在校时',
  GatewayConnectionPhase.loadingTasks => '正在读取任务',
  GatewayConnectionPhase.ready => '已连接',
  GatewayConnectionPhase.reconnecting => '正在重连',
  GatewayConnectionPhase.disconnecting => '正在断开',
  GatewayConnectionPhase.error => '连接失败',
  _ => '可连接',
};

void _showError(BuildContext context, Object error) {
  final message = error is ProtocolException
      ? error.message
      : error.toString().replaceFirst('Exception: ', '');
  ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(message)));
}
