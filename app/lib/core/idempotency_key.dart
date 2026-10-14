import 'dart:math';

/// 生成符合 RFC 4122 格式的随机 UUID v4，用作新建任务的业务幂等键。
String createIdempotencyKey([Random? source]) {
  final random = source ?? Random.secure();
  final bytes = List<int>.generate(16, (_) => random.nextInt(256));
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;

  String byte(int index) => bytes[index].toRadixString(16).padLeft(2, '0');
  return <String>[
    List<String>.generate(4, (index) => byte(index)).join(),
    List<String>.generate(2, (index) => byte(index + 4)).join(),
    List<String>.generate(2, (index) => byte(index + 6)).join(),
    List<String>.generate(2, (index) => byte(index + 8)).join(),
    List<String>.generate(6, (index) => byte(index + 10)).join(),
  ].join('-');
}
