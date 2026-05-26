#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <jsoncpp/json/json.h>

#include "util/status.h"

namespace mmcr::storage {

struct GameRecordTask {
	std::string session_identifier;
	std::uint64_t round_number{0};
	Json::Value payload{Json::objectValue};
};

class GameRecordManager {
public:
	using WriteObserver = std::function<util::Status(const GameRecordTask& task)>;

	explicit GameRecordManager(std::filesystem::path records_root);
	~GameRecordManager();

	GameRecordManager(const GameRecordManager&) = delete;
	auto operator=(const GameRecordManager&) -> GameRecordManager& = delete;
	GameRecordManager(GameRecordManager&&) = delete;
	auto operator=(GameRecordManager&&) -> GameRecordManager& = delete;

	[[nodiscard]] auto Enqueue(GameRecordTask task) -> util::Status;
	void SetWriteObserver(WriteObserver observer);
	[[nodiscard]] auto records_root() const noexcept -> const std::filesystem::path& {
		return records_root_;
	}

private:
	void worker_loop();
	[[nodiscard]] auto write_task(const GameRecordTask& task) -> util::Status;

	std::filesystem::path records_root_;
	std::mutex mutex_;
	std::condition_variable condition_;
	std::deque<GameRecordTask> tasks_;
	WriteObserver write_observer_;
	bool shutdown_{false};
	std::thread worker_;
};

}  // namespace mmcr::storage