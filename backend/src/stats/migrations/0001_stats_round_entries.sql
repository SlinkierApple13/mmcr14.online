CREATE TABLE IF NOT EXISTS stats_round_entries (
    session_identifier TEXT NOT NULL,
    round_number INTEGER NOT NULL,
    drawn_game INTEGER NOT NULL,
    winner_seat INTEGER NOT NULL,
    from_seat INTEGER NOT NULL,
    win_type_bits INTEGER NOT NULL,
    win_tile INTEGER NOT NULL,
    turn INTEGER NOT NULL,
    time_ms INTEGER NOT NULL,
    fan_text TEXT NOT NULL,
    meld_count_json TEXT NOT NULL,
    players_json TEXT NOT NULL,
    fan_results_json TEXT NOT NULL,
    fan_names_json TEXT NOT NULL,
    PRIMARY KEY (session_identifier, round_number)
);

CREATE INDEX IF NOT EXISTS stats_round_entries_time_idx
    ON stats_round_entries(time_ms DESC);