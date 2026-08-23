#include "auth/auth_internal.h"

#include <argon2.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace mmcr::auth {

Argon2Hasher::Argon2Hasher(Argon2Config config, random::SeedContainer* seed_container)
    : config_(config), seed_container_(seed_container) {}

util::StatusOr<std::string> Argon2Hasher::Hash(std::string_view value) const {
    auto salt = RandomBytes(config_.salt_bytes);
    if (!salt.ok()) {
        return salt.status();
    }

    std::array<char, 512> encoded{};
    const int error = argon2id_hash_encoded(
        config_.time_cost,
        config_.memory_cost_kib,
        config_.parallelism,
        value.data(),
        value.size(),
        salt.value().data(),
        salt.value().size(),
        config_.hash_bytes,
        encoded.data(),
        encoded.size());
    if (error != ARGON2_OK) {
        return util::Status::Internal(argon2_error_message(error));
    }

    return std::string(encoded.data());
}

util::Status Argon2Hasher::Verify(std::string_view encoded_hash, std::string_view value) const {
    const int error = argon2id_verify(encoded_hash.data(), value.data(), value.size());
    if (error == ARGON2_OK) {
        return util::Status::Ok();
    }
    if (error == ARGON2_VERIFY_MISMATCH) {
        return util::Status::InvalidArgument("secret mismatch");
    }

    return util::Status::Internal(argon2_error_message(error));
}

util::StatusOr<std::vector<unsigned char>> Argon2Hasher::RandomBytes(
    std::size_t count) const {
    return random::DrawBytes(*seed_container_, count);
}

std::string Normalize(std::string_view value) {
    std::string normalized(value);
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized;
}

void ObserveTraffic(random::SeedContainer* seed_container) {
    if (seed_container != nullptr) {
        seed_container->RecordTraffic();
    }
}

util::StatusOr<TokenParts> MakeOpaqueToken(random::SeedContainer* seed_container,
                     std::size_t id_bytes,
                     std::size_t secret_bytes) {
    auto id = random::DrawHex(*seed_container, id_bytes);
    if (!id.ok()) {
        return id.status();
    }

    auto secret = random::DrawHex(*seed_container, secret_bytes);
    if (!secret.ok()) {
        return secret.status();
    }

    return TokenParts{std::move(id.value()), std::move(secret.value())};
}

util::StatusOr<TokenParts> ParseOpaqueToken(std::string_view token) {
    const auto separator = token.find('.');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= token.size()) {
        return util::Status::InvalidArgument("token must use id.secret format");
    }

    return TokenParts{
        std::string(token.substr(0, separator)),
        std::string(token.substr(separator + 1)),
    };
}

util::Status ValidateRegistration(const RegisterRequest& request) {
    if (request.username.empty()) {
        return util::Status::InvalidArgument("username must not be empty");
    }
    if (request.username.size() > 30) {
        return util::Status::InvalidArgument("username must be at most 30 characters");
    }
    if (request.password.empty()) {
        return util::Status::InvalidArgument("password must not be empty");
    }
    if (request.password.size() > 30) {
        return util::Status::InvalidArgument("password must be at most 30 characters");
    }
    auto is_alphanumeric = [](std::string_view s) {
        for (const unsigned char c : s) {
            if (!std::isalnum(c)) {
                return false;
            }
        }
        return true;
    };
    if (!is_alphanumeric(request.password)) {
        return util::Status::InvalidArgument("password must be alphanumeric");
    }

    return util::Status::Ok();
}

util::Status ValidatePasswordUpdate(std::string_view password) {
    if (password.empty()) {
        return util::Status::InvalidArgument("password must not be empty");
    }

    return util::Status::Ok();
}

}  // namespace mmcr::auth
