CREATE TABLE IF NOT EXISTS duplicate_staged_records (
    creation_token TEXT NOT NULL,
    session_identifier TEXT NOT NULL,
    round_number INTEGER NOT NULL,
    payload_json TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL,
    PRIMARY KEY (creation_token, session_identifier, round_number)
);
