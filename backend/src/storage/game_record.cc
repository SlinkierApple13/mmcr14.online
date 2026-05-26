#include "storage/game_record.h"

#include <fstream>
#include <iostream>
#include <utility>

namespace mmcr::storage {
namespace {

auto JsonToCompactString(const Json::Value& value) -> std::string {
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "";
	builder["commentStyle"] = "None";
	return Json::writeString(builder, value);
}

}  // namespace

GameRecordManager::GameRecordManager(std::filesystem::path records_root)
	: records_root_(std::move(records_root)),
	  worker_(&GameRecordManager::worker_loop, this) {}

GameRecordManager::~GameRecordManager() {
	{
		std::lock_guard lock(mutex_);
		shutdown_ = true;
	}
	condition_.notify_one();
	if (worker_.joinable()) {
		worker_.join();
	}
}

auto GameRecordManager::Enqueue(GameRecordTask task) -> util::Status {
	if (task.session_identifier.empty()) {
		return util::Status::InvalidArgument("session_identifier must not be empty");
	}
	if (task.round_number == 0) {
		return util::Status::InvalidArgument("round_number must be positive");
	}

	{
		std::lock_guard lock(mutex_);
		if (shutdown_) {
			return util::Status::Internal("game record manager is shutting down");
		}
		tasks_.push_back(std::move(task));
	}
	condition_.notify_one();
	return util::Status::Ok();
}

void GameRecordManager::SetWriteObserver(WriteObserver observer) {
	std::lock_guard lock(mutex_);
	write_observer_ = std::move(observer);
}

void GameRecordManager::worker_loop() {
	for (;;) {
		GameRecordTask task;
		{
			std::unique_lock lock(mutex_);
			condition_.wait(lock, [this]() { return shutdown_ || !tasks_.empty(); });
			if (shutdown_ && tasks_.empty()) {
				return;
			}
			task = std::move(tasks_.front());
			tasks_.pop_front();
		}

		const auto status = write_task(task);
		if (!status.ok()) {
			std::cerr << status.DebugString() << '\n';
			continue;
		}

		WriteObserver observer;
		{
			std::lock_guard lock(mutex_);
			observer = write_observer_;
		}
		if (!observer) {
			continue;
		}

		const auto observer_status = observer(task);
		if (!observer_status.ok()) {
			std::cerr << observer_status.DebugString() << '\n';
		}
	}
}

auto GameRecordManager::write_task(const GameRecordTask& task) -> util::Status {
	std::error_code error;
	const auto session_directory = records_root_ / task.session_identifier;
	std::filesystem::create_directories(session_directory, error);
	if (error) {
		return util::Status::Internal(
			"failed to create record directory: " + error.message());
	}

	const auto file_path = session_directory / (std::to_string(task.round_number) + ".json");
	std::ofstream stream(file_path, std::ios::out | std::ios::trunc);
	if (!stream.is_open()) {
		return util::Status::Internal("failed to open record file: " + file_path.string());
	}

	stream << JsonToCompactString(task.payload) << '\n';
	if (!stream.good()) {
		return util::Status::Internal("failed to write record file: " + file_path.string());
	}

	return util::Status::Ok();
}

}  // namespace mmcr::storage