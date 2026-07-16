#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <jsoncpp/json/json.h>

#include "external/qingque/basic/mahjong.h"
#include "util/status.h"
#include "util/status_or.h"

namespace mmcr::game {

struct MeldWrapper {
    mahjong::meld meld_value{mahjong::meld::invalid};
    int chow_mode{0}; // 0: none, 1: <1>23, 2: 1<2>3, 3: 12<3>
    int meld_from_rel{0}; // 0: none, 1: right, 2: opposite, 3: left

    [[nodiscard]] auto ToJson() const -> Json::Value {
        Json::Value payload(Json::objectValue);
        payload["meld_value"] = Json::UInt64(static_cast<Json::UInt64>(static_cast<mahjong::meld_t>(meld_value)));
        payload["tile"] = Json::UInt(static_cast<unsigned int>(meld_value.tile()));
        switch (meld_value.type()) {
            case mahjong::meld_type::sequence:
                payload["type"] = "sequence";
                break;
            case mahjong::meld_type::triplet:
                payload["type"] = "triplet";
                break;
            case mahjong::meld_type::kong:
                payload["type"] = "kong";
                break;
        }
        payload["concealed"] = meld_value.concealed();
        payload["fixed"] = meld_value.fixed();
        payload["chow_mode"] = chow_mode;
        payload["meld_from_rel"] = meld_from_rel;
        return payload;
    }

    [[nodiscard]] static auto FromJson(const Json::Value& json) -> util::StatusOr<MeldWrapper> {
        if (!json.isObject()) {
            return util::Status::InvalidArgument("meld wrapper must be an object");
        }

        MeldWrapper wrapper;
        const Json::Value& raw_value = json["meld_value"];
        if (raw_value.isUInt64() || raw_value.isUInt() || raw_value.isInt64() || raw_value.isInt()) {
            wrapper.meld_value = mahjong::meld(static_cast<mahjong::meld_t>(raw_value.asUInt64()));
        } else {
            const Json::Value& tile = json["tile"];
            const Json::Value& type = json["type"];
            if ((!tile.isUInt() && !tile.isInt() && !tile.isUInt64() && !tile.isInt64()) || !type.isString()) {
                return util::Status::InvalidArgument("meld wrapper requires meld_value or tile/type");
            }
            mahjong::meld_type meld_type = mahjong::meld_type::sequence;
            if (type.asString() == "sequence") {
                meld_type = mahjong::meld_type::sequence;
            } else if (type.asString() == "triplet") {
                meld_type = mahjong::meld_type::triplet;
            } else if (type.asString() == "kong") {
                meld_type = mahjong::meld_type::kong;
            } else {
                return util::Status::InvalidArgument("invalid meld type");
            }
            wrapper.meld_value = mahjong::meld(
                static_cast<mahjong::tile_t>(tile.asUInt()),
                meld_type,
                json["concealed"].isBool() ? json["concealed"].asBool() : false,
                json["fixed"].isBool() ? json["fixed"].asBool() : true);
        }
        wrapper.chow_mode = json["chow_mode"].isInt() ? json["chow_mode"].asInt() : 0;
        wrapper.meld_from_rel = json["meld_from_rel"].isInt() ? json["meld_from_rel"].asInt() : 0;
        return wrapper;
    }
};

struct HandWrapper {
    std::vector<MeldWrapper> melds{};
    std::vector<mahjong::tile_t> hand_tiles{};
    mahjong::tile_t winning_tile{mahjong::tile::invalid};
    mahjong::win_type winning_type{0};

    [[nodiscard]] auto ToJson() const -> Json::Value {
        Json::Value payload(Json::objectValue);
        payload["format"] = "mmcr.hand_wrapper.v1";
        Json::Value melds_json(Json::arrayValue);
        for (const auto& meld : melds) {
            melds_json.append(meld.ToJson());
        }
        payload["melds"] = std::move(melds_json);
        Json::Value hand_tiles_json(Json::arrayValue);
        for (const auto tile : hand_tiles) {
            hand_tiles_json.append(Json::UInt(static_cast<unsigned int>(tile)));
        }
        payload["hand_tiles"] = std::move(hand_tiles_json);
        payload["winning_tile"] = Json::UInt(static_cast<unsigned int>(winning_tile));
        payload["winning_type"] = Json::UInt64(static_cast<Json::UInt64>(static_cast<mahjong::win_t>(winning_type)));
        return payload;
    }

    [[nodiscard]] static auto FromJson(const Json::Value& json) -> util::StatusOr<HandWrapper> {
        if (!json.isObject()) {
            return util::Status::InvalidArgument("hand wrapper must be an object");
        }

        HandWrapper hand;
        const Json::Value& melds_json = json["melds"];
        if (!melds_json.isArray()) {
            return util::Status::InvalidArgument("hand wrapper melds must be an array");
        }
        hand.melds.reserve(melds_json.size());
        for (const auto& meld_json : melds_json) {
            auto meld = MeldWrapper::FromJson(meld_json);
            if (!meld.ok()) {
                return meld.status();
            }
            hand.melds.push_back(meld.value());
        }

        const Json::Value& hand_tiles_json = json["hand_tiles"];
        if (!hand_tiles_json.isArray()) {
            return util::Status::InvalidArgument("hand wrapper hand_tiles must be an array");
        }
        hand.hand_tiles.reserve(hand_tiles_json.size());
        for (const auto& tile : hand_tiles_json) {
            if (!tile.isUInt() && !tile.isInt() && !tile.isUInt64() && !tile.isInt64()) {
                return util::Status::InvalidArgument("hand wrapper hand_tiles must contain integers");
            }
            hand.hand_tiles.push_back(static_cast<mahjong::tile_t>(tile.asUInt()));
        }

        const Json::Value& winning_tile_json = json["winning_tile"];
        if (!winning_tile_json.isUInt() && !winning_tile_json.isInt() &&
            !winning_tile_json.isUInt64() && !winning_tile_json.isInt64()) {
            return util::Status::InvalidArgument("hand wrapper winning_tile must be an integer");
        }
        hand.winning_tile = static_cast<mahjong::tile_t>(winning_tile_json.asUInt());

        const Json::Value& winning_type_json = json["winning_type"];
        if (!winning_type_json.isUInt() && !winning_type_json.isInt() &&
            !winning_type_json.isUInt64() && !winning_type_json.isInt64()) {
            return util::Status::InvalidArgument("hand wrapper winning_type must be an integer");
        }
        hand.winning_type = mahjong::win_type(static_cast<mahjong::win_t>(winning_type_json.asUInt64()));
        return hand;
    }

    mahjong::hand get_base_hand() const {
        std::vector<mahjong::meld> base_melds;
        base_melds.reserve(melds.size());
        for (const auto& mw : melds) {
            base_melds.push_back(mw.meld_value);
        }
        return mahjong::hand(hand_tiles, base_melds, winning_tile, winning_type, false);
    }
};

}  // namespace mmcr::game