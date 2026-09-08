#pragma once

#include <cstddef>
#include <cstdint>

#include "external/qingque/basic/mahjong.h"
#include "util/status_or.h"

namespace mmcr::game {

class DuplicateWall {
public:
    void prepare(std::vector<uint64_t> seeds, 
                 const std::vector<mahjong::tile_t>& initial_tiles);

    [[nodiscard]] auto draw_front(int seat) -> util::StatusOr<mahjong::tile_t>;
    [[nodiscard]] auto draw(int seat, int count) -> util::StatusOr<std::vector<mahjong::tile_t>>;
    [[nodiscard]] auto size() const noexcept -> std::size_t;
    [[nodiscard]] auto empty() const noexcept -> bool;
    [[nodiscard]] auto tiles() const noexcept -> const std::array<mahjong::tile_t, 136>&;
    [[nodiscard]] auto front_stack_indices() const noexcept -> std::array<std::size_t, 4>;

    auto operator==(const DuplicateWall&) const -> bool = default;

private:
    std::array<mahjong::tile_t, 136> tiles_{0};
    std::array<std::size_t, 4> front_stack_indices_{0, 51, 34, 17};
    std::size_t size_{0};
};

}  // namespace mmcr::game
