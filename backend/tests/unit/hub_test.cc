#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "game/hub/hub.h"
#include "random/seed.h"

namespace mmcr::game {
namespace {

struct DeliveredMessage {
    std::int64_t player_id{0};
    Json::Value message{Json::objectValue};
    int delay_ms{0};
};

class RecordingTransport : public GameTransport {
public:
    void send_to_player(std::int64_t player_id,
                        const Json::Value& message,
                        int delay_ms) override {
        std::lock_guard lock(mutex_);
        delivered_.push_back(DeliveredMessage{
            .player_id = player_id,
            .message = message,
            .delay_ms = delay_ms,
        });
    }

    void clear() {
        std::lock_guard lock(mutex_);
        delivered_.clear();
    }

    [[nodiscard]] auto latest_message(std::int64_t player_id, std::string_view type) const
        -> std::optional<Json::Value> {
        std::lock_guard lock(mutex_);
        for (auto it = delivered_.rbegin(); it != delivered_.rend(); ++it) {
            if (it->player_id != player_id || !it->message.isObject()) {
                continue;
            }

            const Json::Value& message_type = it->message["type"];
            if (message_type.isString() && message_type.asString() == type) {
                return it->message;
            }
        }

        return std::nullopt;
    }

private:
    mutable std::mutex mutex_;
    std::vector<DeliveredMessage> delivered_;
};

auto MakePlayer(std::int64_t player_id, std::string username) -> auth::PlayerProfile {
    auth::PlayerProfile player;
    player.player_id = player_id;
    player.username = std::move(username);
    return player;
}

auto MakeLeaveMessage(std::int64_t session_id) -> Json::Value {
    Json::Value message(Json::objectValue);
    message["type"] = "session.leave";

    Json::Value payload(Json::objectValue);
    payload["session_id"] = Json::Int64(session_id);
    message["payload"] = std::move(payload);
    return message;
}

auto SeatPlayerId(const Json::Value& snapshot, Json::ArrayIndex seat_index)
    -> std::optional<std::int64_t> {
    if (!snapshot.isObject()) {
        return std::nullopt;
    }

    const Json::Value& payload = snapshot["payload"];
    if (!payload.isObject()) {
        return std::nullopt;
    }

    const Json::Value& seats = payload["seats"];
    if (!seats.isArray() || seat_index >= seats.size()) {
        return std::nullopt;
    }

    const Json::Value& player_id = seats[seat_index]["player_id"];
    if (player_id.isInt64()) {
        return player_id.asInt64();
    }
    if (player_id.isInt()) {
        return static_cast<std::int64_t>(player_id.asInt());
    }
    return std::nullopt;
}

TEST(GameHubTest, LeavingPendingSessionBroadcastsUpdatedSnapshot) {
    random::SeedContainer seed;
    RecordingTransport transport;
    GameHub hub(&seed, &transport);

    auto created = hub.create_session(CreateGameSessionRequest{
        .owner = MakePlayer(101, "Alpha"),
        .game_config = GameConfig{},
        .queue_config = QueueConfig{},
    });
    ASSERT_TRUE(created.ok()) << created.status().DebugString();

    const auto session_id = created.value().session_id;
    const auto join_status = hub.connect_player(ConnectPlayerRequest{
        .player = MakePlayer(102, "Beta"),
        .session_id = session_id,
    });
    ASSERT_TRUE(join_status.ok()) << join_status.DebugString();

    transport.clear();

    auto beta = std::make_shared<auth::PlayerProfile>(MakePlayer(102, "Beta"));
    auto status = hub.handle_message(RouteGameMessageRequest{
        .player = auth::PlayerProfilePtr(beta),
        .message = MakeLeaveMessage(session_id),
    });
    ASSERT_TRUE(status.ok()) << status.DebugString();

    const auto owner_snapshot = transport.latest_message(101, "session.snapshot");
    ASSERT_TRUE(owner_snapshot.has_value());
    EXPECT_EQ(101, SeatPlayerId(*owner_snapshot, 0).value_or(0));
    EXPECT_FALSE(SeatPlayerId(*owner_snapshot, 1).has_value());
    EXPECT_EQ(1, (*owner_snapshot)["payload"]["summary"]["occupied_seat_count"].asInt());
    EXPECT_EQ(0, (*owner_snapshot)["payload"]["summary"]["ready_seat_count"].asInt());
    EXPECT_FALSE(transport.latest_message(102, "session.snapshot").has_value());
}

TEST(GameHubTest, DisconnectingPendingSessionBroadcastsUpdatedSnapshot) {
    random::SeedContainer seed;
    RecordingTransport transport;
    GameHub hub(&seed, &transport);

    auto created = hub.create_session(CreateGameSessionRequest{
        .owner = MakePlayer(101, "Alpha"),
        .game_config = GameConfig{},
        .queue_config = QueueConfig{},
    });
    ASSERT_TRUE(created.ok()) << created.status().DebugString();

    const auto session_id = created.value().session_id;
    const auto join_status = hub.connect_player(ConnectPlayerRequest{
        .player = MakePlayer(102, "Beta"),
        .session_id = session_id,
    });
    ASSERT_TRUE(join_status.ok()) << join_status.DebugString();

    transport.clear();

    auto status = hub.disconnect_player(DisconnectPlayerRequest{.player_id = 102});
    ASSERT_TRUE(status.ok()) << status.DebugString();

    const auto owner_snapshot = transport.latest_message(101, "session.snapshot");
    ASSERT_TRUE(owner_snapshot.has_value());
    EXPECT_EQ(101, SeatPlayerId(*owner_snapshot, 0).value_or(0));
    EXPECT_FALSE(SeatPlayerId(*owner_snapshot, 1).has_value());
    EXPECT_EQ(1, (*owner_snapshot)["payload"]["summary"]["occupied_seat_count"].asInt());
}

TEST(GameHubTest, SwitchingPendingSessionsBroadcastsOldRoomSnapshot) {
    random::SeedContainer seed;
    RecordingTransport transport;
    GameHub hub(&seed, &transport);

    auto first = hub.create_session(CreateGameSessionRequest{
        .owner = MakePlayer(101, "Alpha"),
        .game_config = GameConfig{},
        .queue_config = QueueConfig{},
    });
    ASSERT_TRUE(first.ok()) << first.status().DebugString();

    const auto first_join = hub.connect_player(ConnectPlayerRequest{
        .player = MakePlayer(102, "Beta"),
        .session_id = first.value().session_id,
    });
    ASSERT_TRUE(first_join.ok()) << first_join.DebugString();

    auto second = hub.create_session(CreateGameSessionRequest{
        .owner = MakePlayer(103, "Gamma"),
        .game_config = GameConfig{},
        .queue_config = QueueConfig{},
    });
    ASSERT_TRUE(second.ok()) << second.status().DebugString();

    transport.clear();

    const auto switch_status = hub.connect_player(ConnectPlayerRequest{
        .player = MakePlayer(102, "Beta"),
        .session_id = second.value().session_id,
    });
    ASSERT_TRUE(switch_status.ok()) << switch_status.DebugString();

    const auto first_snapshot = transport.latest_message(101, "session.snapshot");
    ASSERT_TRUE(first_snapshot.has_value());
    EXPECT_EQ(101, SeatPlayerId(*first_snapshot, 0).value_or(0));
    EXPECT_FALSE(SeatPlayerId(*first_snapshot, 1).has_value());
    EXPECT_EQ(1, (*first_snapshot)["payload"]["summary"]["occupied_seat_count"].asInt());

    const auto second_snapshot = transport.latest_message(103, "session.snapshot");
    ASSERT_TRUE(second_snapshot.has_value());
    EXPECT_EQ(103, SeatPlayerId(*second_snapshot, 0).value_or(0));
    EXPECT_EQ(102, SeatPlayerId(*second_snapshot, 1).value_or(0));
    EXPECT_EQ(2, (*second_snapshot)["payload"]["summary"]["occupied_seat_count"].asInt());
}

TEST(GameHubTest, SessionIdsAreRandomizedWithinConfiguredRange) {
    random::SeedContainer seed;
    RecordingTransport transport;
    GameHub hub(&seed, &transport);

    std::unordered_set<std::int64_t> session_ids;
    for (std::int64_t index = 0; index < 64; ++index) {
        auto created = hub.create_session(CreateGameSessionRequest{
            .owner = MakePlayer(1000 + index, "Player" + std::to_string(index)),
            .game_config = GameConfig{},
            .queue_config = QueueConfig{},
        });
        ASSERT_TRUE(created.ok()) << created.status().DebugString();
        EXPECT_GE(created.value().session_id, 1);
        EXPECT_LE(created.value().session_id, 999999);
        EXPECT_TRUE(session_ids.insert(created.value().session_id).second);
    }
}

}  // namespace
}  // namespace mmcr::game