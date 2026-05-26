#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <jsoncpp/json/json.h>

#include "auth/service.h"
#include "game/config.h"
#include "game/hub/pending_session.h"
#include "game/rating_snapshot.h"
#include "random/seed.h"
#include "util/status.h"
#include "util/status_or.h"

namespace mmcr::storage {
class GameRecordManager;
}

namespace mmcr::game {

class ActiveSession;

class GameTransport {
public:
	virtual ~GameTransport() = default;

	virtual void send_to_player(std::int64_t player_id,
								const Json::Value& message,
								int delay_ms = 0) = 0;

	virtual void on_session_ended(std::int64_t /*session_id*/,
								const std::array<std::int64_t, 4>& /*player_ids*/,
								const std::array<int, 4>& /*final_scores*/,
								int /*round_count*/) {}

	virtual auto get_player_ratings(const std::array<std::int64_t, 4>& player_ids)
		-> std::vector<PlayerRatingSnapshot> { (void)player_ids; return {}; }
};

struct CreateGameSessionRequest {
	auth::PlayerProfile owner;
	GameConfig game_config;
	QueueConfig queue_config;
};

struct CreateGameSessionResult {
	std::int64_t session_id{0};
};

struct ConnectPlayerRequest {
	auth::PlayerProfile player;
	std::optional<std::int64_t> session_id;
};

struct DisconnectPlayerRequest {
	std::int64_t player_id{0};
};

struct RouteGameMessageRequest {
	auth::PlayerProfilePtr player;
	Json::Value message{Json::objectValue};
};

struct ActiveSessionSummary {
	std::int64_t session_id{0};
	int primary_timer_ms{7000};
	int secondary_timer_ms{4000};
	int auxiliary_timer_ms{12000};
	int round_count{16};
	std::uint64_t round_counter{0};
	bool recorded{true};
	bool debug_mode{false};
	bool ended{false};
	bool public_session{true};
	std::vector<std::string> names;
};

class GameHub {
public:
	GameHub(random::SeedContainer* seed_container,
			GameTransport* transport,
			storage::GameRecordManager* record_manager = nullptr);
	~GameHub();

	[[nodiscard]] auto create_session(const CreateGameSessionRequest& request)
		-> util::StatusOr<CreateGameSessionResult>;
	[[nodiscard]] auto connect_player(const ConnectPlayerRequest& request) -> util::Status;
	[[nodiscard]] auto disconnect_player(const DisconnectPlayerRequest& request) -> util::Status;
	[[nodiscard]] auto handle_message(const RouteGameMessageRequest& request) -> util::Status;

	[[nodiscard]] auto list_joinable_sessions() const -> std::vector<PendingSessionSummary>;
	[[nodiscard]] auto list_active_sessions() const -> std::vector<ActiveSessionSummary>;
	[[nodiscard]] auto find_pending_session(std::int64_t session_id) const
		-> util::StatusOr<const PendingSession*>;
	[[nodiscard]] auto find_active_session(std::int64_t session_id) const
		-> util::StatusOr<const ActiveSession*>;
	[[nodiscard]] auto find_player_pending_session_id(std::int64_t player_id) const
		-> std::optional<std::int64_t>;
	[[nodiscard]] auto find_player_active_session_id(std::int64_t player_id) const
		-> std::optional<std::int64_t>;

	void register_anonymous_browser();
	void notify_session_lists_changed();
	void send_to_player(std::int64_t player_id, const Json::Value& message, int delay_ms = 0);
	void broadcast_to_players(const std::vector<std::int64_t>& player_ids,
							  const Json::Value& message,
							  int delay_ms = 0);

	[[nodiscard]] auto transport() -> GameTransport* { return transport_; }

private:
	[[nodiscard]] auto allocate_session_id_locked() -> util::StatusOr<std::int64_t>;
	[[nodiscard]] auto route_pending_message(const RouteGameMessageRequest& request,
											 PendingSession& session) -> util::Status;
	[[nodiscard]] auto route_active_message(const RouteGameMessageRequest& request,
											ActiveSession& session) -> util::Status;
	[[nodiscard]] auto start_active_session(std::int64_t session_id) -> util::Status;
	void garbage_collect_loop();
	void garbage_collect_active_sessions();
	void garbage_collect_pending_sessions();
	void broadcast_joinable_sessions();

	random::SeedContainer* seed_container_{nullptr};
	GameTransport* transport_{nullptr};
	storage::GameRecordManager* record_manager_{nullptr};
	mutable std::shared_mutex mutex_;
	std::mt19937_64 session_id_rng_;
	std::unordered_map<std::int64_t, std::shared_ptr<auth::PlayerProfile>> known_players_;
	std::unordered_map<std::int64_t, std::unique_ptr<PendingSession>> pending_sessions_;
	std::unordered_map<std::int64_t, std::unique_ptr<ActiveSession>> active_sessions_;
	std::unordered_map<std::int64_t, std::int64_t> active_session_all_afk_since_ms_;
	std::unordered_map<std::int64_t, std::int64_t> player_pending_sessions_;
	std::unordered_map<std::int64_t, std::int64_t> player_active_sessions_;
	std::unordered_set<std::int64_t> browsing_players_;
	std::mutex gc_mutex_;
	std::condition_variable gc_cv_;
	bool gc_shutdown_{false};
	std::thread gc_thread_;
};

}  // namespace mmcr::game