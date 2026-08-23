#include "game/hub/hub_internal.h"

#include <algorithm>
#include <chrono>

#include "game/engine/session.h"

namespace mmcr::game {

void GameHub::garbage_collect_loop() {
    std::unique_lock lock(gc_mutex_);
    while (!gc_shutdown_) {
        gc_cv_.wait_for(lock, std::chrono::seconds(1), [this] {
            return gc_shutdown_;
        });
        if (gc_shutdown_) {
            return;
        }

        lock.unlock();
        garbage_collect_active_sessions();
        garbage_collect_pending_sessions();
        lock.lock();
    }
}

void GameHub::garbage_collect_active_sessions() {
    const auto now_ms = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    std::vector<std::int64_t> expired_session_ids;
    std::vector<std::int64_t> removed_player_ids;
    bool sessions_changed = false;
    {
        std::unique_lock lock(mutex_);
        for (const auto& [session_id, session] : active_sessions_) {
            if (session == nullptr) {
                continue;
            }

            if (session->ended()) {
                active_session_all_afk_since_ms_.erase(session_id);
                if (now_ms - session->ended_at_ms() >= GameConfig::dead_time) {
                    expired_session_ids.push_back(session_id);
                    for (const auto& seat : session->seats()) {
                        const auto player = seat.player.lock();
                        if (player != nullptr && player->player_id > 0) {
                            removed_player_ids.push_back(player->player_id);
                        }
                    }
                }
                continue;
            }

            bool all_afk = true;
            for (const auto& seat : session->seats()) {
                if (!seat.is_afk() && !seat.disconnected) {
                    all_afk = false;
                    break;
                }
            }

            if (!all_afk) {
                active_session_all_afk_since_ms_.erase(session_id);
                continue;
            }

            auto& since_ms = active_session_all_afk_since_ms_[session_id];
            if (since_ms == 0) {
                since_ms = now_ms;
                continue;
            }

            if (now_ms - since_ms < GameConfig::afk_tolerance_ms) {
                continue;
            }

            session->end_session(now_ms);
            active_session_all_afk_since_ms_.erase(session_id);
            sessions_changed = true;
        }

        for (const auto session_id : expired_session_ids) {
            active_sessions_.erase(session_id);
            active_session_all_afk_since_ms_.erase(session_id);
        }
        for (auto it = player_active_sessions_.begin(); it != player_active_sessions_.end();) {
            if (std::find(expired_session_ids.begin(), expired_session_ids.end(), it->second) != expired_session_ids.end()) {
                it = player_active_sessions_.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto player_id : removed_player_ids) {
            browsing_players_.insert(player_id);
        }
    }

    if (sessions_changed || !expired_session_ids.empty()) {
        broadcast_joinable_sessions();
    }
}

void GameHub::garbage_collect_pending_sessions() {
    std::vector<std::int64_t> removed_player_ids;
    std::vector<std::int64_t> changed_session_ids;
    std::vector<std::int64_t> expired_session_ids;

    {
        std::shared_lock lock(mutex_);
        for (const auto& [session_id, session] : pending_sessions_) {
            auto invalid_players = session->collect_invalid_players();
            if (!invalid_players.empty()) {
                removed_player_ids.insert(
                    removed_player_ids.end(), invalid_players.begin(), invalid_players.end());
                changed_session_ids.push_back(session_id);
            }

            if (session->is_empty()) {
                session->ensure_empty_timer();
                if (session->empty_timeout_elapsed()) {
                    expired_session_ids.push_back(session_id);
                }
                continue;
            }

            session->reset_empty_timer();
        }
    }

    if (!removed_player_ids.empty()) {
        std::unique_lock lock(mutex_);
        for (const auto player_id : removed_player_ids) {
            auto it = player_pending_sessions_.find(player_id);
            if (it != player_pending_sessions_.end()) {
                player_pending_sessions_.erase(it);
            }
        }
    }

    bool removed_any_session = false;
    for (const auto session_id : expired_session_ids) {
        std::unique_lock lock(mutex_);
        auto it = pending_sessions_.find(session_id);
        if (it == pending_sessions_.end()) {
            continue;
        }
        if (!it->second->is_empty() || !it->second->empty_timeout_elapsed()) {
            continue;
        }

        for (auto pending_it = player_pending_sessions_.begin(); pending_it != player_pending_sessions_.end();) {
            if (pending_it->second == session_id) {
                pending_it = player_pending_sessions_.erase(pending_it);
            } else {
                ++pending_it;
            }
        }
        pending_sessions_.erase(it);
        removed_any_session = true;
    }

    for (const auto session_id : changed_session_ids) {
        std::shared_lock lock(mutex_);
        auto it = pending_sessions_.find(session_id);
        if (it == pending_sessions_.end()) {
            continue;
        }
        PendingSession* session = it->second.get();
        lock.unlock();
        BroadcastPendingSnapshot(*this, *session);
    }

    if (!removed_player_ids.empty() || removed_any_session) {
        broadcast_joinable_sessions();
    }
}

void GameHub::broadcast_joinable_sessions() {
    auto all_sessions = list_joinable_sessions();
    const auto active_sessions = list_active_sessions();

    // Only broadcast public sessions to browsing players. Players who join a
    // non-public session will see it via the game WS path, not via the lobby.
    std::vector<PendingSessionSummary> sessions;
    sessions.reserve(all_sessions.size());
    for (const auto& s : all_sessions) {
        if (s.public_session) {
            sessions.push_back(s);
        }
    }

    Json::Value payload(Json::objectValue);
    payload["sessions"] = SerializePendingSummaryList(sessions);
    payload["active_sessions"] = SerializeActiveSummaryList(active_sessions);
    const Json::Value envelope = BuildEnvelope("lobby.list.snapshot", std::move(payload));

    std::vector<std::int64_t> browsing_players;
    {
        std::shared_lock lock(mutex_);
        browsing_players.reserve(browsing_players_.size());
        for (const auto player_id : browsing_players_) {
            browsing_players.push_back(player_id);
        }
    }

    broadcast_to_players(browsing_players, envelope);
}

}  // namespace mmcr::game
