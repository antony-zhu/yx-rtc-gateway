enum GatewayConnectionPhase {
  idle,
  permissionRequired,
  bluetoothOff,
  scanning,
  connecting,
  discovering,
  pairing,
  subscribing,
  syncingTime,
  loadingTasks,
  ready,
  reconnecting,
  disconnecting,
  error,
}

class GatewayDevice {
  const GatewayDevice({
    required this.deviceId,
    required this.peripheralId,
    required this.rssi,
    required this.advertisedName,
    this.remark = '',
    this.alias = '',
    this.firmware,
    this.maxTasks = 16,
    this.taskCount = 0,
    this.utcTrusted = false,
    this.tzOffsetMin,
    this.lastSeen,
    this.lastTimeSync,
    this.phase = GatewayConnectionPhase.idle,
    this.features = const <String>{},
  });

  final String deviceId;
  final String peripheralId;
  final int rssi;
  final String advertisedName;
  final String remark;
  final String alias;
  final String? firmware;
  final int maxTasks;
  final int taskCount;
  final bool utcTrusted;
  final int? tzOffsetMin;
  final DateTime? lastSeen;
  final DateTime? lastTimeSync;
  final GatewayConnectionPhase phase;
  final Set<String> features;

  String get displayName {
    if (remark.trim().isNotEmpty) return remark.trim();
    if (alias.trim().isNotEmpty) return alias.trim();
    if (advertisedName.trim().isNotEmpty) return advertisedName.trim();
    return 'YX Gateway';
  }

  String get shortId => deviceId.length <= 4
      ? deviceId
      : '…${deviceId.substring(deviceId.length - 4)}';

  int get signalBars {
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -78) return 2;
    return 1;
  }

  bool get isConnected => phase == GatewayConnectionPhase.ready;
  bool get supportsIdentify => features.contains('device_identify');
  bool get supportsSceneTest => features.contains('gs200_scene_test');
  bool get supportsLightTest => features.contains('gs200_light_test');

  GatewayDevice copyWith({
    String? deviceId,
    String? peripheralId,
    int? rssi,
    String? advertisedName,
    String? remark,
    String? alias,
    String? firmware,
    int? maxTasks,
    int? taskCount,
    bool? utcTrusted,
    int? tzOffsetMin,
    DateTime? lastSeen,
    DateTime? lastTimeSync,
    GatewayConnectionPhase? phase,
    Set<String>? features,
  }) {
    return GatewayDevice(
      deviceId: deviceId ?? this.deviceId,
      peripheralId: peripheralId ?? this.peripheralId,
      rssi: rssi ?? this.rssi,
      advertisedName: advertisedName ?? this.advertisedName,
      remark: remark ?? this.remark,
      alias: alias ?? this.alias,
      firmware: firmware ?? this.firmware,
      maxTasks: maxTasks ?? this.maxTasks,
      taskCount: taskCount ?? this.taskCount,
      utcTrusted: utcTrusted ?? this.utcTrusted,
      tzOffsetMin: tzOffsetMin ?? this.tzOffsetMin,
      lastSeen: lastSeen ?? this.lastSeen,
      lastTimeSync: lastTimeSync ?? this.lastTimeSync,
      phase: phase ?? this.phase,
      features: features ?? this.features,
    );
  }
}
