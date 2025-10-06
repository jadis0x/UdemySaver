#pragma once

#include <functional>
#include <string>
#include <vector>

class FFmpegHelper {
public:
    static bool convert_m3u8_to_ts(
        const std::string& url,
        const std::string& out_path,
        const std::vector<std::string>& extra_headers,
        const std::string& proxy,
        std::function<void(double, double)> on_progress,
        std::string& msg);
};