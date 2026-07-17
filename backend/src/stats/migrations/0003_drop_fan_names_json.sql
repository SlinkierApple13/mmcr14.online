CREATE TABLE IF NOT EXISTS stats_round_entries_new (
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
    winning_hand_json TEXT,
    PRIMARY KEY (session_identifier, round_number)
);

INSERT INTO stats_round_entries_new(
    session_identifier,
    round_number,
    drawn_game,
    winner_seat,
    from_seat,
    win_type_bits,
    win_tile,
    turn,
    time_ms,
    fan_text,
    meld_count_json,
    players_json,
    fan_results_json,
    winning_hand_json
)
SELECT
    session_identifier,
    round_number,
    drawn_game,
    winner_seat,
    from_seat,
    win_type_bits,
    win_tile,
    turn,
    time_ms,
    fan_text,
    meld_count_json,
    players_json,
    fan_results_json,
    winning_hand_json
FROM stats_round_entries;

DROP TABLE stats_round_entries;

ALTER TABLE stats_round_entries_new RENAME TO stats_round_entries;

CREATE INDEX IF NOT EXISTS stats_round_entries_time_idx
    ON stats_round_entries(time_ms DESC);