import 'dart:math';

import 'package:flutter_test/flutter_test.dart';
import 'package:yx_gateway_timer/core/idempotency_key.dart';

void main() {
  test('新建任务幂等键是标准 UUID v4 且每次不同', () {
    final first = createIdempotencyKey(Random(1));
    final second = createIdempotencyKey(Random(2));
    final pattern = RegExp(
      r'^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$',
    );

    expect(first, matches(pattern));
    expect(second, matches(pattern));
    expect(first, isNot(second));
  });
}
