import 'dart:async';
import 'dart:io';

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:yx_gateway_timer/core/yx_constants.dart';

enum BleHostState {
  unknown,
  ready,
  unauthorized,
  poweredOff,
  unsupported,
  resetting,
  locationServicesOff,
}

class ScannedGateway {
  const ScannedGateway({
    required this.deviceId,
    required this.peripheralId,
    required this.name,
    required this.rssi,
  });

  final String deviceId;
  final String peripheralId;
  final String name;
  final int rssi;
}

class BlePermissionResult {
  const BlePermissionResult({
    required this.granted,
    this.permanentlyDenied = false,
    this.locationServicesOff = false,
  });

  final bool granted;
  final bool permanentlyDenied;
  final bool locationServicesOff;
}

abstract interface class BleScanner {
  Stream<BleHostState> get hostStateStream;
  BleHostState get hostState;
  Future<BlePermissionResult> ensurePermissions();
  Stream<ScannedGateway> scan();
  Future<void> openSettings();
}

class ReactiveBleScanner implements BleScanner {
  ReactiveBleScanner(this._ble);

  final FlutterReactiveBle _ble;

  @override
  Stream<BleHostState> get hostStateStream =>
      _ble.statusStream.map(_mapBleStatus).distinct();

  @override
  BleHostState get hostState => _mapBleStatus(_ble.status);

  @override
  Future<BlePermissionResult> ensurePermissions() async {
    if (Platform.isAndroid) {
      final sdk = _androidSdkVersion();
      final permissions = sdk >= 31
          ? const <Permission>[
              Permission.bluetoothScan,
              Permission.bluetoothConnect,
            ]
          : const <Permission>[Permission.locationWhenInUse];
      final statuses = await permissions.request();
      final permanentlyDenied = statuses.values.any(
        (status) => status.isPermanentlyDenied,
      );
      final granted = statuses.values.every((status) => status.isGranted);
      if (!granted) {
        return BlePermissionResult(
          granted: false,
          permanentlyDenied: permanentlyDenied,
        );
      }
      if (sdk < 31) {
        final locationStatus = await Permission.locationWhenInUse.serviceStatus;
        if (!locationStatus.isEnabled) {
          return const BlePermissionResult(
            granted: false,
            locationServicesOff: true,
          );
        }
      }
      return const BlePermissionResult(granted: true);
    }

    if (Platform.isIOS) {
      // CoreBluetooth 在首次初始化时显示系统授权框；最终状态以 BleStatus 为准。
      return const BlePermissionResult(granted: true);
    }
    return const BlePermissionResult(granted: false);
  }

  @override
  Stream<ScannedGateway> scan() {
    final requireLocation = Platform.isAndroid && _androidSdkVersion() < 31;
    return _ble
        .scanForDevices(
          withServices: <Uuid>[YxGatt.service],
          scanMode: ScanMode.lowLatency,
          requireLocationServicesEnabled: requireLocation,
        )
        .map((device) {
          final deviceId = ManufacturerDataParser.parseDeviceId(
            device.manufacturerData,
          );
          if (deviceId == null) return null;
          return ScannedGateway(
            deviceId: deviceId,
            peripheralId: device.id,
            name: device.name,
            rssi: device.rssi,
          );
        })
        .where((device) => device != null)
        .cast<ScannedGateway>();
  }

  @override
  Future<void> openSettings() async {
    await openAppSettings();
  }

  static BleHostState _mapBleStatus(BleStatus value) => switch (value) {
    BleStatus.ready => BleHostState.ready,
    BleStatus.unauthorized => BleHostState.unauthorized,
    BleStatus.poweredOff => BleHostState.poweredOff,
    BleStatus.unsupported => BleHostState.unsupported,
    BleStatus.locationServicesDisabled => BleHostState.locationServicesOff,
    _ => BleHostState.unknown,
  };

  static int _androidSdkVersion() {
    final match = RegExp(
      r'(?:SDK|API)\s*(\d+)',
      caseSensitive: false,
    ).firstMatch(Platform.operatingSystemVersion);
    return int.tryParse(match?.group(1) ?? '') ?? 31;
  }
}

abstract final class ManufacturerDataParser {
  static String? parseDeviceId(List<int> data) {
    if (data.length < 10) return null;
    final companyId = data[0] | (data[1] << 8);
    if (companyId != YxProtocol.manufacturerCompanyId || data[2] != 1) {
      return null;
    }
    return data
        .sublist(4, 10)
        .map((byte) => byte.toRadixString(16).padLeft(2, '0'))
        .join()
        .toUpperCase();
  }
}
