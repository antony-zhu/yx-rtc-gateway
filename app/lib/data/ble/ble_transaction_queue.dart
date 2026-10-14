import 'dart:async';
import 'dart:convert';

import 'package:yx_gateway_timer/core/yx_constants.dart';
import 'package:yx_gateway_timer/data/ble/ble_frame_codec.dart';
import 'package:yx_gateway_timer/domain/protocol_error.dart';

abstract interface class BleCommandTransport {
  Future<void> writeWithResponse(List<int> value);
  Future<List<int>> readResponse();
}

class BleTransactionQueue {
  BleTransactionQueue({
    required BleCommandTransport transport,
    required int Function() negotiatedMtu,
    Duration timeout = YxProtocol.responseTimeout,
  }) : _transport = transport,
       _negotiatedMtu = negotiatedMtu,
       _timeout = timeout;

  final BleCommandTransport _transport;
  final int Function() _negotiatedMtu;
  final Duration _timeout;
  final TransactionIdGenerator _transactionIds = TransactionIdGenerator();
  final BleFrameReassembler _responseReassembler = BleFrameReassembler();

  Future<void> _tail = Future<void>.value();
  _PendingTransaction? _pending;
  bool _connected = true;
  int _generation = 0;

  Future<Map<String, Object?>> execute(
    String operation,
    Map<String, Object?> parameters,
  ) {
    final result = Completer<Map<String, Object?>>();
    final previous = _tail;
    final gate = Completer<void>();
    _tail = gate.future;
    final scheduledGeneration = _generation;

    unawaited(() async {
      await previous;
      try {
        if (!_connected || scheduledGeneration != _generation) {
          throw const ProtocolException(
            ProtocolErrorCode.disconnected,
            '连接已断开，命令已取消',
          );
        }
        result.complete(await _executeNow(operation, parameters));
      } on Object catch (error, stackTrace) {
        result.completeError(error, stackTrace);
      } finally {
        gate.complete();
      }
    }());
    return result.future;
  }

  Future<Map<String, Object?>> _executeNow(
    String operation,
    Map<String, Object?> parameters,
  ) async {
    final transactionId = _transactionIds.next();
    final command = <String, Object?>{
      'v': YxProtocol.version,
      'id': transactionId,
      'op': operation,
      'params': parameters,
    };
    final fragments = BleFrameCodec.encodeJson(
      json: command,
      transactionId: transactionId,
      negotiatedMtu: _negotiatedMtu(),
    );
    final pending = _PendingTransaction(transactionId);
    _pending = pending;
    try {
      for (final fragment in fragments) {
        if (!_connected) {
          throw const ProtocolException(
            ProtocolErrorCode.disconnected,
            '写入分片时连接已断开',
          );
        }
        await _transport.writeWithResponse(fragment);
      }

      Map<String, Object?> response;
      try {
        response = await pending.completer.future.timeout(_timeout);
      } on TimeoutException {
        response = await _recoverByReading(transactionId);
      }
      return _unwrap(response, transactionId);
    } finally {
      if (identical(_pending, pending)) _pending = null;
    }
  }

  void onResponseFragment(List<int> value) {
    try {
      final response = _responseReassembler.accept(value);
      if (response == null) return;
      final pending = _pending;
      if (pending != null &&
          response['id'] == pending.transactionId &&
          !pending.completer.isCompleted) {
        pending.completer.complete(response);
      }
    } on Object catch (error, stackTrace) {
      final pending = _pending;
      if (pending != null && !pending.completer.isCompleted) {
        pending.completer.completeError(error, stackTrace);
      }
    }
  }

  Future<Map<String, Object?>> _recoverByReading(int transactionId) async {
    final bytes = await _transport.readResponse();
    try {
      final decoded = jsonDecode(utf8.decode(bytes, allowMalformed: false));
      if (decoded is! Map || decoded['id'] != transactionId) {
        throw const ProtocolException(
          ProtocolErrorCode.timeout,
          'Response Read 未返回当前事务',
        );
      }
      return decoded.map<String, Object?>(
        (key, value) => MapEntry(key.toString(), value),
      );
    } on ProtocolException {
      rethrow;
    } on Object catch (error) {
      throw ProtocolException(
        ProtocolErrorCode.malformedResponse,
        'Response Read 不是有效 JSON',
        cause: error,
      );
    }
  }

  Map<String, Object?> _unwrap(
    Map<String, Object?> response,
    int transactionId,
  ) {
    if (response['v'] != YxProtocol.version ||
        response['id'] != transactionId) {
      throw const ProtocolException(
        ProtocolErrorCode.malformedResponse,
        '响应版本或事务 ID 不匹配',
      );
    }
    if (response['ok'] == true) {
      final result = response['result'];
      if (result is! Map) {
        throw const ProtocolException(
          ProtocolErrorCode.malformedResponse,
          '成功响应缺少 result 对象',
        );
      }
      return result.map<String, Object?>(
        (key, value) => MapEntry(key.toString(), value),
      );
    }

    final error = response['error'];
    final errorMap = error is Map ? error : const <String, Object?>{};
    final code = ProtocolErrorCode.fromWire(errorMap['code'] as String?);
    final message = errorMap['message'] is String
        ? errorMap['message']! as String
        : '设备拒绝了命令';
    throw ProtocolException(code, message);
  }

  void markConnected() {
    _connected = true;
  }

  void reset() {
    _connected = false;
    _generation++;
    _responseReassembler.reset();
    final pending = _pending;
    if (pending != null && !pending.completer.isCompleted) {
      pending.completer.complete(<String, Object?>{
        'v': YxProtocol.version,
        'id': pending.transactionId,
        'ok': false,
        'error': const <String, Object?>{
          'code': 'disconnected',
          'message': 'BLE 连接已断开',
        },
      });
    }
    _pending = null;
  }
}

class _PendingTransaction {
  _PendingTransaction(this.transactionId);

  final int transactionId;
  final Completer<Map<String, Object?>> completer =
      Completer<Map<String, Object?>>();
}
