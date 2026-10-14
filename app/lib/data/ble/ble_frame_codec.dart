import 'dart:convert';
import 'dart:typed_data';

import 'package:yx_gateway_timer/core/yx_constants.dart';
import 'package:yx_gateway_timer/domain/protocol_error.dart';

class BleFrame {
  const BleFrame({
    required this.flags,
    required this.sequence,
    required this.transactionId,
    required this.totalLength,
    required this.payload,
  });

  static const int startFlag = 0x01;
  static const int endFlag = 0x02;

  final int flags;
  final int sequence;
  final int transactionId;
  final int totalLength;
  final Uint8List payload;

  bool get isStart => flags & startFlag != 0;
  bool get isEnd => flags & endFlag != 0;
}

abstract final class BleFrameCodec {
  static const int headerLength = 8;
  static const int magic = 0xa5;
  static const int version = 1;
  static const int maximumWireLength = 180;

  static List<Uint8List> encodeJson({
    required Map<String, Object?> json,
    required int transactionId,
    required int negotiatedMtu,
  }) {
    if (transactionId < 0 || transactionId > 0xffff) {
      throw const ProtocolException(
        ProtocolErrorCode.invalidParam,
        'transactionId 超出 uint16 范围',
      );
    }
    final body = Uint8List.fromList(utf8.encode(jsonEncode(json)));
    if (body.isEmpty || body.length > YxProtocol.maxJsonBytes) {
      throw const ProtocolException(
        ProtocolErrorCode.responseTooLarge,
        'JSON 必须为 1～768 个 UTF-8 字节',
      );
    }

    final attPayload = negotiatedMtu - 3;
    final wireLimit = attPayload < maximumWireLength
        ? attPayload
        : maximumWireLength;
    final fragmentPayload = wireLimit - headerLength;
    if (fragmentPayload <= 0) {
      throw const ProtocolException(
        ProtocolErrorCode.invalidParam,
        '协商 MTU 无法容纳分片头',
      );
    }

    final fragmentCount = (body.length / fragmentPayload).ceil();
    if (fragmentCount > 256) {
      throw const ProtocolException(
        ProtocolErrorCode.responseTooLarge,
        '分片数超过 seq 可表示范围',
      );
    }

    return <Uint8List>[
      for (var sequence = 0; sequence < fragmentCount; sequence++)
        _encodeFragment(
          body: body,
          start: sequence * fragmentPayload,
          end: ((sequence + 1) * fragmentPayload).clamp(0, body.length),
          sequence: sequence,
          transactionId: transactionId,
          isFirst: sequence == 0,
          isLast: sequence == fragmentCount - 1,
        ),
    ];
  }

  static Uint8List _encodeFragment({
    required Uint8List body,
    required int start,
    required int end,
    required int sequence,
    required int transactionId,
    required bool isFirst,
    required bool isLast,
  }) {
    final payloadLength = end - start;
    final output = Uint8List(headerLength + payloadLength);
    final data = ByteData.sublistView(output);
    output[0] = magic;
    output[1] = version;
    output[2] =
        (isFirst ? BleFrame.startFlag : 0) | (isLast ? BleFrame.endFlag : 0);
    output[3] = sequence;
    data.setUint16(4, transactionId, Endian.little);
    data.setUint16(6, body.length, Endian.little);
    output.setRange(headerLength, output.length, body, start);
    return output;
  }

