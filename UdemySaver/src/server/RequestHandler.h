#pragma once

#include <string>
#include <utility>
#include <vector>
#include <boost/beast/http/status.hpp>
#include <mutex>
#include <functional>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <unordered_set>

struct HeaderProbe {
	long long content_length = -1;       // Content-Length
	long long content_range_total = -1;  // Content-Range: */TOTAL
};

class RequestHandler {
public:
	struct CourseProgress {
		std::string title;
		int total = 0; 
		int done = 0; 
	};

	struct Job {
		uint64_t id = 0;
		std::string url;
		std::string filename;
		std::vector<std::string> headers;

		int  course_id = 0;
		std::string course_title;
		std::string out_path_dir;
		std::string out_path;

		int section_index = 0;
		std::string section_title;
		int lecture_index = 0;
		std::string lecture_title;

		enum class State { Queued, Downloading, Done, Failed, Paused };
		State state = State::Queued;

		double progress = 0.0;
		std::string message;

		long long bytes_now = 0;
		long long bytes_total = 0;
		double    speed_bps = 0.0;
	};

	explicit RequestHandler(std::string webroot);
	~RequestHandler();

	// static files&helpers need this
	const std::string& webroot() const noexcept { return webroot_; }

	// GET /session  -> returns user info if token exists in settings.ini
	std::pair<boost::beast::http::status, std::string> handleSession();

	std::pair<boost::beast::http::status, std::string>
		handleSettingsUpdate(const std::string& body);

	std::pair<boost::beast::http::status, std::string> handleCourses(int page, int page_size);

	std::pair<boost::beast::http::status, std::string> handleDownloadRaw(const std::string& body);

	std::pair<boost::beast::http::status, std::string>
		handleLectures(int course_id, int page, int page_size = 100);

	std::pair<boost::beast::http::status, std::string> handleQueueAdd(const std::string& body);

	std::pair<boost::beast::http::status, std::string> handleQueueList();

	std::pair<boost::beast::http::status, std::string> handleQueuePause(const std::string& body);

	std::pair<boost::beast::http::status, std::string> handleQueueResume(const std::string& body);

	std::pair<boost::beast::http::status, std::string> handleReconcile(const std::string& target);

	std::pair<boost::beast::http::status, std::string> handleEstimate(const std::string& target);

	static size_t header_probe_cb(char* buffer, size_t size, size_t nitems, void* userdata);
	bool probe_content_length(const std::string& url,
							  const std::vector<std::string>& headers,
							  long long& out_bytes, std::string& err);

	static std::string pick_from_asset_for_size(const nlohmann::json& a, const std::string& pref);

	bool curl_download_file(const std::string& url,
							const std::string& out_path,
							const std::vector<std::string>& extra_headers,
							std::function<void(double, double)> on_progress,
							std::string& msg);

private:
	// settings
	void load_settings();
	static std::string trim(std::string s);
	static std::string read_file_utf8(const std::string& path);

	// Udemy API GET with bearer token from settings.ini
	std::string udemy_get(const std::string& url, long timeout_ms = 15000);


	std::string resolve_lecture_stream(int course_id, int lecture_id,
									   const std::string& prefer_quality = "Auto");

	std::string resolve_supplementary_asset(int course_id, int lecture_id, int asset_id);

	void worker_loop();

private:
	std::string webroot_;
	std::string token_;     // settings: udemy_access_token / access_token
	std::string api_base_;  // settings: udemy_api_base (default https://www.udemy.com)
	std::string proxy_;     // settings: http_proxy (optional)
	bool download_subtitles_ = true; // settings: download_subtitles
	bool download_assets_ = true; // settings: download_assets

	// queue
	std::mutex mtx_;
	std::condition_variable cv_;
	std::vector<Job> queue_;
	std::atomic<uint64_t> next_id_{ 1 };
	std::thread worker_;
	bool stop_ = false;
	bool running_ = false;

	std::unordered_map<int, CourseProgress> progress_;  // course_id -> progress
	std::unordered_set<int> paused_courses_;
};
