#pragma once

#include <cstddef>
#include <cstdint>

#include "external/qingque/basic/mahjong.h"
#include "util/status_or.h"

namespace mmcr::game {

class Wall {
public:
    void prepare(std::vector<uint64_t> seeds, 
                 const std::vector<mahjong::tile_t>& initial_tiles);

    [[nodiscard]] auto draw_front() -> util::StatusOr<mahjong::tile_t>;
    [[nodiscard]] auto draw_back() -> util::StatusOr<mahjong::tile_t>;
    [[nodiscard]] auto draw(int count) -> util::StatusOr<std::vector<mahjong::tile_t>>;
    [[nodiscard]] auto size() const noexcept -> std::size_t;
    [[nodiscard]] auto empty() const noexcept -> bool;

    auto operator==(const Wall&) const -> bool = default;

private:
    std::array<mahjong::tile_t, 136> tiles_{0};
    std::size_t front_stack_index_{0};
    std::size_t back_stack_index_{0};
    std::size_t size_{0};
};

}  // namespace mmcr::game