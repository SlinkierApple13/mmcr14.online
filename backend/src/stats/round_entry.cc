#include "stats/stats_internal.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace mmcr::stats {

std::string JsonToCompactString(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["commentStyle"] = "None";
    return Json::writeString(builder, value);
}

util::StatusOr<Json::Value> ParseJsonString(std::string_view raw) {
    Json::CharReaderBuilder builder;
    Json::Value parsed;
    std::string errors;
    std::istringstream stream{std::string(raw)};
    if (!Json::parseFromStream(builder, stream, &parsed, &errors)) {
        return util::Status::InvalidArgument("failed to parse stats JSON: " + errors);
    }
    return parsed;
}

util::StatusOr<const Json::Value*> ReadRequiredObject(const Json::Value& object,
                        std::string_view field_name) {
    const Json::Value& field = object[std::string(field_name)];
    if (!field.isObject()) {
        return util::Status::InvalidArgument(std::string(field_name) + " must be an object");
    }
    return &field;
}

util::StatusOr<const Json::Value*> ReadRequiredArray(const Json::Value& object,
                       std::string_view field_name) {
    const Json::Value& field = object[std::string(field_name)];
    if (!field.isArray()) {
        return util::Status::InvalidArgument(std::string(field_name) + " must be an array");
    }
    return &field;
}

util::StatusOr<std::string> ReadRequiredString(const Json::Value& object,
                        std::string_view field_name) {
    const Json::Value& field = object[std::string(field_name)];
    if (!field.isString()) {
        return util::Status::InvalidArgument(std::string(field_name) + " must be a string");
    }
    return field.asString();
}

util::StatusOr<std::int64_t> ReadRequiredInt64(const Json::Value& object,
                       std::string_view field_name) {
    const Json::Value& field = object[std::string(field_name)];
    if (field.isInt64()) {
        return field.asInt64();
    }
    if (field.isInt()) {
        return static_cast<std::int64_t>(field.asInt());
    }
    if (field.isUInt64()) {
        return static_cast<std::int64_t>(field.asUInt64());
    }
    if (field.isUInt()) {
        return static_cast<std::int64_t>(field.asUInt());
    }
    return util::Status::InvalidArgument(std::string(field_name) + " must be an integer");
}

util::StatusOr<std::uint64_t> ReadRequiredUInt64(const Json::Value& object,
                        std::string_view field_name) {
    auto value = ReadRequiredInt64(object, field_name);
    if (!value.ok()) {
        return value.status();
    }
    if (value.value() < 0) {
        return util::Status::InvalidArgument(std::string(field_name) + " must be non-negative");
    }
    return static_cast<std::uint64_t>(value.value());
}

util::StatusOr<double> ReadRequiredDouble(const Json::Value& object,
                        std::string_view field_name) {
    const Json::Value& field = object[std::string(field_name)];
    if (!field.isDouble() && !field.isInt() && !field.isUInt() && !field.isInt64() && !field.isUInt64()) {
        return util::Status::InvalidArgument(std::string(field_name) + " must be numeric");
    }
    return field.asDouble();
}

util::StatusOr<std::array<RoundPlayer, 4>> ParsePlayers(const Json::Value& initial_seats) {
    if (!initial_seats.isArray() || initial_seats.size() != 4) {
        return util::Status::InvalidArgument("initial_seats must contain exactly four seats");
    }

    std::array<RoundPlayer, 4> players;
    for (Json::ArrayIndex index = 0; index < initial_seats.size(); ++index) {
        const auto& seat = initial_seats[index];
        if (!seat.isObject()) {
            return util::Status::InvalidArgument("initial_seats entries must be objects");
        }

        players[index].seat_index = static_cast<int>(index);
        const Json::Value& player_id = seat["player_id"];
        if (player_id.isInt64()) {
            players[index].player_id = player_id.asInt64();
        } else if (player_id.isInt()) {
            players[index].player_id = player_id.asInt();
        } else if (player_id.isUInt64()) {
            players[index].player_id = static_cast<std::int64_t>(player_id.asUInt64());
        } else {
            players[index].player_id = 0;
        }

        const Json::Value& player_name = seat["player_name"];
        players[index].username = player_name.isString() ? player_name.asString() : std::string();
    }

    return players;
}

