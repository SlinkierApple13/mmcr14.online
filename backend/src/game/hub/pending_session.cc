#include "game/hub/pending_session.h"

#include <algorithm>
#include <utility>

#include "game/hub/hub.h"

namespace mmcr::game {

auto PendingSession::is_empty_locked() const -> bool {
    if (game_config_.duplicate_mode) {
        return members_.empty() && std::none_of(seats_.begin(), seats_.end(), [](const PendingSeat& seat) {
            return seat.player.valid();
        });
    }
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
    if (game_config_.duplicate_mode) {
        members_.erase(std::remove_if(members_.begin(), members_.end(), [&removed_player_ids](const auto& member) {
            const bool invalid = !member.valid();
            if (invalid && member.player_id() != 0) {
                removed_player_ids.push_back(member.player_id());
            }
            return invalid;
        }), members_.end());
    }
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
        if (game_config_.duplicate_mode) {
            auto member_it = std::find_if(members_.begin(), members_.end(), [&player](const auto& member) {
                return member.matches(player.player_id());
            });
            if (member_it != members_.end()) {
                *member_it = player;
                // Refresh the profile on the chosen seat, if any.
                for (auto& seat : seats_) {
                    if (seat.player.matches(player.player_id())) {
                        seat.player = player;
                        seat.ready = false;
                        break;
                    }
                }
                empty_timeout_elapsed_ = false;
            } else {
                if (members_.size() >= seats_.size()) {
                    return util::Status::InvalidArgument("房间已满");
                }
                members_.push_back(player);
                empty_timeout_elapsed_ = false;
            }
        } else {
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
                    return util::Status::InvalidArgument("房间已满");
                }

                seat_it->player = player;
                seat_it->ready = false;
                empty_timeout_elapsed_ = false;
            }
        }
    }

    reset_empty_timer();
    return util::Status::Ok();
}

auto PendingSession::is_member(std::int64_t player_id) const -> bool {
    std::shared_lock lock(mutex_);
    return std::any_of(members_.begin(), members_.end(), [player_id](const auto& member) {
        return member.matches(player_id);
    });
}

auto PendingSession::chosen_seat_of(std::int64_t player_id) const -> std::optional<int> {
    std::shared_lock lock(mutex_);
    for (const auto& seat : seats_) {
        if (seat.player.matches(player_id)) {
            return seat.seat_index;
        }
    }
    return std::nullopt;
}

auto PendingSession::choose_seat(std::int64_t player_id, int seat_index) -> util::Status {
    if (!game_config_.duplicate_mode) {
        return util::Status::InvalidArgument("仅复式模式支持选择座位");
    }
    if (seat_index < 0 || seat_index >= static_cast<int>(seats_.size())) {
        return util::Status::InvalidArgument("无效的座位");
    }

    std::unique_lock lock(mutex_);
    const auto member_it = std::find_if(members_.begin(), members_.end(), [player_id](const auto& member) {
        return member.matches(player_id);
    });
    if (member_it == members_.end()) {
        return util::Status::NotFound("玩家不在等待房间中");
    }

    auto& target = seats_[seat_index];
    if (target.player.matches(player_id)) {
        // Un-choose: release the seat and clear readiness.
        target.player.reset();
        target.ready = false;
        return util::Status::Ok();
    }
    if (target.player.valid()) {
        return util::Status::InvalidArgument("该座位已被占用");
    }
    // Selecting another seat moves the player there directly.
    for (auto& seat : seats_) {
        if (seat.player.matches(player_id)) {
            seat.player.reset();
            seat.ready = false;
            break;
        }
    }

    target.player = *member_it;
    target.ready = false;
    return util::Status::Ok();
}

auto PendingSession::player_leaves(std::int64_t player_id) -> util::Status {
    std::unique_lock lock(mutex_);
    if (game_config_.duplicate_mode) {
        const auto member_it = std::find_if(members_.begin(), members_.end(), [player_id](const auto& member) {
            return member.matches(player_id);
        });
        if (member_it != members_.end()) {
            members_.erase(member_it);
        } else {
            return util::Status::NotFound("玩家不在等待房间中");
        }
    }
    for (auto& seat : seats_) {
        if (!seat.player.matches(player_id)) {
            continue;
        }

        seat.player.reset();
        seat.ready = false;
        return util::Status::Ok();
    }

    if (game_config_.duplicate_mode) {
        return util::Status::Ok();
    }
    return util::Status::NotFound("玩家不在等待房间中");
}

auto PendingSession::player_ready(std::int64_t player_id, bool ready) -> util::Status {
    std::unique_lock lock(mutex_);
    if (game_config_.duplicate_mode) {
        const auto member_it = std::find_if(members_.begin(), members_.end(), [player_id](const auto& member) {
            return member.matches(player_id);
        });
        if (member_it == members_.end()) {
            return util::Status::NotFound("玩家不在等待房间中");
        }
        if (ready) {
            auto seat_it = std::find_if(seats_.begin(), seats_.end(), [player_id](const PendingSeat& seat) {
                return seat.player.matches(player_id);
            });
            if (seat_it == seats_.end()) {
                return util::Status::InvalidArgument("请选择座位");
            }
            seat_it->ready = true;
        } else {
            for (auto& seat : seats_) {
                if (seat.player.matches(player_id)) {
                    seat.ready = false;
                    break;
                }
            }
        }
        return util::Status::Ok();
    }
    for (auto& seat : seats_) {
        if (!seat.player.matches(player_id)) {
            continue;
        }

        seat.ready = ready;
        return util::Status::Ok();
    }

    return util::Status::NotFound("玩家不在等待房间中");
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