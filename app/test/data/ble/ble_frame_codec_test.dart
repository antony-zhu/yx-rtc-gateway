import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:yx_gateway_timer/data/ble/ble_frame_codec.dart';
import 'package:yx_gateway_timer/domain/protocol_error.dart';

void main() {
  group('BleFrameCodec', () {
    test('MTU 23 时按 UTF-8 字节生成连续多片', () {
      final message = <String, Object?>{
        'v': 1,
        'id': 0x1234,
        'name': '仓库网关定时任务',
      };
      final frames = BleFrameCodec.encodeJson(
        json: message,
        transactionId: 0x1234,
        negotiatedMtu: 23,
      );

      expect(frames.length, greaterThan(1));
      expect(frames.every((frame) => frame.length <= 20), isTrue);
      for (var index = 0; index < frames.length; index++) {
        final decoded = BleFrameCodec.decode(frames[index]);
        expect(decoded.sequence, index);
        expect(decoded.transactionId, 0x1234);
        expect(decoded.isStart, index == 0);
        expect(decoded.isEnd, index == frames.length - 1);
      }
      final reassembler = BleFrameReassembler();
      Map<String, Object?>? decoded;
      for (final frame in frames) {
        decoded = reassembler.accept(frame) ?? decoded;
      }
      expect(decoded, message);
    });

    test('大 MTU 的短消息只生成一个 START|END 分片', () {
      final frames = BleFrameCodec.encodeJson(
        json: const <String, Object?>{'v': 1, 'id': 7, 'ok': true},
        transactionId: 7,
        negotiatedMtu: 247,
      );
      expect(frames, hasLength(1));
      final decoded = BleFrameCodec.decode(frames.single);
      expect(decoded.flags, BleFrame.startFlag | BleFrame.endFlag);
      expect(decoded.sequence, 0);
    });

    test('wireLimit 固定不超过 180 字节', () {
      final frames = BleFrameCodec.encodeJson(
        json: <String, Object?>{'v': 1, 'id': 8, 'p': 'x' * 400},
        transactionId: 8,
        negotiatedMtu: 512,
      );
      expect(frames.every((frame) => frame.length <= 180), isTrue);
    });

    test('768 字节接受，769 字节拒绝', () {
      Map<String, Object?> bodyOfSize(int size) {
        for (var length = 0; length < 900; length++) {
          final map = <String, Object?>{'v': 1, 'id': 1, 'p': 'x' * length};
          if (utf8.encode(jsonEncode(map)).length == size) return map;
        }
        throw StateError('无法构造目标长度');
      }

      expect(
        BleFrameCodec.encodeJson(
          json: bodyOfSize(768),
          transactionId: 1,
          negotiatedMtu: 23,
        ),
        isNotEmpty,
      );
      expect(
        () => BleFrameCodec.encodeJson(
          json: bodyOfSize(769),
          transactionId: 1,
          negotiatedMtu: 23,
        ),
        throwsA(isA<ProtocolException>()),
      );
    });

    test('头字段按小端编码', () {
      final frame = BleFrameCodec.encodeJson(
        json: const <String, Object?>{'id': 0x1234},
        transactionId: 0x1234,
        negotiatedMtu: 247,
      ).single;
      expect(frame[4], 0x34);
      expect(frame[5], 0x12);
      final total = utf8
          .encode(jsonEncode(const <String, Object?>{'id': 0x1234}))
          .length;
      expect(frame[6], total & 0xff);
      expect(frame[7], total >> 8);
    });

    test('错误 magic、version 和保留 flags 均被拒绝', () {
      final good = BleFrameCodec.encodeJson(
        json: const <String, Object?>{'id': 1},
        transactionId: 1,
        negotiatedMtu: 23,
      ).first;
      for (final mutation in <void Function(Uint8List)>[
        (bytes) => bytes[0] = 0,
        (bytes) => bytes[1] = 2,
        (bytes) => bytes[2] |= 0x80,
      ]) {
        final bad = Uint8List.fromList(good);
        mutation(bad);
        expect(
          () => BleFrameCodec.decode(bad),
          throwsA(isA<ProtocolException>()),
        );
      }
    });
  });

  group('BleFrameReassembler', () {
    late List<Uint8List> frames;

    setUp(() {
      frames = BleFrameCodec.encodeJson(
        json: <String, Object?>{'id': 41, 'value': '中文内容' * 8},
        transactionId: 41,
        negotiatedMtu: 23,
      );
      expect(frames.length, greaterThan(3));
    });

    test('缺片、重复片和乱序片被拒绝', () {
      final missing = BleFrameReassembler()..accept(frames[0]);
      expect(
        () => missing.accept(frames[2]),
        throwsA(isA<ProtocolException>()),
      );

      final duplicate = BleFrameReassembler()
        ..accept(frames[0])
        ..accept(frames[1]);
      expect(
        () => duplicate.accept(frames[1]),
        throwsA(isA<ProtocolException>()),
      );

      final noStart = BleFrameReassembler();
      expect(
        () => noStart.accept(frames[1]),
        throwsA(isA<ProtocolException>()),
      );
    });

    test('total_len 不一致被拒绝', () {
      final reassembler = BleFrameReassembler()..accept(frames[0]);
      final bad = Uint8List.fromList(frames[1]);
      bad[6] ^= 1;
      expect(() => reassembler.accept(bad), throwsA(isA<ProtocolException>()));
    });

    test('reset 会清空断线前残片', () {
      final reassembler = BleFrameReassembler()..accept(frames[0]);
      expect(reassembler.hasPendingMessage, isTrue);
      reassembler.reset();
      expect(reassembler.hasPendingMessage, isFalse);
      expect(
        () => reassembler.accept(frames[1]),
        throwsA(isA<ProtocolException>()),
      );
    });

    test('Response 与 Event 使用独立重组器', () {
      final responses = BleFrameCodec.encodeJson(
        json: const <String, Object?>{'id': 2, 'ok': true},
        transactionId: 2,
        negotiatedMtu: 23,
      );
      final events = BleFrameCodec.encodeJson(
        json: const <String, Object?>{
          'v': 1,
          'event': 'timer.fired',
          'data': <String, Object?>{},
        },
        transactionId: 0,
        negotiatedMtu: 23,
      );
      final responseAssembler = BleFrameReassembler();
      final eventAssembler = BleFrameReassembler();
      Map<String, Object?>? response;
      Map<String, Object?>? event;
      final count = responses.length > events.length
          ? responses.length
          : events.length;
      for (var index = 0; index < count; index++) {
        if (index < responses.length) {
          response = responseAssembler.accept(responses[index]) ?? response;
        }
        if (index < events.length) {
          event = eventAssembler.accept(events[index]) ?? event;
        }
      }
      expect(response?['id'], 2);
      expect(event?['event'], 'timer.fired');
    });
  });

  test('事务 ID 从 65535 回绕到 1', () {
    final generator = TransactionIdGenerator(initialValue: 65534);
    expect(generator.next(), 65535);
    expect(generator.next(), 1);
  });
}
