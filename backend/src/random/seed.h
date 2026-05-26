#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "util/status_or.h"

namespace mmcr::random {

class SeedContainer {
public:
    explicit SeedContainer(std::size_t capacity = 256);

    void RecordTraffic();
    void RecordTraffic(std::uint64_t timestamp_ns);
    [[nodiscard]] std::uint64_t Extract();
    [[nodiscard]] std::uint64_t Extract(std::uint64_t timestamp_ns);
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::uint64_t last_extracted() const noexcept;

private:
    [[nodiscard]] std::uint64_t FallbackValue(std::uint64_t timestamp_ns) noexcept;

    mutable std::mutex mutex_;
    std::deque<std::uint64_t> queue_;
    std::size_t capacity_;
    std::uint64_t last_extracted_{0};
};

[[nodiscard]] util::StatusOr<std::vector<unsigned char>> DrawBytes(SeedContainer& container,
                                                                   std::size_t count);
[[nodiscard]] util::StatusOr<std::string> DrawHex(SeedContainer& container,
                                                  std::size_t byte_count);

}  // namespace mmcr::random