PRAGMA foreign_keys = OFF;

CREATE TEMP TABLE credentials_backup AS
SELECT player_id, password_hash, password_changed_at_ms
FROM credentials;

CREATE TEMP TABLE sessions_backup AS
SELECT id,
       player_id,
       secret_hash,
       created_at_ms,
       expires_at_ms,
       last_refreshed_at_ms,
       revoked_at_ms,
       revoked_reason
FROM sessions;

CREATE TABLE players_new (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL,
    username_normalized TEXT NOT NULL UNIQUE,
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);

INSERT INTO players_new(id, username, username_normalized, created_at_ms, updated_at_ms)
SELECT id, username, username_normalized, created_at_ms, updated_at_ms
FROM players
ORDER BY id;

DROP TABLE credentials;
DROP TABLE sessions;
DROP TABLE players;

ALTER TABLE players_new RENAME TO players;

CREATE TABLE credentials (
    player_id INTEGER PRIMARY KEY REFERENCES players(id) ON DELETE CASCADE,
    password_hash TEXT NOT NULL,
    password_changed_at_ms INTEGER NOT NULL
);

INSERT INTO credentials(player_id, password_hash, password_changed_at_ms)
SELECT player_id, password_hash, password_changed_at_ms
FROM credentials_backup;

DROP TABLE credentials_backup;

CREATE TABLE sessions (
    id TEXT PRIMARY KEY,
    player_id INTEGER NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    secret_hash TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL,
    expires_at_ms INTEGER NOT NULL,
    last_refreshed_at_ms INTEGER NOT NULL,
    revoked_at_ms INTEGER,
    revoked_reason TEXT
);

INSERT INTO sessions(
    id,
    player_id,
    secret_hash,
    created_at_ms,
    expires_at_ms,
    last_refreshed_at_ms,
    revoked_at_ms,
    revoked_reason)
SELECT id,
       player_id,
       secret_hash,
       created_at_ms,
       expires_at_ms,
       last_refreshed_at_ms,
       revoked_at_ms,
       revoked_reason
FROM sessions_backup;

DROP TABLE sessions_backup;

CREATE INDEX idx_sessions_player_id ON sessions(player_id);

INSERT INTO sqlite_sequence(name, seq)
SELECT 'players', COALESCE(MAX(id), 0)
FROM players
WHERE NOT EXISTS (SELECT 1 FROM sqlite_sequence WHERE name = 'players');

UPDATE sqlite_sequence
SET seq = (SELECT COALESCE(MAX(id), 0) FROM players)
WHERE name = 'players';

PRAGMA foreign_keys = ON;