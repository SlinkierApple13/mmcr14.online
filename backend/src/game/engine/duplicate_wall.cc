#include "game/engine/duplicate_wall.h"

#include <algorithm>
#include <random>

#include "util/status.h"

namespace mmcr::game {

void DuplicateWall::prepare(std::vector<uint64_t> seeds,
                            const std::vector<mahjong::tile_t>& initial_tiles) {
    auto init_tiles = initial_tiles;
    if (init_tiles.empty()) {
        std::copy(mahjong::tile_set::all_tiles.begin(),
                  mahjong::tile_set::all_tiles.end(),
                  std::back_inserter(init_tiles));
    }
    if (seeds.empty()) {
        seeds.push_back(0);
    }
    std::mt19937_64 aux_rng(seeds[0]);
    std::shuffle(init_tiles.begin(), init_tiles.end(), aux_rng);
    for (std::size_t i = 0; i < 136; ++i) {
        tiles_[i] = init_tiles[i % init_tiles.size()];
    }
    for (const auto& seed : seeds) {
        std::mt19937_64 rng(seed);
        std::shuffle(tiles_.begin(), tiles_.end(), rng);
    }
    front_stack_indices_ = {0, 51, 34, 17};
    size_ = 136;
}

auto DuplicateWall::draw_front(int seat) -> util::StatusOr<mahjong::tile_t> {
    if (empty()) {
        return util::Status::InvalidArgument("No tiles left in the wall");
    }
    const auto normalized_seat = static_cast<std::size_t>(((seat % 4) + 4) % 4);
    auto& front = front_stack_indices_[normalized_seat];
    
    static constexpr std::array<std::size_t, 4> end_indices = {17, 0, 51, 34};
    if (front == end_indices[normalized_seat]) {
        return draw_front((seat + 3) % 4);
    }

    if (tiles_[front * 2] != mahjong::tile::invalid) {
        mahjong::tile_t tile = tiles_[front * 2];
        tiles_[front * 2] = mahjong::tile::invalid;
        --size_;
        return tile;
    }
    if (tiles_[front * 2 + 1] != mahjong::tile::invalid) {
        mahjong::tile_t tile = tiles_[front * 2 + 1];
        tiles_[front * 2 + 1] = mahjong::tile::invalid;
        front = (front + 1) % 68;
        --size_;
        return tile;
    }
    return draw_front((seat + 3) % 4);
}

auto DuplicateWall::draw(int seat, int count) -> util::StatusOr<std::vector<mahjong::tile_t>> {
    if (count < 0) {
        return util::Status::InvalidArgument("draw count must be non-negative");
    }
    std::vector<mahjong::tile_t> tiles;
    tiles.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        auto tile = draw_front(seat);
        if (!tile.ok()) {
            return tile.status();
        }
        tiles.push_back(tile.value());
    }
    return tiles;
}

auto DuplicateWall::size() const noexcept -> std::size_t {
    return size_;
}

auto DuplicateWall::empty() const noexcept -> bool {
    return size_ == 0;
}

auto DuplicateWall::tiles() const noexcept -> const std::array<mahjong::tile_t, 136>& {
    return tiles_;
}

auto DuplicateWall::front_stack_indices() const noexcept -> std::array<std::size_t, 4> {
    return front_stack_indices_;
}

}  // namespace mmcr::game