  static BleFrame decode(Uint8List bytes) {
    if (bytes.length < headerLength) {
      throw const ProtocolException(ProtocolErrorCode.badFrame, '分片短于 8 字节头');
    }
    if (bytes[0] != magic) {
      throw const ProtocolException(ProtocolErrorCode.badFrame, '分片 magic 错误');
    }
    if (bytes[1] != version) {
      throw const ProtocolException(
        ProtocolErrorCode.badFrame,
        '分片 version 错误',
      );
    }
    final flags = bytes[2];
    if (flags & ~(BleFrame.startFlag | BleFrame.endFlag) != 0) {
      throw const ProtocolException(
        ProtocolErrorCode.badFrame,
        '分片 flags 包含保留位',
      );
    }
    final header = ByteData.sublistView(bytes);
    final totalLength = header.getUint16(6, Endian.little);
    if (totalLength == 0 || totalLength > YxProtocol.maxJsonBytes) {
      throw const ProtocolException(
        ProtocolErrorCode.badFrame,
        'total_len 超出 1～768 字节',
      );
    }
    return BleFrame(
      flags: flags,
      sequence: bytes[3],
      transactionId: header.getUint16(4, Endian.little),
      totalLength: totalLength,
      payload: Uint8List.sublistView(bytes, headerLength),
    );
  }
}

class BleFrameReassembler {
  int? _transactionId;
  int? _totalLength;
  int _nextSequence = 0;
  BytesBuilder? _payload;

  bool get hasPendingMessage => _payload != null;

  Map<String, Object?>? accept(List<int> value) {
    final frame = BleFrameCodec.decode(Uint8List.fromList(value));
    if (frame.isStart) {
      if (frame.sequence != 0) {
        reset();
        throw const ProtocolException(
          ProtocolErrorCode.fragmentMismatch,
          'START 分片的 seq 必须为 0',
        );
      }
      reset();
      _transactionId = frame.transactionId;
      _totalLength = frame.totalLength;
      _nextSequence = 0;
      _payload = BytesBuilder(copy: false);
    } else if (_payload == null) {
      throw const ProtocolException(
        ProtocolErrorCode.incompleteFrame,
        '未收到 START 分片',
      );
    }

    if (frame.sequence != _nextSequence ||
        frame.transactionId != _transactionId ||
        frame.totalLength != _totalLength) {
      reset();
      throw const ProtocolException(
        ProtocolErrorCode.fragmentMismatch,
        'seq、txn_id 或 total_len 不连续',
      );
    }

    _payload!.add(frame.payload);
    _nextSequence++;
    final receivedLength = _payload!.length;
    if (receivedLength > frame.totalLength ||
        (!frame.isEnd && receivedLength >= frame.totalLength)) {
      reset();
      throw const ProtocolException(
        ProtocolErrorCode.fragmentMismatch,
        '重组长度超过或提前到达 total_len',
      );
    }
    if (!frame.isEnd) return null;
    if (receivedLength != frame.totalLength) {
      reset();
      throw const ProtocolException(
        ProtocolErrorCode.incompleteFrame,
        'END 分片后的长度与 total_len 不一致',
      );
    }

    final bytes = _payload!.takeBytes();
    final expectedTransactionId = _transactionId;
    reset();
    try {
      final value = jsonDecode(utf8.decode(bytes, allowMalformed: false));
      if (value is! Map) {
        throw const FormatException('JSON 根节点不是对象');
      }
      final result = value.map<String, Object?>(
        (key, item) => MapEntry(key.toString(), item),
      );
      if (expectedTransactionId != 0 && result['id'] != expectedTransactionId) {
        throw const ProtocolException(
          ProtocolErrorCode.fragmentMismatch,
          'JSON id 与分片 txn_id 不一致',
        );
      }
      return result;
    } on ProtocolException {
      rethrow;
    } on Object catch (error) {
      throw ProtocolException(
        ProtocolErrorCode.badJson,
        '分片内容不是有效的 UTF-8 JSON',
        cause: error,
      );
    }
  }

  void reset() {
    _transactionId = null;
    _totalLength = null;
    _nextSequence = 0;
    _payload = null;
  }
}

class TransactionIdGenerator {
  TransactionIdGenerator({int initialValue = 0}) : _value = initialValue;

  int _value;

  int next() {
    _value = _value >= 0xffff ? 1 : _value + 1;
    if (_value == 0) _value = 1;
    return _value;
  }
}