std::string EncodePlayersJson(const std::array<RoundPlayer, 4>& players) {
    Json::Value payload(Json::arrayValue);
    for (const auto& player : players) {
        Json::Value entry(Json::objectValue);
        entry["seat_index"] = player.seat_index;
        entry["player_id"] = Json::Int64(player.player_id);
        entry["username"] = player.username;
        payload.append(std::move(entry));
    }
    return JsonToCompactString(payload);
}

util::StatusOr<std::array<RoundPlayer, 4>> ParsePlayersJson(std::string_view raw) {
    auto parsed = ParseJsonString(raw);
    if (!parsed.ok()) {
        return parsed.status();
    }
    if (!parsed.value().isArray() || parsed.value().size() != 4) {
        return util::Status::InvalidArgument("players_json must contain four entries");
    }

    std::array<RoundPlayer, 4> players;
    for (Json::ArrayIndex index = 0; index < parsed.value().size(); ++index) {
        const auto& entry = parsed.value()[index];
        players[index].seat_index = static_cast<int>(index);
        players[index].player_id = entry["player_id"].isInt64() ? entry["player_id"].asInt64() : 0;
        players[index].username = entry["username"].isString() ? entry["username"].asString() : std::string();
    }
    return players;
}

std::string EncodeMeldCountJson(const std::array<int, 4>& meld_count) {
    Json::Value payload(Json::arrayValue);
    for (const int count : meld_count) {
        payload.append(count);
    }
    return JsonToCompactString(payload);
}

util::StatusOr<std::array<int, 4>> ParseMeldCountJson(std::string_view raw) {
    auto parsed = ParseJsonString(raw);
    if (!parsed.ok()) {
        return parsed.status();
    }
    if (!parsed.value().isArray() || parsed.value().size() != 4) {
        return util::Status::InvalidArgument("meld_count_json must contain four integers");
    }

    std::array<int, 4> meld_count{};
    for (Json::ArrayIndex index = 0; index < parsed.value().size(); ++index) {
        meld_count[index] = parsed.value()[index].isInt() ? parsed.value()[index].asInt() : 0;
    }
    return meld_count;
}

std::string EncodeFanResultsJson(const std::vector<qingque::fan_code>& fan_results) {
    Json::Value payload(Json::arrayValue);
    for (const auto& fan_result : fan_results) {
        payload.append(fan_result.to_string());
    }
    return JsonToCompactString(payload);
}

util::StatusOr<std::vector<qingque::fan_code>> ParseFanResultsJson(std::string_view raw) {
    auto parsed = ParseJsonString(raw);
    if (!parsed.ok()) {
        return parsed.status();
    }
    if (!parsed.value().isArray()) {
        return util::Status::InvalidArgument("fan_results_json must be an array");
    }

    std::vector<qingque::fan_code> fan_results;
    fan_results.reserve(parsed.value().size());
    for (const auto& entry : parsed.value()) {
        if (!entry.isString()) {
            return util::Status::InvalidArgument("fan_results_json must contain strings");
        }
        fan_results.emplace_back(entry.asString());
    }
    return fan_results;
}

std::vector<int> FanIdsFromResults(const std::vector<qingque::fan_code>& fan_results) {
    std::vector<int> fan_ids;
    if (fan_results.empty()) {
        return fan_ids;
    }
    const auto& primary_result = fan_results.front();
    for (std::size_t index = 0; index < qingque::fans.size(); ++index) {
        if (primary_result[index]) {
            fan_ids.push_back(static_cast<int>(index));
        }
    }
    return fan_ids;
}

std::string EncodeWinningHandJson(const std::optional<game::HandWrapper>& winning_hand) {
    return winning_hand.has_value() ? JsonToCompactString(winning_hand->ToJson()) : std::string();
}

util::StatusOr<std::optional<game::HandWrapper>> ParseWinningHandJson(std::string_view raw) {
    if (raw.empty()) {
        return std::optional<game::HandWrapper>{};
    }
    auto parsed = ParseJsonString(raw);
    if (!parsed.ok()) {
        return parsed.status();
    }
    if (parsed.value().isNull()) {
        return std::optional<game::HandWrapper>{};
    }
    auto hand = game::HandWrapper::FromJson(parsed.value());
    if (!hand.ok()) {
        return hand.status();
    }
    return std::optional<game::HandWrapper>{hand.value()};
}

util::Status StepDone(storage::Statement& statement) {
    auto step = statement.Step();
    if (!step.ok()) {
        return step.status();
    }
    if (step.value() != storage::Statement::StepResult::kDone) {
        return util::Status::Internal("statement unexpectedly returned a row");
    }
    return util::Status::Ok();
}

