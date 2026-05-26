CREATE TABLE players (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL,
    username_normalized TEXT NOT NULL UNIQUE,
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);

CREATE TABLE credentials (
    player_id INTEGER PRIMARY KEY REFERENCES players(id) ON DELETE CASCADE,
    password_hash TEXT NOT NULL,
    password_changed_at_ms INTEGER NOT NULL
);

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

CREATE INDEX idx_sessions_player_id ON sessions(player_id);