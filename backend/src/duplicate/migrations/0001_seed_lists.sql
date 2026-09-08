CREATE TABLE IF NOT EXISTS duplicate_seed_lists (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    creation_token TEXT NOT NULL UNIQUE,
    master_token TEXT NOT NULL UNIQUE,
    round_count INTEGER NOT NULL,
    seeds_hex TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL,
    expires_at_ms INTEGER NOT NULL,
    next_session_number INTEGER NOT NULL DEFAULT 0,
    live_session_count INTEGER NOT NULL DEFAULT 0
);