util::StatusOr<double> ParseFanText(std::string_view raw) {
    try {
        return std::stod(std::string(raw));
    } catch (...) {
        return util::Status::InvalidArgument("invalid stored fan value");
    }
}

std::size_t RoundKeyHash::operator()(const RoundKey& key) const noexcept {
    const auto first = std::hash<std::string>{}(key.session_identifier);
    const auto second = std::hash<std::uint64_t>{}(key.round_number);
    return first ^ (second << 1);
}

std::size_t FanCodeHash::operator()(const qingque::fan_code& code) const noexcept {
    return std::hash<std::string>{}(code.to_string());
}

bool RoundEntry::has_player(std::int64_t player_id) const {
    return std::any_of(players.begin(), players.end(), [player_id](const RoundPlayer& player) {
        return player.player_id == player_id;
    });
}

std::int64_t RoundEntry::winner_player_id() const {
    if (winner_seat < 0 || winner_seat >= static_cast<int>(players.size())) {
        return 0;
    }
    return players[static_cast<std::size_t>(winner_seat)].player_id;
}

std::int64_t RoundEntry::from_player_id() const {
    if (from_seat < 0 || from_seat >= static_cast<int>(players.size())) {
        return 0;
    }
    return players[static_cast<std::size_t>(from_seat)].player_id;
}

std::string_view RoundEntry::winner_username() const {
    if (winner_seat < 0 || winner_seat >= static_cast<int>(players.size())) {
        return {};
    }
    return players[static_cast<std::size_t>(winner_seat)].username;
}

std::string_view RoundEntry::from_username() const {
    if (from_seat < 0 || from_seat >= static_cast<int>(players.size())) {
        return {};
    }
    return players[static_cast<std::size_t>(from_seat)].username;
}

bool RoundEntry::self_drawn() const {
    return win_type(mahjong::win_type::self_drawn);
}

bool RoundEntry::has_win_type(mahjong::win_t flag) const {
    return win_type(flag);
}

int RoundEntry::pt_gain(std::int64_t player_id) const {
    if (drawn_game || player_id == 0) {
        return 0;
    }
    const int points = static_cast<int>(3 * std::round(fan * fan));
    if (winner_player_id() == player_id) {
        return points;
    }
    if (!self_drawn() && from_player_id() == player_id) {
        return -points;
    }
    return self_drawn() ? -static_cast<int>(std::round(fan * fan)) : 0;
}

int RoundEntry::player_meld_count(std::int64_t player_id) const {
    for (const auto& player : players) {
        if (player.player_id == player_id) {
            return meld_count[static_cast<std::size_t>(player.seat_index)];
        }
    }
    return 0;
}

double FanStatEntry::fan() const {
    return occurrences.empty() ? 0.0 : occurrences.front()->fan;
}

