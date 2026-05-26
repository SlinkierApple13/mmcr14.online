#include "qingque.h"
#include "w_data.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

namespace qingque_wd {
    namespace {

    auto resolve_fan_cache_path() -> std::filesystem::path {
        if (const char* configured = std::getenv("MMCR_QINGQUE_FAN_CACHE");
            configured != nullptr && *configured != '\0') {
            return configured;
        }
        // by default, look for fan_cache.json in the same folder as this code
        return std::filesystem::path(__FILE__).parent_path() / "fan_cache.json";
    }

    auto extract_string_field(std::string_view object,
                              std::string_view key) -> std::optional<std::string> {
        const std::string needle = "\"" + std::string(key) + "\"";
        const auto key_pos = object.find(needle);
        if (key_pos == std::string_view::npos) {
            return std::nullopt;
        }

        const auto colon_pos = object.find(':', key_pos + needle.size());
        if (colon_pos == std::string_view::npos) {
            return std::nullopt;
        }

        const auto quote_start = object.find('"', colon_pos + 1);
        if (quote_start == std::string_view::npos) {
            return std::nullopt;
        }

        const auto quote_end = object.find('"', quote_start + 1);
        if (quote_end == std::string_view::npos) {
            return std::nullopt;
        }

        return std::string(object.substr(quote_start + 1, quote_end - quote_start - 1));
    }

    auto extract_number_field(std::string_view object,
                              std::string_view key) -> std::optional<double> {
        const std::string needle = "\"" + std::string(key) + "\"";
        const auto key_pos = object.find(needle);
        if (key_pos == std::string_view::npos) {
            return std::nullopt;
        }

        const auto colon_pos = object.find(':', key_pos + needle.size());
        if (colon_pos == std::string_view::npos) {
            return std::nullopt;
        }

        const auto value_start = object.find_first_of("-0123456789", colon_pos + 1);
        if (value_start == std::string_view::npos) {
            return std::nullopt;
        }

        const auto value_end = object.find_first_not_of("+-.0123456789eE", value_start);
        try {
            return std::stod(std::string(object.substr(value_start, value_end - value_start)));
        } catch (...) {
            return std::nullopt;
        }
    }

    auto load_fan_cache_entries(std::string_view contents,
                                qingque::w_data& wd) -> bool {
        auto fan_cache_pos = contents.find("\"fan_cache\"");
        if (fan_cache_pos == std::string_view::npos) {
            std::cerr << "Missing 'fan_cache' in fan_cache.json.\n";
            return false;
        }

        auto array_start = contents.find('[', fan_cache_pos);
        auto cursor = contents.find('{', array_start);
        bool inserted_any = false;
        while (cursor != std::string_view::npos) {
            const auto object_end = contents.find('}', cursor);
            if (object_end == std::string_view::npos) {
                break;
            }

            const auto object = contents.substr(cursor, object_end - cursor + 1);
            auto code = extract_string_field(object, "code");
            auto value = extract_number_field(object, "value");
            if (code.has_value() && value.has_value()) {
                wd.fan_cache[qingque::fan_code(*code)] = *value;
                inserted_any = true;
            }

            cursor = contents.find('{', object_end + 1);
        }

        if (!inserted_any) {
            std::cerr << "No usable fan_cache entries found in fan_cache.json.\n";
        }
        return inserted_any;
    }

    }  // namespace

    const qingque::w_data& get_wd() {
        static qingque::w_data wd;
        // if fan_cache is already populated, assume wd is already initialized
        if (!wd.fan_cache.empty()) {
            return wd;
        }

        const auto fan_cache_path = resolve_fan_cache_path();
        std::ifstream fan_cache_file(fan_cache_path, std::ios::in);
        if (!fan_cache_file.is_open()) {
            std::cerr << "Cannot open fan_cache.json at '" << fan_cache_path.string()
                      << "'. No cached fan data available.\n";
            return wd;
        }

        std::string fan_cache_contents((std::istreambuf_iterator<char>(fan_cache_file)), std::istreambuf_iterator<char>());
        static_cast<void>(load_fan_cache_entries(fan_cache_contents, wd));
        return wd;
    }

    std::unordered_map<qingque::fan_code, double> fan_cache(const qingque::w_data& wd) {
        return wd.fan_cache;
    }
}
