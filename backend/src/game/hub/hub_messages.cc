#include "game/hub/hub_internal.h"

#include <charconv>
#include <string>
#include <utility>

#include "game/engine/session.h"

namespace mmcr::game {

std::optional<std::string> FindMessageType(const Json::Value& message) {
    if (!message.isObject()) {
        return std::nullopt;
    }

    const Json::Value& value = message["type"];
    if (!value.isString()) {
        return std::nullopt;
    }
    return value.asString();
}

util::StatusOr<std::int64_t> ReadRequiredInt64(const Json::Value& object, std::string_view name) {
    if (!object.isObject()) {
        return util::Status::InvalidArgument("payload must be a JSON object");
    }

    const Json::Value& value = object[std::string(name)];
    if (value.isInt64()) {
        return value.asInt64();
    }
    if (value.isInt()) {
        return static_cast<std::int64_t>(value.asInt());
    }
    if (value.isString()) {
        std::int64_t parsed_value = 0;
        const std::string raw_value = value.asString();
        const auto result = std::from_chars(
            raw_value.data(), raw_value.data() + raw_value.size(), parsed_value);
        if (result.ec == std::errc() && result.ptr == raw_value.data() + raw_value.size()) {
            return parsed_value;
        }
    }

    return util::Status::InvalidArgument(std::string(name) + " must be an integer");
}

util::StatusOr<std::optional<std::int64_t>> ReadOptionalInt64(const Json::Value& object,
                                                              std::string_view name) {
    if (!object.isObject()) {
        return util::Status::InvalidArgument("payload must be a JSON object");
    }

    const std::string key(name);
    if (!object.isMember(key) || object[key].isNull()) {
        return std::optional<std::int64_t>{};
    }

    const Json::Value& value = object[key];
    if (value.isInt64()) {
        return std::optional<std::int64_t>(value.asInt64());
    }
    if (value.isInt()) {
        return std::optional<std::int64_t>(static_cast<std::int64_t>(value.asInt()));
    }
    if (value.isString()) {
        std::int64_t parsed_value = 0;
        const std::string raw_value = value.asString();
        const auto result = std::from_chars(
            raw_value.data(), raw_value.data() + raw_value.size(), parsed_value);
        if (result.ec == std::errc() && result.ptr == raw_value.data() + raw_value.size()) {
            return std::optional<std::int64_t>(parsed_value);
        }
    }

    return util::Status::InvalidArgument(std::string(name) + " must be an integer");
}

util::StatusOr<bool> ReadRequiredBool(const Json::Value& object, std::string_view name) {
    if (!object.isObject()) {
        return util::Status::InvalidArgument("payload must be a JSON object");
    }

    const Json::Value& value = object[std::string(name)];
    if (!value.isBool()) {
        return util::Status::InvalidArgument(std::string(name) + " must be a boolean");
    }
    return value.asBool();
}

std::shared_ptr<auth::PlayerProfile> UpsertKnownPlayer(
    std::unordered_map<std::int64_t, std::shared_ptr<auth::PlayerProfile>>& known_players,
    const auth::PlayerProfile& player) {
    auto& slot = known_players[player.player_id];
    if (slot == nullptr) {
        slot = std::make_shared<auth::PlayerProfile>(player);
    } else {
        *slot = player;
    }
    return slot;
}

Json::Value BuildResumeRequiredEnvelope(std::int64_t session_id) {
    Json::Value payload(Json::objectValue);
    payload["session_id"] = Json::Int64(session_id);
    return BuildEnvelope("resume.required", std::move(payload));
}

PendingSessionSummary BuildPendingSummary(const PendingSession& session) {
    int occupied_seat_count = 0;
    int ready_seat_count = 0;
    for (const auto& seat : session.seats()) {
        if (seat.player.valid()) {
            ++occupied_seat_count;
            if (seat.ready) {
                ++ready_seat_count;
            }
        }
    }

    std::vector<std::string> names;
    for (const auto& seat : session.seats()) {
        auto player = seat.player.lock();
        if (player) {
            names.push_back(player->username);
        } else {
            names.push_back("");
        }
    }

    return PendingSessionSummary{
        .session_id = session.session_id(),
        .occupied_seat_count = occupied_seat_count,
        .ready_seat_count = ready_seat_count,
        .primary_timer_ms = session.game_config().primary_timer_ms,
        .secondary_timer_ms = session.game_config().secondary_timer_ms,
        .auxiliary_timer_ms = session.game_config().auxiliary_timer_ms,
        .round_count = session.game_config().round_count,
        .forced_end_floor = session.game_config().forced_end_floor,
        .recorded = session.game_config().recorded,
        .debug_mode = session.game_config().debug_mode,
        .public_session = session.game_config().public_session,
        .can_join = occupied_seat_count < static_cast<int>(session.seats().size()),
        .can_start = occupied_seat_count == static_cast<int>(session.seats().size()) &&
                     ready_seat_count == static_cast<int>(session.seats().size()),
        .mode = std::string(GameModeName(session.game_config().mode)),
        .mode_name = std::string(ModeDisplayName(session.game_config().mode)),
        .names = std::move(names),
    };
}

Json::Value SerializePendingSummary(const PendingSessionSummary& summary) {
    Json::Value payload(Json::objectValue);
    payload["session_id"] = Json::Int64(summary.session_id);
    payload["occupied_seat_count"] = summary.occupied_seat_count;
    payload["ready_seat_count"] = summary.ready_seat_count;
    payload["primary_timer_ms"] = summary.primary_timer_ms;
    payload["secondary_timer_ms"] = summary.secondary_timer_ms;
    payload["auxiliary_timer_ms"] = summary.auxiliary_timer_ms;
    payload["round_count"] = summary.round_count;
    payload["forced_end_floor"] = summary.forced_end_floor.has_value()
        ? Json::Value(*summary.forced_end_floor)
        : Json::Value(Json::nullValue);
    payload["recorded"] = summary.recorded;
    payload["debug_mode"] = summary.debug_mode;
    payload["public_session"] = summary.public_session;
    payload["can_join"] = summary.can_join;
    payload["can_start"] = summary.can_start;
    payload["mode"] = summary.mode;
    payload["mode_name"] = summary.mode_name;
    Json::Value names(Json::arrayValue);
    for (const auto& name : summary.names) {
        names.append(name);
    }
    payload["names"] = std::move(names);
    return payload;
}

Json::Value SerializePendingSummaryList(const std::vector<PendingSessionSummary>& sessions) {
    Json::Value payload(Json::arrayValue);
    for (const auto& session : sessions) {
        payload.append(SerializePendingSummary(session));
    }
    return payload;
}

Json::Value SerializeActiveSummaryList(const std::vector<ActiveSessionSummary>& sessions) {
    Json::Value payload(Json::arrayValue);
    for (const auto& session : sessions) {
        Json::Value entry(Json::objectValue);
        entry["session_id"] = Json::Int64(session.session_id);
        entry["primary_timer_ms"] = session.primary_timer_ms;
        entry["secondary_timer_ms"] = session.secondary_timer_ms;
        entry["auxiliary_timer_ms"] = session.auxiliary_timer_ms;
        entry["round_count"] = session.round_count;
        entry["forced_end_floor"] = session.forced_end_floor.has_value()
            ? Json::Value(*session.forced_end_floor)
            : Json::Value(Json::nullValue);
        entry["round_counter"] = Json::UInt64(session.round_counter);
        entry["recorded"] = session.recorded;
        entry["debug_mode"] = session.debug_mode;
        entry["ended"] = session.ended;
        entry["public_session"] = session.public_session;
        entry["mode"] = session.mode;
        entry["mode_name"] = session.mode_name;
        Json::Value names(Json::arrayValue);
        for (const auto& name : session.names) {
            names.append(name);
        }
        entry["names"] = std::move(names);
        payload.append(std::move(entry));
    }
    return payload;
}

Json::Value SerializePendingSeat(const PendingSeat& seat) {
    Json::Value payload(Json::objectValue);
    payload["seat_index"] = seat.seat_index;
    payload["ready"] = seat.ready;
    payload["team"] = seat.team < 0 ? Json::Value(Json::nullValue) : Json::Value(seat.team);
    const auto player = seat.player.lock();
    if (player != nullptr) {
        payload["player_id"] = Json::Int64(player->player_id);
        payload["username"] = player->username;
    } else {
        payload["player_id"] = Json::Value(Json::nullValue);
        payload["username"] = Json::Value(Json::nullValue);
    }
    return payload;
}

PendingSessionSnapshot BuildPendingSnapshot(const PendingSession& session) {
    return PendingSessionSnapshot{
        .summary = BuildPendingSummary(session),
        .seats = session.seats(),
    };
}

Json::Value SerializePendingSnapshot(const PendingSessionSnapshot& snapshot) {
    Json::Value payload(Json::objectValue);
    payload["phase"] = "pending";
    payload["summary"] = SerializePendingSummary(snapshot.summary);

    Json::Value seats(Json::arrayValue);
    for (const auto& seat : snapshot.seats) {
        seats.append(SerializePendingSeat(seat));
    }
    payload["seats"] = std::move(seats);
    return payload;
}

void BroadcastPendingSnapshot(GameHub& hub, const PendingSession& session) {
    const auto snapshot = BuildPendingSnapshot(session);
    Json::Value payload = SerializePendingSnapshot(snapshot);
    
    // Include ratings for all players in the session
    std::array<std::int64_t, 4> player_ids{};
    const auto& seats = session.seats();
    for (std::size_t i = 0; i < seats.size(); ++i) {
        const auto player = seats[i].player.lock();
        player_ids[i] = (player != nullptr) ? player->player_id : 0;
    }
    if (auto* transport = hub.transport(); transport != nullptr) {
        auto ratings = transport->get_player_ratings(player_ids);
        if (!ratings.empty()) {
            Json::Value ratings_arr(Json::arrayValue);
            for (const auto& r : ratings) {
                ratings_arr.append(r.ToJson());
            }
            payload["ratings"] = std::move(ratings_arr);
        }
    }
    
    const Json::Value envelope = BuildEnvelope("session.snapshot", std::move(payload));
    for (const auto& seat : snapshot.seats) {
        const auto player = seat.player.lock();
        if (player != nullptr) {
            hub.send_to_player(player->player_id, envelope);
        }
    }
}

ActiveSessionSummary BuildActiveSummary(const ActiveSession& session) {
    ActiveSessionSummary summary;
    summary.session_id = session.session_id();
    summary.primary_timer_ms = session.config().primary_timer_ms;
    summary.secondary_timer_ms = session.config().secondary_timer_ms;
    summary.auxiliary_timer_ms = session.config().auxiliary_timer_ms;
    summary.round_count = session.config().round_count;
    summary.forced_end_floor = session.config().forced_end_floor;
    summary.round_counter = session.state().round_counter;
    summary.recorded = session.config().recorded;
    summary.debug_mode = session.config().debug_mode;
    summary.ended = session.ended();
    summary.public_session = session.public_session();
    summary.mode = std::string(GameModeName(session.config().mode));
    summary.mode_name = std::string(ModeDisplayName(session.config().mode));
    for (const auto& seat : session.seats()) {
        const auto player = seat.player.lock();
        if (player != nullptr) {
            summary.names.push_back(player->username);
        }
    }
    return summary;
}

}  // namespace mmcr::game