util::StatusOr<RoundEntry> ProjectRoundRecord(const Json::Value& record) {
    if (!record.isObject()) {
        return util::Status::InvalidArgument("round record must be a JSON object");
    }

    auto header = ReadRequiredObject(record, "header");
    if (!header.ok()) {
        return header.status();
    }

    auto session_identifier = ReadRequiredString(*header.value(), "session_identifier");
    if (!session_identifier.ok()) {
        return session_identifier.status();
    }
    auto round_number = ReadRequiredUInt64(*header.value(), "round_number");
    if (!round_number.ok()) {
        return round_number.status();
    }

    std::string duplicate_token;
    std::int64_t duplicate_session_number = -1;
    const Json::Value& duplicate_token_value = (*header.value())["duplicate_token"];
    if (duplicate_token_value.isString() && !duplicate_token_value.asString().empty()) {
        duplicate_token = duplicate_token_value.asString();
    }
    const Json::Value& duplicate_number_value = (*header.value())["duplicate_session_number"];
    if (duplicate_number_value.isInt64()) {
        duplicate_session_number = duplicate_number_value.asInt64();
    }

    auto initial_seats = ReadRequiredArray(record, "initial_seats");
    if (!initial_seats.ok()) {
        return initial_seats.status();
    }
    auto players = ParsePlayers(*initial_seats.value());
    if (!players.ok()) {
        return players.status();
    }

    auto round_result = ReadRequiredObject(record, "round_result");
    if (!round_result.ok()) {
        return round_result.status();
    }
    if (!(*round_result.value())["completed"].isBool()) {
        return util::Status::InvalidArgument("round_result.completed must be a bool");
    }
    if (!(*round_result.value())["completed"].asBool()) {
        return util::Status::InvalidArgument("round record is incomplete");
    }

    auto turn = ReadRequiredInt64(*round_result.value(), "turn");
    if (!turn.ok()) {
        return turn.status();
    }
    auto time_ms = ReadRequiredInt64(*round_result.value(), "time_ms");
    if (!time_ms.ok()) {
        return time_ms.status();
    }
    auto meld_count_json = ReadRequiredArray(*round_result.value(), "meld_count");
    if (!meld_count_json.ok()) {
        return meld_count_json.status();
    }
    if (meld_count_json.value()->size() != 4) {
        return util::Status::InvalidArgument("round_result.meld_count must contain four entries");
    }

    RoundEntry round_entry;
    round_entry.round_key = RoundKey{session_identifier.value(), round_number.value()};
    round_entry.duplicate_token = duplicate_token;
    round_entry.duplicate_session_number = duplicate_session_number;
    round_entry.players = players.value();
    round_entry.turn = turn.value();
    round_entry.timestamp_ms = time_ms.value();
    for (Json::ArrayIndex index = 0; index < meld_count_json.value()->size(); ++index) {
        const auto& count = (*meld_count_json.value())[index];
        if (!count.isInt()) {
            return util::Status::InvalidArgument("round_result.meld_count entries must be integers");
        }
        round_entry.meld_count[index] = count.asInt();
    }

    if (!(*round_result.value())["drawn_game"].isBool()) {
        return util::Status::InvalidArgument("round_result.drawn_game must be a bool");
    }
    round_entry.drawn_game = (*round_result.value())["drawn_game"].asBool();
    if (round_entry.drawn_game) {
        return round_entry;
    }

    auto winner_seat = ReadRequiredInt64(*round_result.value(), "winner_seat");
    if (!winner_seat.ok()) {
        return winner_seat.status();
    }
    auto from_seat = ReadRequiredInt64(*round_result.value(), "from_seat");
    if (!from_seat.ok()) {
        return from_seat.status();
    }
    auto win_type_bits = ReadRequiredUInt64(*round_result.value(), "win_type_bits");
    if (!win_type_bits.ok()) {
        return win_type_bits.status();
    }
    const Json::Value& win_tile = (*round_result.value())["win_tile"];
    if (!win_tile.isInt() && !win_tile.isUInt() && !win_tile.isInt64() && !win_tile.isUInt64()) {
        return util::Status::InvalidArgument("round_result.win_tile must be an integer");
    }

    round_entry.winner_seat = static_cast<int>(winner_seat.value());
    round_entry.from_seat = static_cast<int>(from_seat.value());
    round_entry.win_type = mahjong::win_type(static_cast<mahjong::win_t>(win_type_bits.value()));
    round_entry.win_tile = static_cast<mahjong::tile_t>(win_tile.asUInt());

    auto fan = ReadRequiredDouble(*round_result.value(), "fan");
    if (!fan.ok()) {
        return fan.status();
    }
    round_entry.fan = fan.value();

    const Json::Value& fan_results_json = (*round_result.value())["fan_results"];
    if (!fan_results_json.isArray()) {
        return util::Status::InvalidArgument("round_result.fan_results must be an array");
    }
    std::vector<qingque::fan_code> fan_results;
    fan_results.reserve(fan_results_json.size());
    for (const auto& entry : fan_results_json) {
        if (!entry.isString()) {
            return util::Status::InvalidArgument("round_result.fan_results must contain strings");
        }
        fan_results.emplace_back(entry.asString());
    }
    round_entry.fan_results = std::move(fan_results);

    round_entry.fan_ids = FanIdsFromResults(round_entry.fan_results);

    const Json::Value& winning_hand = (*round_result.value())["winning_hand"];
    if (!winning_hand.isNull()) {
        if (!winning_hand.isObject()) {
            return util::Status::InvalidArgument("round_result.winning_hand must be an object");
        }
        auto parsed_hand = game::HandWrapper::FromJson(winning_hand);
        if (!parsed_hand.ok()) {
            return parsed_hand.status();
        }
        round_entry.winning_hand = parsed_hand.value();
    }

    return round_entry;
}

}  // namespace mmcr::stats
