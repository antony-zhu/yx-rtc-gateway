import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';

abstract final class YxGatt {
  static final Uuid service = Uuid.parse(
    '666ca870-68df-4426-8378-4587311687e0',
  );
  static final Uuid info = Uuid.parse('666ca870-68df-4426-8378-4587311687e1');
  static final Uuid command = Uuid.parse(
    '666ca870-68df-4426-8378-4587311687e2',
  );
  static final Uuid response = Uuid.parse(
    '666ca870-68df-4426-8378-4587311687e3',
  );
  static final Uuid event = Uuid.parse('666ca870-68df-4426-8378-4587311687e4');
}

abstract final class YxProtocol {
  static const int version = 1;
  static const int maxJsonBytes = 768;
  static const int maxTasks = 16;
  static const int manufacturerCompanyId = 0xffff;
  static const Duration responseTimeout = Duration(seconds: 5);
  static const List<Duration> reconnectBackoff = <Duration>[
    Duration(seconds: 1),
    Duration(seconds: 2),
    Duration(seconds: 4),
    Duration(seconds: 8),
    Duration(seconds: 16),
    Duration(seconds: 30),
  ];
}
