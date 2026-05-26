#include "random/seed.h"

#include <chrono>
#include <random>

namespace mmcr::random {

namespace {

auto CurrentNanoseconds() noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

auto GenerateValue(std::uint64_t seed) noexcept -> std::uint64_t {
    std::mt19937_64 generator(seed);
    return generator();
}

}  // namespace

SeedContainer::SeedContainer(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

void SeedContainer::RecordTraffic() {
    RecordTraffic(CurrentNanoseconds());
}

void SeedContainer::RecordTraffic(std::uint64_t timestamp_ns) {
    const std::uint64_t generated = GenerateValue(timestamp_ns);

    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() == capacity_) {
        queue_.pop_front();
    }
    queue_.push_back(generated);
}

std::uint64_t SeedContainer::Extract() {
    return Extract(CurrentNanoseconds());
}

std::uint64_t SeedContainer::Extract(std::uint64_t timestamp_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!queue_.empty()) {
        const std::uint64_t value = queue_.front();
        queue_.pop_front();
        last_extracted_ = value;
        return value;
    }

    last_extracted_ = FallbackValue(timestamp_ns);
    return last_extracted_;
}

std::size_t SeedContainer::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

std::size_t SeedContainer::capacity() const noexcept {
    return capacity_;
}

std::uint64_t SeedContainer::last_extracted() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_extracted_;
}

std::uint64_t SeedContainer::FallbackValue(std::uint64_t timestamp_ns) noexcept {
    return GenerateValue(timestamp_ns ^ last_extracted_);
}

util::StatusOr<std::vector<unsigned char>> DrawBytes(SeedContainer& container,
                                                     std::size_t count) {
    try {
        std::vector<unsigned char> bytes;
        bytes.reserve(count);

        while (bytes.size() < count) {
            std::uint64_t value = container.Extract();
            for (std::size_t byte_index = 0; byte_index < sizeof(value) && bytes.size() < count;
                 ++byte_index) {
                bytes.push_back(static_cast<unsigned char>((value >> (byte_index * 8U)) & 0xffU));
            }
        }

        return bytes;
    } catch (const std::exception& exception) {
        return util::Status::Internal(exception.what());
    }
}

util::StatusOr<std::string> DrawHex(SeedContainer& container, std::size_t byte_count) {
    static constexpr char kHexDigits[] = "0123456789abcdef";

    auto bytes = DrawBytes(container, byte_count);
    if (!bytes.ok()) {
        return bytes.status();
    }

    try {
        std::string output;
        output.reserve(byte_count * 2);

        for (unsigned char byte : bytes.value()) {
            output.push_back(kHexDigits[byte >> 4U]);
            output.push_back(kHexDigits[byte & 0x0fU]);
        }

        return output;
    } catch (const std::exception& exception) {
        return util::Status::Internal(exception.what());
    }
}

}  // namespace mmcr::random