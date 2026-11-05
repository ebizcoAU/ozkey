'use strict';
/**
 * S1e-5 (ozkey-50 §7): one zone for both clocks, ozlodge's copy of S1e-4.
 *
 * ozlodge is a fork and is not on ozserver-core, so S1e-4 never reached it.
 * Its pool had no `timezone`, so mysql2 read naive DATETIME columns as
 * Node-local while SQL NOW() wrote them in MySQL's session zone. The only
 * naive DATETIME is rooms.last_synced_at (written by NOW() in the PMS sync,
 * returned by GET /pms/rooms/status). Whenever Node's zone differs from
 * MySQL's, that stamp is off by the gap. The lab MySQL's global zone is
 * +07:00 (/etc/my.cnf) on an AEDT host, so it is off there today.
 *
 * Kept in its own module so the regression test exercises the exact pool
 * the server uses, without spawning the server (which would join the lab
 * broker and advertise itself over mDNS).
 */

const S1E5_FLAG = 's1e5_utc_last_synced_at';

/** A pool whose JS Dates and SQL NOW() both mean UTC. */
function createUtcPool(mysql, dbConfig, logEvent) {
  const pool = mysql.createPool({ ...dbConfig, timezone: 'Z' });
  pool.on('connection', (conn) => {
    conn.query("SET time_zone = '+00:00'", (err) => {
      if (err) logEvent('error', `SET time_zone failed: ${err.message}`);
    });
  });
  return pool;
}

/** One-time clear of rooms.last_synced_at written before the pool was pinned.
 *  Those values are in whatever zone MySQL had, and nothing records which, so
 *  they can't be shifted safely. The column is a display-only sync stamp that
 *  the next PMS sync refills, so NULL is the honest value. Guarded by a
 *  schema_flags row in the same transaction: runs once per DB, and on a fresh
 *  DB it clears nothing and just sets the flag. */
async function clearPreUtcSyncStamps(pool, logEvent) {
  await pool.query(`
    CREATE TABLE IF NOT EXISTS schema_flags (
      name VARCHAR(64) PRIMARY KEY,
      applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ) ENGINE=InnoDB`);
  const [done] = await pool.query('SELECT 1 FROM schema_flags WHERE name = ?', [S1E5_FLAG]);
  if (done.length) return null;

  const conn = await pool.getConnection();
  try {
    await conn.beginTransaction();
    const [r] = await conn.query('UPDATE rooms SET last_synced_at = NULL WHERE last_synced_at IS NOT NULL');
    await conn.query('INSERT INTO schema_flags (name) VALUES (?)', [S1E5_FLAG]);
    await conn.commit();
    logEvent('info', `S1e-5 UTC migration: cleared last_synced_at on ${r.affectedRows} room(s)`);
    return r.affectedRows;
  } catch (err) {
    await conn.rollback().catch(() => {});
    throw err;
  } finally {
    conn.release();
  }
}

module.exports = { createUtcPool, clearPreUtcSyncStamps, S1E5_FLAG };
