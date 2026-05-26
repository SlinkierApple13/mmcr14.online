#include "game/hub/pending_session.h"

#include <algorithm>
#include <utility>

#include "game/hub/hub.h"

namespace mmcr::game {

auto PendingSession::is_empty_locked() const -> bool {
    return std::none_of(seats_.begin(), seats_.end(), [](const PendingSeat& seat) {
        return seat.player.valid();
    });
}

PendingSession::PendingSession(GameHub* hub,
                               std::int64_t session_id,
                               GameConfig game_config,
                               QueueConfig queue_config)
    : queue_config_(std::move(queue_config)),
      hub_(hub),
      session_id_(session_id),
      game_config_(std::move(game_config)) {
    for (std::size_t index = 0; index < seats_.size(); ++index) {
        seats_[index].seat_index = static_cast<int>(index);
    }
}

auto PendingSession::is_empty() const -> bool {
    std::shared_lock lock(mutex_);
    return is_empty_locked();
}

auto PendingSession::is_full() const -> bool {
    std::shared_lock lock(mutex_);
    return std::all_of(seats_.begin(), seats_.end(), [](const PendingSeat& seat) {
        return seat.player.valid();
    });
}

auto PendingSession::all_ready() const -> bool {
    std::shared_lock lock(mutex_);
    return std::all_of(seats_.begin(), seats_.end(), [](const PendingSeat& seat) {
        return seat.player.valid() && seat.ready;
    });
}

auto PendingSession::collect_invalid_players() -> std::vector<std::int64_t> {
    std::vector<std::int64_t> removed_player_ids;

    std::unique_lock lock(mutex_);
    for (auto& seat : seats_) {
        if (seat.player.valid() || seat.player.player_id() == 0) {
            continue;
        }

        removed_player_ids.push_back(seat.player.player_id());
        seat.player.reset();
        seat.ready = false;
    }

    return removed_player_ids;
}

auto PendingSession::empty_timeout_elapsed() const -> bool {
    std::shared_lock lock(mutex_);
    return empty_timeout_elapsed_;
}

auto PendingSession::join_player(auth::PlayerProfilePtr player) -> util::Status {
    if (!player.valid()) {
        return util::Status::InvalidArgument("player is required");
    }

    {
        std::unique_lock lock(mutex_);
        for (auto& seat : seats_) {
            if (seat.player.matches(player.player_id())) {
                seat.player = player;
                seat.ready = false;
                empty_timeout_elapsed_ = false;
                break;
            }
        }

        auto existing_it = std::find_if(seats_.begin(), seats_.end(), [&player](const PendingSeat& seat) {
            return seat.player.matches(player.player_id());
        });
        if (existing_it == seats_.end()) {
            auto seat_it = std::find_if(seats_.begin(), seats_.end(), [](const PendingSeat& seat) {
                return !seat.player.valid();
            });
            if (seat_it == seats_.end()) {
                return util::Status::InvalidArgument("session is full");
            }

            seat_it->player = player;
            seat_it->ready = false;
            empty_timeout_elapsed_ = false;
        }
    }

    reset_empty_timer();
    return util::Status::Ok();
}

auto PendingSession::player_leaves(std::int64_t player_id) -> util::Status {
    std::unique_lock lock(mutex_);
    for (auto& seat : seats_) {
        if (!seat.player.matches(player_id)) {
            continue;
        }

        seat.player.reset();
        seat.ready = false;
        return util::Status::Ok();
    }

    return util::Status::NotFound("player is not in pending session");
}

auto PendingSession::player_ready(std::int64_t player_id, bool ready) -> util::Status {
    std::unique_lock lock(mutex_);
    for (auto& seat : seats_) {
        if (!seat.player.matches(player_id)) {
            continue;
        }

        seat.ready = ready;
        return util::Status::Ok();
    }

    return util::Status::NotFound("player is not in pending session");
}

void PendingSession::ensure_empty_timer() {
    std::unique_lock lock(mutex_);
    if (!is_empty_locked() || empty_timeout_elapsed_ || empty_session_timer_.isRunning()) {
        return;
    }

    empty_session_timer_.set(queue_config_.empty_timeout_ms, [this] {
        std::unique_lock lock(mutex_);
        if (is_empty_locked()) {
            empty_timeout_elapsed_ = true;
        }
    });
}

void PendingSession::reset_empty_timer() {
    std::unique_lock lock(mutex_);
    empty_timeout_elapsed_ = false;
    empty_session_timer_.stop();
}

void PendingSession::send_message(std::int64_t player_id, const Json::Value& message) {
    if (hub_ == nullptr) {
        return;
    }
    hub_->send_to_player(player_id, message);
}

}  // namespace mmcr::game