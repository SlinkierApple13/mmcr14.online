CREATE TABLE test_items (
    id TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL DEFAULT (CAST((julianday('now') - 2440587.5) * 86400000 AS INTEGER))
);