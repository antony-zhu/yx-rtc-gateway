import 'package:flutter_test/flutter_test.dart';
import 'package:yx_gateway_timer/data/ble/ble_scanner.dart';

void main() {
  test('Manufacturer Data 按稳定 device_id 解析', () {
    final id = ManufacturerDataParser.parseDeviceId(const <int>[
      0xff,
      0xff,
      1,
      0,
      0x80,
      0xb5,
      0x4e,
      0xe9,
      0x21,
      0x38,
    ]);
    expect(id, '80B54EE92138');
  });

  test('拒绝长度、Company ID 和协议版本错误的广播', () {
    expect(ManufacturerDataParser.parseDeviceId(const <int>[]), isNull);
    expect(
      ManufacturerDataParser.parseDeviceId(const <int>[
        0,
        0,
        1,
        0,
        1,
        2,
        3,
        4,
        5,
        6,
      ]),
      isNull,
    );
    expect(
      ManufacturerDataParser.parseDeviceId(const <int>[
        0xff,
        0xff,
        2,
        0,
        1,
        2,
        3,
        4,
        5,
        6,
      ]),
      isNull,
    );
  });
}
