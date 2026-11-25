#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace sdvx {

// File-name snapshot for extracted resources. A directory is enumerated only
// when queried; audio/model contents are still opened normally when used.
// Resource installation already requires restarting the application.
class AssetDirectory {
public:
    explicit AssetDirectory(std::filesystem::path root) : root_(std::move(root)) {}

    // The platform asset entry point validates the relative path first.
    bool contains(std::string_view relative) {
        if (root_.empty()) return false;
        const std::filesystem::path path(relative);
        const auto parent = path.parent_path().generic_string();
        std::lock_guard lock(mutex_);
        auto [directory, inserted] = directories_.try_emplace(parent);
        if (inserted) {
            std::error_code error;
            const std::filesystem::directory_iterator end;
            for (std::filesystem::directory_iterator it(root_ / parent, error); it != end; it.increment(error)) {
                if (it->is_regular_file(error)) directory->second.insert(it->path().filename().string());
            }
        }
        return directory->second.contains(path.filename().string());
    }

private:
    std::filesystem::path root_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::unordered_set<std::string>> directories_;
};

} // namespace sdvx
