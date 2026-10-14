import 'package:path/path.dart' as p;
import 'package:sqflite/sqflite.dart';

class PendingTaskCreate {
  const PendingTaskCreate({
    required this.idempotencyKey,
    required this.taskJson,
  });

  final String idempotencyKey;
  final String taskJson;
}

abstract interface class DeviceRemarkStore {
  Future<void> initialize();
  Future<Map<String, String>> loadAllRemarks();
  Future<String?> getRemark(String deviceId);
  Future<void> saveRemark(String deviceId, String remark);
  Future<void> recordConnected(String deviceId, DateTime connectedAt);
  Future<PendingTaskCreate?> getPendingTaskCreate(String deviceId);
  Future<void> savePendingTaskCreate(
    String deviceId,
    PendingTaskCreate pending,
  );
  Future<void> clearPendingTaskCreate(String deviceId);
  Future<void> close();
}

class SqfliteDeviceRemarkStore implements DeviceRemarkStore {
  Database? _database;

  Future<Database> get _db async {
    await initialize();
    return _database!;
  }

  @override
  Future<void> initialize() async {
    if (_database?.isOpen ?? false) return;
    final databaseDirectory = await getDatabasesPath();
    _database = await openDatabase(
      p.join(databaseDirectory, 'yx_gateway.db'),
      version: 2,
      onCreate: (database, version) async {
        await database.execute('''
          CREATE TABLE device_notes(
            device_id TEXT PRIMARY KEY NOT NULL,
            remark TEXT NOT NULL DEFAULT '',
            last_connected_at INTEGER,
            pending_create_key TEXT,
            pending_create_task_json TEXT
          )
        ''');
      },
      onUpgrade: (database, oldVersion, newVersion) async {
        if (oldVersion < 2) {
          await database.execute(
            'ALTER TABLE device_notes ADD COLUMN pending_create_key TEXT',
          );
          await database.execute(
            'ALTER TABLE device_notes '
            'ADD COLUMN pending_create_task_json TEXT',
          );
        }
      },
    );
  }

  @override
  Future<Map<String, String>> loadAllRemarks() async {
    final rows = await (await _db).query('device_notes');
    return <String, String>{
      for (final row in rows)
        row['device_id']! as String: (row['remark'] as String?) ?? '',
    };
  }

  @override
  Future<String?> getRemark(String deviceId) async {
    final rows = await (await _db).query(
      'device_notes',
      columns: const <String>['remark'],
      where: 'device_id = ?',
      whereArgs: <Object>[deviceId],
      limit: 1,
    );
    return rows.isEmpty ? null : rows.first['remark'] as String?;
  }

  @override
  Future<void> saveRemark(String deviceId, String remark) async {
    final database = await _db;
    final changed = await database.update(
      'device_notes',
      <String, Object>{'remark': remark},
      where: 'device_id = ?',
      whereArgs: <Object>[deviceId],
    );
    if (changed == 0) {
      await database.insert('device_notes', <String, Object>{
        'device_id': deviceId,
        'remark': remark,
      });
    }
  }

  @override
  Future<void> recordConnected(String deviceId, DateTime connectedAt) async {
    final database = await _db;
    final changed = await database.update(
      'device_notes',
      <String, Object>{'last_connected_at': connectedAt.millisecondsSinceEpoch},
      where: 'device_id = ?',
      whereArgs: <Object>[deviceId],
    );
    if (changed == 0) {
      await database.insert('device_notes', <String, Object>{
        'device_id': deviceId,
        'remark': '',
        'last_connected_at': connectedAt.millisecondsSinceEpoch,
      });
    }
  }

  @override
  Future<PendingTaskCreate?> getPendingTaskCreate(String deviceId) async {
    final rows = await (await _db).query(
      'device_notes',
      columns: const <String>['pending_create_key', 'pending_create_task_json'],
      where: 'device_id = ?',
      whereArgs: <Object>[deviceId],
      limit: 1,
    );
    if (rows.isEmpty) return null;
    final key = rows.first['pending_create_key'] as String?;
    final taskJson = rows.first['pending_create_task_json'] as String?;
    if (key == null || taskJson == null) return null;
    return PendingTaskCreate(idempotencyKey: key, taskJson: taskJson);
  }

  @override
  Future<void> savePendingTaskCreate(
    String deviceId,
    PendingTaskCreate pending,
  ) async {
    final database = await _db;
    final values = <String, Object>{
      'pending_create_key': pending.idempotencyKey,
      'pending_create_task_json': pending.taskJson,
    };
    final changed = await database.update(
      'device_notes',
      values,
      where: 'device_id = ?',
      whereArgs: <Object>[deviceId],
    );
    if (changed == 0) {
      await database.insert('device_notes', <String, Object>{
        'device_id': deviceId,
        'remark': '',
        ...values,
      });
    }
  }

  @override
  Future<void> clearPendingTaskCreate(String deviceId) async {
    await (await _db).update(
      'device_notes',
      const <String, Object?>{
        'pending_create_key': null,
        'pending_create_task_json': null,
      },
      where: 'device_id = ?',
      whereArgs: <Object>[deviceId],
    );
  }

  @override
  Future<void> close() async {
    await _database?.close();
    _database = null;
  }
}
