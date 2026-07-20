# Device Status Schema Migration

`device_status` now requires these additive fields for S3 child-registry facts:

- `status_source`
- `child_status`
- `child_last_seen_ms`
- `server_received_ms`
- `link_lost`
- `voice_busy`

The migration only adds missing columns and indexes. It does not delete columns or rows, rewrite historical BME records, or infer a C5 owner for historical empty `device_id` values.

## Deployment procedure

Run these commands on the deployment host only after backing up the intended SQLite file. Do not run them against a live database until the path has been reviewed.

```sh
cp /absolute/path/to/database.db /absolute/path/to/database.db.before-device-status-migration.bak
ESP_SERVER_DB_PATH=/absolute/path/to/database.db \
  node scripts/migrate-device-status-schema.js --dry-run
ESP_SERVER_DB_PATH=/absolute/path/to/database.db \
  node scripts/migrate-device-status-schema.js
```

Verify before restarting the application:

```sql
PRAGMA table_info(device_status);
SELECT COUNT(*) FROM dashboard_snapshots;
SELECT device_id, COUNT(*) AS count
FROM sensor_records
GROUP BY device_id
ORDER BY count DESC;
SELECT COUNT(*) AS empty_device_id_count
FROM sensor_records
WHERE device_id IS NULL OR TRIM(device_id) = '';
```

Then submit one authenticated S3 dashboard snapshot, verify a new `dashboard_snapshots` row, and verify both C5 status rows. Submit BME v1 records only for `sensair_shuttle_01` and `sensair_shuttle_02`; an absent or unsupported `device_id` must return a 4xx without a new sensor row.

## Rollback

Stop the application through the deployment process, replace the migrated database with the backup created above, and start the previously deployed application version. The migration is additive, so a code rollback can also be performed without restoring the database when the older code tolerates the added columns.

The migration tool refuses to run without `ESP_SERVER_DB_PATH` and refuses the repository default `db/database.db`. It never starts the HTTP server.
