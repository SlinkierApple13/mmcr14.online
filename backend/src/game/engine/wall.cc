#include "game/engine/wall.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "util/status.h"

namespace mmcr::game {

void Wall::prepare(std::vector<uint64_t> seeds, 
                   const std::vector<mahjong::tile_t>& initial_tiles) { 
    auto init_tiles = initial_tiles;
    if (init_tiles.empty()) {
        std::copy(mahjong::tile_set::all_tiles.begin(), mahjong::tile_set::all_tiles.end(), std::back_inserter(init_tiles));
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
    std::array<int, 4> dices{1, 1, 1, 1};
    std::uniform_int_distribution<int> dist(1, 6);
    for (auto& dice : dices) {
        dice = dist(aux_rng);
    }
    front_stack_index_ = ((dices[0] + dices[1]) * 52 + (dices[2] + dices[3])) % 68;
    back_stack_index_ = (front_stack_index_ + 67) % 68;
    size_ = 136;
}

auto Wall::draw_front() -> util::StatusOr<mahjong::tile_t> {
    if (empty()) {
        return util::Status::InvalidArgument("No tiles left in the wall");
    }
    if (tiles_[front_stack_index_ * 2] != mahjong::tile::invalid) {
        mahjong::tile_t tile = tiles_[front_stack_index_ * 2];
        tiles_[front_stack_index_ * 2] = mahjong::tile::invalid;
        --size_;
        return tile;
    }
    mahjong::tile_t tile = tiles_[front_stack_index_ * 2 + 1];
    tiles_[front_stack_index_ * 2 + 1] = mahjong::tile::invalid;
    front_stack_index_ = (front_stack_index_ + 1) % 68;
    --size_;
    return tile;
}

auto Wall::draw_back() -> util::StatusOr<mahjong::tile_t> {
    if (empty()) {
        return util::Status::InvalidArgument("No tiles left in the wall");
    }
    if (tiles_[back_stack_index_ * 2] != mahjong::tile::invalid) {
        mahjong::tile_t tile = tiles_[back_stack_index_ * 2];
        tiles_[back_stack_index_ * 2] = mahjong::tile::invalid;
        --size_;
        return tile;
    }
    mahjong::tile_t tile = tiles_[back_stack_index_ * 2 + 1];
    tiles_[back_stack_index_ * 2 + 1] = mahjong::tile::invalid;
    back_stack_index_ = (back_stack_index_ + 67) % 68;
    --size_;
    return tile;
}

auto Wall::draw(int count) -> util::StatusOr<std::vector<mahjong::tile_t>> {
    if (std::abs(count) > size()) {
        return util::Status::InvalidArgument("Not enough tiles in the wall");
    }
    std::vector<mahjong::tile_t> drawn_tiles;
    drawn_tiles.reserve(std::abs(count));
    if (count > 0) {
        for (int i = 0; i < count; ++i) {
            drawn_tiles.push_back(draw_front().value());
        }
    } else {
        for (int i = 0; i < -count; ++i) {
            drawn_tiles.push_back(draw_back().value());
        }
    }
    return drawn_tiles;
}

auto Wall::size() const noexcept -> std::size_t {
    return size_;
}

auto Wall::empty() const noexcept -> bool {
    return size() == 0;
}

}  // namespace mmcr::game