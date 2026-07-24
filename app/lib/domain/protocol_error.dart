enum ProtocolErrorCode {
  badJson('bad_json'),
  badRequest('bad_request'),
  badFrame('bad_frame'),
  fragmentMismatch('fragment_mismatch'),
  incompleteFrame('incomplete_frame'),
  unknownOp('unknown_op'),
  invalidParam('invalid_param'),
  invalidFrame('invalid_frame'),
  notFound('not_found'),
  revisionConflict('revision_conflict'),
  idempotencyConflict('idempotency_conflict'),
  limitReached('limit_reached'),
  timeUntrusted('time_untrusted'),
  timeRollback('time_rollback'),
  noMemory('no_memory'),
  internalError('internal_error'),
  timeout('timeout'),
  responseTooLarge('response_too_large'),
  identityConflict('identity_conflict'),
  disconnected('disconnected'),
  malformedResponse('malformed_response'),
  unknown('unknown');

  const ProtocolErrorCode(this.wireName);

  final String wireName;

  static ProtocolErrorCode fromWire(String? value) => values.firstWhere(
    (item) => item.wireName == value,
    orElse: () => unknown,
  );
}

class ProtocolException implements Exception {
  const ProtocolException(this.code, this.message, {this.cause});

  final ProtocolErrorCode code;
  final String message;
  final Object? cause;

  bool get isRevisionConflict => code == ProtocolErrorCode.revisionConflict;

  @override
  String toString() => 'ProtocolException(${code.wireName}): $message';
}
