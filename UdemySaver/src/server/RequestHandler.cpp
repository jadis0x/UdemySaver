#include "RequestHandler.h"

#define NOMINMAX
#include <Windows.h>

#include <nlohmann/json.hpp>
#include <boost/beast/http/status.hpp>

#include <curl/curl.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <stdexcept>
#include <thread>
#include <vector>
#include <filesystem>
#include <map>
#include <functional>
#include <cstdio>
#include <atomic>
#include <cerrno>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

// utils/helper
#include "FFmpegHelper.h"
#include "Helper.h"

using boost::beast::http::status;
using json = nlohmann::json;

namespace {
	constexpr const char* kDefaultUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36 Edg/134.0.0.0";
}

struct CurlHandle { CURL* h = nullptr; CurlHandle() { h = curl_easy_init(); } ~CurlHandle() { if (h) curl_easy_cleanup(h); } };

size_t RequestHandler::header_probe_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
	size_t total = size * nitems;
	HeaderProbe* hp = reinterpret_cast<HeaderProbe*>(userdata);
	std::string line(buffer, buffer + total);

	std::string low = line; std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char) std::tolower(c); });

	const std::string cl = "content-length:";
	if (low.rfind(cl, 0) == 0)
	{
		std::string num = line.substr((int) cl.size());
		try { hp->content_length = std::stoll(Helper::trim(num)); }
		catch (...) { }
	}

	// content-range: bytes 0-0/12345
	const std::string cr = "content-range:";
	if (low.rfind(cr, 0) == 0)
	{
		auto slash = line.find('/');
		if (slash != std::string::npos)
		{
			std::string total_s = line.substr(slash + 1);
			try { hp->content_range_total = std::stoll(Helper::trim(total_s)); }
			catch (...) { }
		}
	}
	return total;
}


bool RequestHandler::probe_content_length(const std::string& url,
										  const std::vector<std::string>& extra_headers,
										  long long& out_bytes, std::string& err) {
	out_bytes = -1; err.clear();
	CurlHandle ch; if (!ch.h) { err = "curl init failed"; return false; }

	// header list
	struct curl_slist* hdr = nullptr;
	for (auto& h : extra_headers) hdr = curl_slist_append(hdr, h.c_str());

	HeaderProbe hp;
	curl_easy_setopt(ch.h, CURLOPT_URL, url.c_str());
	curl_easy_setopt(ch.h, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(ch.h, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(ch.h, CURLOPT_USERAGENT, kDefaultUserAgent);
	curl_easy_setopt(ch.h, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(ch.h, CURLOPT_HTTPHEADER, hdr);
	curl_easy_setopt(ch.h, CURLOPT_NOBODY, 1L);                       // HEAD
	curl_easy_setopt(ch.h, CURLOPT_HEADERFUNCTION, header_probe_cb);
	curl_easy_setopt(ch.h, CURLOPT_HEADERDATA, &hp);
	curl_easy_setopt(ch.h, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
	curl_easy_setopt(ch.h, CURLOPT_TIMEOUT_MS, 20000L);
	curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYHOST, 2L);
	if (!proxy_.empty()) curl_easy_setopt(ch.h, CURLOPT_PROXY, proxy_.c_str());

	CURLcode rc = curl_easy_perform(ch.h);

	if (rc == CURLE_OK && hp.content_length >= 0)
	{
		out_bytes = hp.content_length;
		if (hdr) curl_slist_free_all(hdr);
		return true;
	}

	hp = HeaderProbe {};
	curl_easy_setopt(ch.h, CURLOPT_NOBODY, 0L);
	curl_easy_setopt(ch.h, CURLOPT_WRITEFUNCTION, Helper::write_discard);
	curl_easy_setopt(ch.h, CURLOPT_RANGE, "0-0");
	rc = curl_easy_perform(ch.h);

	if (hdr) curl_slist_free_all(hdr);

	if (rc == CURLE_OK && hp.content_range_total >= 0)
	{
		out_bytes = hp.content_range_total;
		return true;
	}

	err = curl_easy_strerror(rc);
	return false;
}

std::string RequestHandler::pick_from_asset_for_size(const json& a, const std::string& pref) {
	if (!a.is_object()) return {};

	std::map<int, std::string> mp4_by_quality;
	std::string fallback_mp4;
	std::string hls_candidate;

	auto register_mp4 = [&](const std::string& file, int quality)
	{
		if (file.empty()) return;
		if (fallback_mp4.empty()) fallback_mp4 = file;
		if (quality > 0) mp4_by_quality[quality] = file;
	};

	auto consider_hls = [&](const std::string& src)
	{
		if (!src.empty() && hls_candidate.empty()) hls_candidate = src;
	};

	if (a.contains("download_urls") && a["download_urls"].is_object())
	{
		auto it = a["download_urls"].find("Video");
		if (it != a["download_urls"].end() && it->is_array() && !it->empty())
		{
			for (auto& v : *it)
			{
				if (!v.contains("file") || !v["file"].is_string()) continue;
				std::string file = v["file"].get<std::string>();
				int quality = Helper::extract_quality_value(v.value("label", ""));
				register_mp4(file, quality);
			}
		}
	}

	if (a.contains("stream_urls") && a["stream_urls"].is_object())
	{
		auto it = a["stream_urls"].find("Video");
		if (it != a["stream_urls"].end() && it->is_array())
		{
			for (auto& v : *it)
			{
				std::string type = v.value("type", "");
				std::string file = v.value("file", "");
				if (file.empty()) continue;

				if (type == "video/mp4")
				{
					int quality = Helper::extract_quality_value(v.value("label", ""));
					register_mp4(file, quality);
				}
				else if (type == "application/x-mpegURL")
				{
					consider_hls(file);
				}
			}
		}
	}

	if (a.contains("hls_url") && a["hls_url"].is_string())
	{
		consider_hls(a["hls_url"].get<std::string>());
	}

	if (a.contains("media_sources") && a["media_sources"].is_array())
	{
		for (auto& m : a["media_sources"])
		{
			if (m.contains("src") && m["src"].is_string())
			{
				consider_hls(m["src"].get<std::string>());
				if (!hls_candidate.empty()) break;
			}
		}
	}

	auto pick_preferred_mp4 = [&]() -> std::string
	{
		if (mp4_by_quality.empty())
		{
			return fallback_mp4;
		}

		if (pref == "Lowest") return mp4_by_quality.begin()->second;
		if (pref == "Highest" || pref == "Auto") return mp4_by_quality.rbegin()->second;

		int wanted = Helper::extract_quality_value(pref);
		if (wanted > 0)
		{
			auto it = mp4_by_quality.find(wanted);
			if (it != mp4_by_quality.end()) return it->second;
			auto it_up = mp4_by_quality.lower_bound(wanted);
			if (it_up != mp4_by_quality.end()) return it_up->second;
			return mp4_by_quality.rbegin()->second;
		}

		return mp4_by_quality.rbegin()->second;
	};

	int highest_mp4_quality = mp4_by_quality.empty() ? 0 : mp4_by_quality.rbegin()->first;
	bool prefer_hls = false;
	if (!hls_candidate.empty())
	{
		if (pref == "Highest")
		{
			prefer_hls = true;
		}
		else
		{
			int wanted = Helper::extract_quality_value(pref);
			if (wanted >= 1080)
			{
				prefer_hls = true;
			}
			else if (wanted > 0 && highest_mp4_quality > 0 && wanted > highest_mp4_quality)
			{
				prefer_hls = true;
			}
		}
	}

	if (prefer_hls) return hls_candidate;

	std::string picked_mp4 = pick_preferred_mp4();
	if (!picked_mp4.empty()) return picked_mp4;

	return hls_candidate;
}

// ---------------- ctor / dtor ----------------
RequestHandler::RequestHandler(std::string webroot)
	: webroot_(std::move(webroot)), api_base_("https://www.udemy.com") {
	curl_global_init(CURL_GLOBAL_DEFAULT);
	avformat_network_init();
	load_settings();

	worker_ = std::thread([this] { worker_loop(); });
}

RequestHandler::~RequestHandler() {
	{
		std::lock_guard<std::mutex> lk(mtx_);
		stop_ = true;
	}
	cv_.notify_all();
	if (worker_.joinable()) worker_.join();

	curl_global_cleanup();
	avformat_network_deinit();
}

// ---------------- settings.ini ----------------
void RequestHandler::load_settings() {
	std::unordered_map<std::string, std::string> kv;

	std::string ini = Helper::read_file_utf8("settings.ini");
	if (ini.empty())
	{
		std::ofstream f("settings.ini");
		if (f)
		{
			f << "# UdemySaver settings\n";
			f << "udemy_access_token=\n";
			f << "udemy_api_base=https://www.udemy.com\n";
			f << "# http_proxy=\n";
			f << "download_subtitles=true\n";
			f << "download_assets=true\n";
		}

		token_.clear();
		api_base_ = "https://www.udemy.com";
		proxy_.clear();
		download_subtitles_ = true;
		download_assets_ = true;
		return;
	}

	std::istringstream is(ini);
	std::string line;
	while (std::getline(is, line))
	{
		line = Helper::trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';') continue;
		auto pos = line.find('=');
		if (pos == std::string::npos) continue;
		auto key = Helper::trim(line.substr(0, pos));
		auto val = Helper::trim(line.substr(pos + 1));
		if (!val.empty() && (val.front() == '"' || val.front() == '\''))
		{
			if (val.size() >= 2 && val.back() == val.front())
				val = val.substr(1, val.size() - 2);
		}
		std::transform(key.begin(), key.end(), key.begin(), ::tolower);
		kv[key] = val;
	}

	if (kv.count("udemy_access_token")) token_ = kv["udemy_access_token"];
	else if (kv.count("access_token"))  token_ = kv["access_token"];

	if (kv.count("udemy_api_base")) api_base_ = kv["udemy_api_base"];
	else if (kv.count("api_base"))   api_base_ = kv["api_base"];

	api_host_ = Helper::extract_host(api_base_);

	if (kv.count("http_proxy")) proxy_ = kv["http_proxy"];
	else if (kv.count("proxy")) proxy_ = kv["proxy"];

	if (kv.count("download_subtitles"))
	{
		std::string v = kv["download_subtitles"];
		std::transform(v.begin(), v.end(), v.begin(), ::tolower);
		download_subtitles_ = (v == "1" || v == "true" || v == "yes" || v == "on");
	}

	if (kv.count("download_assets"))
	{
		std::string v = kv["download_assets"];
		std::transform(v.begin(), v.end(), v.begin(), ::tolower);
		download_assets_ = (v == "1" || v == "true" || v == "yes" || v == "on");
	}
}

// ---------------- Udemy GET ----------------
std::string RequestHandler::udemy_get(const std::string& url, long timeout_ms) {
	if (token_.empty()) throw std::runtime_error("no_token");

	CURL* curl = curl_easy_init();
	if (!curl) throw std::runtime_error("curl_init_failed");

	struct CurlHeaders { curl_slist* list = nullptr; ~CurlHeaders() { if (list) curl_slist_free_all(list); } } hdr;

	std::string body;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, kDefaultUserAgent);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // enable gzip/deflate
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Helper::write_to_string);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

	if (!proxy_.empty())
	{
		curl_easy_setopt(curl, CURLOPT_PROXY, proxy_.c_str());
	}

	// headers
	std::string auth = "Authorization: Bearer " + token_;
	hdr.list = curl_slist_append(hdr.list, auth.c_str());
	hdr.list = curl_slist_append(hdr.list, "Accept: application/json, text/plain, */*");
	hdr.list = curl_slist_append(hdr.list, "Referer: https://www.udemy.com/");
	hdr.list = curl_slist_append(hdr.list, "Origin: https://www.udemy.com");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr.list);

	CURLcode rc = curl_easy_perform(curl);
	if (rc != CURLE_OK)
	{
		std::string err = curl_easy_strerror(rc);
		curl_easy_cleanup(curl);
		throw std::runtime_error("curl: " + err);
	}
	long code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(curl);

	if (code < 200 || code >= 300)
	{
		throw std::runtime_error("http " + std::to_string(code));
	}
	return body;
}

// ---------------- /session ----------------
std::pair<status, std::string> RequestHandler::handleSession() {
	json out;
	out["ok"] = true;
	out["source"] = "settings";
	out["auth"] = !token_.empty();
	out["opts"] = { {"subs", download_subtitles_}, {"assets", download_assets_} };

	if (token_.empty())
	{
		// not authenticated, but not an error
		return { status::ok, out.dump() };
	}

	try
	{
		// GET /api-2.0/users/me/?fields[user]=@default
		std::string url = api_base_ + "/api-2.0/users/me/?fields[user]=@default";
		auto body = udemy_get(url, 15000);
		json me = json::parse(body);
		out["user"] = me;
		return { status::ok, out.dump() };
	}
	catch (const std::exception& e)
	{
		out["ok"] = false;
		out["error"] = e.what();
		out["auth"] = false;
		return { status::bad_request, out.dump() };
	}
}

std::pair<boost::beast::http::status, std::string> RequestHandler::handleSettingsUpdate(const std::string& body) {
	using status = boost::beast::http::status;
	nlohmann::json out; out["ok"] = false;

	try
	{
		auto in = nlohmann::json::parse(body);

		std::string new_token = token_;
		std::string new_api = api_base_;
		std::string new_proxy = proxy_;
		bool new_subs = download_subtitles_;
		bool new_assets = download_assets_;

		if (in.contains("udemy_access_token")) new_token = in.value("udemy_access_token", std::string {});
		if (in.contains("udemy_api_base"))    new_api = in.value("udemy_api_base", std::string {});
		if (in.contains("http_proxy"))        new_proxy = in.value("http_proxy", std::string {});
		if (in.contains("download_subtitles")) new_subs = in.value("download_subtitles", false);
		if (in.contains("download_assets"))    new_assets = in.value("download_assets", false);

		auto trim2 = [](std::string s)
		{
			auto ws = [](int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
			while (!s.empty() && ws((unsigned char) s.front())) s.erase(s.begin());
			while (!s.empty() && ws((unsigned char) s.back()))  s.pop_back();
			return s;
		};
		new_token = trim2(new_token);
		new_api = trim2(new_api);
		new_proxy = trim2(new_proxy);
		if (new_api.empty()) new_api = "https://www.udemy.com";

		{
			std::ofstream f("settings.ini", std::ios::binary);
			if (!f)
			{
				out["error"] = "cannot write settings.ini";
				return { status::internal_server_error, out.dump() };
			}
			f << "# UdemySaver settings\n";
			f << "udemy_access_token=" << new_token << "\n";
			f << "udemy_api_base=" << new_api << "\n";
			if (!new_proxy.empty()) f << "http_proxy=" << new_proxy << "\n";
			f << "download_subtitles=" << (new_subs ? "true" : "false") << "\n";
			f << "download_assets=" << (new_assets ? "true" : "false") << "\n";
			f.flush();
		}

		token_ = new_token;
		api_base_ = new_api;
		proxy_ = new_proxy;
		download_subtitles_ = new_subs;
		download_assets_ = new_assets;

		out["ok"] = true;
		out["auth"] = !token_.empty();
		out["opts"] = { {"subs", download_subtitles_}, {"assets", download_assets_} };
		return { status::ok, out.dump() };
	}
	catch (const std::exception& e)
	{
		out["ok"] = false;
		out["error"] = e.what();
		out["auth"] = false;
		out["courses"] = json::array();
		return { status::bad_request, out.dump() };
	}
}

std::pair<boost::beast::http::status, std::string>
RequestHandler::handleCourses(int page, int page_size) {
	using boost::beast::http::status;
	json out;
	out["ok"] = true;

	if (page <= 0) page = 1;
	if (page_size <= 0 || page_size > 100) page_size = 12;

	if (token_.empty())
	{
		out["auth"] = false;
		out["page"] = page;
		out["page_size"] = page_size;
		out["courses"] = json::array();
		return { status::ok, out.dump() };
	}

	auto fix_proto = [](std::string s)->std::string
	{
		// "//img-..." -> "https://img-..."
		if (!s.empty() && s.rfind("//", 0) == 0) s = "https:" + s;
		return s;
	};

	auto make_abs = [&](std::string s)->std::string
	{
		if (!s.empty() && s[0] == '/') return api_base_ + s;
		return s;
	};

	try
	{
		std::ostringstream url;
		url << api_base_
			<< "/api-2.0/users/me/subscribed-courses/?page=" << page
			<< "&page_size=" << page_size
			<< "&fields[course]=@min,"
			"title,headline,url,"
			"image_480x270,image_480x270@2x,"
			"image_240x135,image_240x135@2x,"
			"image_125_H,image_200_H,"
			"visible_instructors";

		auto body = udemy_get(url.str(), 15000);
		json raw = json::parse(body);

		json courses = json::array();
		if (raw.contains("results") && raw["results"].is_array())
		{
			for (auto& c : raw["results"])
			{
				auto pick = [&](const char* key)->std::string
				{
					if (c.contains(key) && c[key].is_string())
						return c[key].get<std::string>();
					return {};
				};


				std::string img =
					pick("image_480x270"); if (img.empty()) img = pick("image_480x270@2x");
				if (img.empty()) img = pick("image_240x135"); if (img.empty()) img = pick("image_240x135@2x");
				if (img.empty()) img = pick("image_200_H"); if (img.empty()) img = pick("image_125_H");

				img = fix_proto(img);

				std::string rel = c.value("url", "");
				std::string abs = make_abs(rel);

				json j;
				j["id"] = c.value("id", 0);
				j["title"] = c.value("title", "");
				j["headline"] = c.value("headline", "");
				j["url"] = abs;
				j["image"] = img;
				j["image_raw"] = c.value("image_480x270", "");
				if (c.contains("visible_instructors") &&
					c["visible_instructors"].is_array() &&
					!c["visible_instructors"].empty())
				{
					j["instructor"] = c["visible_instructors"][0].value("title", "");
				}
				else
				{
					j["instructor"] = "";
				}
				courses.push_back(j);
			}
		}

		out["auth"] = true;
		out["page"] = page;
		out["page_size"] = page_size;
		out["total"] = raw.value("count", 0);
		out["courses"] = courses;

		return { status::ok, out.dump() };
	}
	catch (const std::exception& e)
	{
		out["ok"] = false;
		out["error"] = e.what();
		return { status::bad_request, out.dump() };
	}
}


// ---------------- download (generic raw) ----------------
std::pair<status, std::string> RequestHandler::handleDownloadRaw(const std::string& body) {
	json out;
	try
	{
		json in = json::parse(body);
		std::string url;
		int course_id = in.value("course_id", 0);
		int lecture_id = in.value("lecture_id", 0);
		std::string quality = in.value("quality", "Auto");
		if (course_id && lecture_id)
		{
			url = resolve_lecture_stream(course_id, lecture_id, quality);
		}
		else if (in.contains("url") && in["url"].is_string())
		{
			url = in["url"].get<std::string>();
		}
		else
		{
			throw std::runtime_error("missing url or course_id/lecture_id");
		}

		json q;
		q["url"] = url;
		q["filename"] = in.value("filename", "output.mp4");
		q["course_id"] = course_id;
		q["course_title"] = in.value("course_title", std::string {});
		auto [st, resp] = handleQueueAdd(q.dump());
		return { st, resp };
	}
	catch (const std::exception& e)
	{
		out["ok"] = false; out["error"] = e.what();
		return { status::bad_request, out.dump() };
	}
}


std::pair<boost::beast::http::status, std::string>
RequestHandler::handleLectures(int course_id, int page, int page_size) {
	using boost::beast::http::status;
	if (page <= 0) page = 1;
	if (page_size <= 0 || page_size > 200) page_size = 100;

	nlohmann::json out;
	try
	{
		if (token_.empty())
		{
			out["count"] = 0;
			out["next"] = nullptr;
			out["previous"] = nullptr;
			out["results"] = nlohmann::json::array();
			return { status::ok, out.dump() };
		}

		// subscriber-curriculum-items => chapter + lecture + asset
		std::ostringstream url;
		url << api_base_
			<< "/api-2.0/courses/" << course_id
			<< "/subscriber-curriculum-items/?page=" << page
			<< "&page_size=" << page_size
			<< "&fields[lecture]=asset,title,object_index,asset_type,supplementary_assets,description,download_url,is_free,last_watched_second"
			<< "&fields[asset]=stream_urls,download_urls,download_url,captions,title,filename,data,body,hls_url,media_sources,asset_type,length,media_license_token,course_is_drmed,thumbnail_sprite,slides,slide_urls,external_url"
			<< "&fields[chapter]=title,object_index"
			<< "&fields[supplementary_asset]=id,title,asset_type,download_urls,external_url,filename";

		auto body = udemy_get(url.str(), 20000);
		nlohmann::json raw = nlohmann::json::parse(body);

		out["count"] = raw.value("count", 0);
		out["next"] = raw.contains("next") ? raw["next"] : nullptr;
		out["previous"] = raw.contains("previous") ? raw["previous"] : nullptr;

		out["results"] = nlohmann::json::array();
		int cur_section = 0;
		std::string cur_section_title;

		if (raw.contains("results") && raw["results"].is_array())
		{
			for (auto& it : raw["results"])
			{
				const std::string klass = it.value("_class", it.value("type", ""));
				if (klass == "chapter")
				{
					cur_section += 1;
					cur_section_title = it.value("title", "");
					continue;
				}
				if (klass == "lecture")
				{
					nlohmann::json lec;
					lec["id"] = it.value("id", 0);
					lec["title"] = it.value("title", "");
					if (it.contains("asset")) lec["asset"] = it["asset"];
					if (it.contains("supplementary_assets"))
						lec["supplementary_assets"] = it["supplementary_assets"];
					lec["section_index"] = cur_section;
					lec["section_title"] = cur_section_title;
					lec["object_index"] = it.value("object_index", 0);
					out["results"].push_back(std::move(lec));
				}
			}
		}
		return { status::ok, out.dump() };
	}
	catch (const std::exception& e)
	{
		out["ok"] = false;
		out["error"] = e.what();
		return { status::bad_request, out.dump() };
	}
}

std::pair<boost::beast::http::status, std::string>
RequestHandler::handleQueueAdd(const std::string& body) {
	using status = boost::beast::http::status;
	json out;

	try
	{
		json in = json::parse(body);

		int lecture_id = in.value("lecture_id", 0);
		int asset_id = in.value("asset_id", 0);

		std::string in_url = in.value("url", std::string {});
		if (in_url.empty())
		{
			if (asset_id && in.value("course_id", 0) && lecture_id)
			{
				in_url = resolve_supplementary_asset(in["course_id"].get<int>(), lecture_id, asset_id);
			}
			else
			{
				throw std::runtime_error("missing url");
			}
		}

		// ---- job ----
		Job j;
		j.id = next_id_++;
		j.url = in_url;
		j.filename = in.value("filename", "");

		j.course_id = in.value("course_id", 0);
		j.course_title = in.value("course_title", std::string {});

		j.section_index = in.value("section_index", 0);
		j.section_title = in.value("section_title", std::string {});
		j.lecture_index = in.value("lecture_index", 0);
		j.lecture_title = in.value("lecture_title", std::string {});

		if (j.course_id && !j.course_title.empty())
		{
			j.out_path_dir = Helper::course_dir(j.course_id, j.course_title);
			if (!j.section_title.empty() && j.section_index > 0)
			{
				j.out_path_dir += "/" + Helper::section_dir(j.section_index, j.section_title);
			}
		}
		else
		{
			j.out_path_dir = "downloads/misc";
		}

		if (j.filename.empty())
		{
			std::string base = j.lecture_title.empty() ? "video" : Helper::slugify(j.lecture_title);
			if (j.lecture_index > 0) base = Helper::zpad(j.lecture_index, 3) + " - " + base;
			j.filename = base + ".mp4";
		}

		{
			std::error_code fec;
			auto out_dir = std::filesystem::u8path(j.out_path_dir);
			std::filesystem::create_directories(out_dir, fec);
			auto final_path = out_dir / std::filesystem::u8path(j.filename);

			bool exists_final = std::filesystem::exists(final_path, fec);
			if (exists_final)
			{
				json out2;
				out2["ok"] = true;
				out2["queued"] = false;
				out2["skipped"] = true;
				out2["reason"] = "exists";
				out2["path"] = Helper::path_to_utf8(final_path);
				return { status::ok, out2.dump() };
			}
		}

		if (j.course_id)
		{
			auto& cp = progress_[j.course_id];
			if (cp.title.empty()) cp.title = j.course_title;
			cp.total += 1;
		}

		j.headers.push_back(std::string("User-Agent: ") + kDefaultUserAgent);
		j.headers.push_back("Referer: https://www.udemy.com/");
		j.headers.push_back("Origin: https://www.udemy.com");

		append_auth_headers_for_url(j.url, j.headers);

		if (in.contains("headers") && in["headers"].is_array())
			for (auto& h : in["headers"]) if (h.is_string()) j.headers.push_back(h.get<std::string>());

		{
			std::lock_guard<std::mutex> lk(mtx_);

			auto dup = std::find_if(
				queue_.begin(), queue_.end(),
				[&](const Job& q)
			{
				return (q.url == j.url ||
						(q.out_path_dir == j.out_path_dir &&
						 q.filename == j.filename)) &&
					q.state != Job::State::Done &&
					q.state != Job::State::Failed;
			});

			if (dup != queue_.end())
			{
				json out2;
				out2["ok"] = true;
				out2["queued"] = false;
				out2["skipped"] = true;
				out2["reason"] = "queued";
				out2["id"] = dup->id;
				return { status::ok, out2.dump() };
			}

			if (paused_courses_.find(j.course_id) != paused_courses_.end())
				j.state = Job::State::Paused;
			queue_.push_back(std::move(j));
		}

		cv_.notify_one();

		out["ok"] = true;
		out["queued"] = true;
		out["id"] = j.id;
		return { status::ok, out.dump() };
	}
	catch (const std::exception& e)
	{
		out["ok"] = false;
		out["error"] = e.what();
		return { status::bad_request, out.dump() };
	}
}


std::pair<boost::beast::http::status, std::string> RequestHandler::handleQueueList() {
	json out;
	out["ok"] = true;
	out["running"] = running_;
	out["items"] = json::array();

	auto jstate = [](RequestHandler::Job::State s) -> const char*
	{
		using S = RequestHandler::Job::State;
		switch (s)
		{
		case S::Queued:       return "queued";
		case S::Downloading:  return "downloading";
		case S::Done:         return "done";
		case S::Failed:       return "failed";
		case S::Paused:       return "paused";
		}
		return "unknown";
	};

	std::lock_guard<std::mutex> lk(mtx_);
	out["items"] = json::array();
	for (auto const& j : queue_)
	{
		double eta = -1.0;
		if (j.state == Job::State::Downloading &&
			j.speed_bps > 1.0 &&
			j.bytes_total > 0 &&
			j.bytes_now >= 0 &&
			j.bytes_total >= j.bytes_now)
		{
			const double remain = static_cast<double>(j.bytes_total - j.bytes_now);
			eta = remain / j.speed_bps;
		}

		json it;
		it["id"] = j.id;
		it["url"] = j.url;
		it["filename"] = j.filename;
		it["state"] = jstate(j.state);
		it["progress"] = j.progress;
		it["message"] = j.message;
		it["out_path"] = j.out_path;

		it["course_id"] = j.course_id;
		it["course_title"] = j.course_title;
		it["section_index"] = j.section_index;
		it["section_title"] = j.section_title;
		it["lecture_index"] = j.lecture_index;
		it["lecture_title"] = j.lecture_title;

		it["out_dir"] = j.out_path_dir;

		it["bytes_now"] = j.bytes_now;
		it["bytes_total"] = j.bytes_total;
		it["speed_bps"] = j.speed_bps;

		it["eta_sec"] = eta;

		out["items"].push_back(std::move(it));
	}

	json courses = json::array();
	for (auto const& [cid, cp] : progress_)
	{
		json c;
		c["course_id"] = cid;
		c["title"] = cp.title;
		c["done"] = cp.done;
		c["total"] = cp.total;
		courses.push_back(std::move(c));
	}
	out["courses"] = courses;

	return { boost::beast::http::status::ok, out.dump() };
}

std::pair<boost::beast::http::status, std::string> RequestHandler::handleQueuePause(const std::string& body) {
	using status = boost::beast::http::status;
	json out;
	try
	{
		json in = json::parse(body);
		int course_id = in.value("course_id", 0);
		if (!course_id) throw std::runtime_error("missing course_id");

		{
			std::lock_guard<std::mutex> lk(mtx_);
			paused_courses_.insert(course_id);
			for (auto& q : queue_)
			{
				if (q.course_id == course_id &&
					q.state == Job::State::Queued)
				{
					q.state = Job::State::Paused;
				}
			}
		}

		out["ok"] = true;
		return { status::ok, out.dump() };
	}
	catch (const std::exception& e)
	{
		out["ok"] = false;
		out["error"] = e.what();
		return { status::bad_request, out.dump() };
	}
}

std::pair<boost::beast::http::status, std::string> RequestHandler::handleQueueResume(const std::string& body) {
	using status = boost::beast::http::status;
	json out;
	try
	{
		json in = json::parse(body);
		int course_id = in.value("course_id", 0);
		if (!course_id) throw std::runtime_error("missing course_id");

		{
			std::lock_guard<std::mutex> lk(mtx_);
			paused_courses_.erase(course_id);
			for (auto& q : queue_)
			{
				if (q.course_id == course_id &&
					q.state == Job::State::Paused)
				{
					q.state = Job::State::Queued;
				}
			}
		}
		cv_.notify_one();

		out["ok"] = true;
		return { status::ok, out.dump() };
	}
	catch (const std::exception& e)
	{
		out["ok"] = false;
		out["error"] = e.what();
		return { status::bad_request, out.dump() };
	}
}

std::pair<boost::beast::http::status, std::string> RequestHandler::handleReconcile(const std::string& target) {
	using status = boost::beast::http::status;
	auto get_param = [&](const char* key)->std::string
	{
		auto qpos = target.find('?'); if (qpos == std::string::npos) return {};
		std::string qs = target.substr(qpos + 1);
		std::istringstream ss(qs); std::string kv;
		while (std::getline(ss, kv, '&'))
		{
			auto eq = kv.find('='); if (eq == std::string::npos) continue;
			auto k = kv.substr(0, eq), v = kv.substr(eq + 1);
			if (k == key) return v;
		}
		return {};
	};

	int course_id = 0;
	try { course_id = std::stoi(get_param("course_id")); }
	catch (...) { }

	json out; out["ok"] = true;
	out["present"] = json::array();


	std::string dir = "";
	{
		auto it = progress_.find(course_id);
		if (it != progress_.end() && !it->second.title.empty())
		{
			dir = Helper::course_dir(course_id, it->second.title);
		}
	}
	if (dir.empty())
	{
		out["note"] = "course_dir not resolved";
		return { status::ok, out.dump() };
	}

	std::error_code ec;
	auto dir_path = std::filesystem::u8path(dir);
	if (std::filesystem::exists(dir_path, ec))
	{
		for (auto& p : std::filesystem::recursive_directory_iterator(dir_path, ec))
		{
			if (p.is_regular_file())
			{
				auto name = Helper::path_to_utf8(p.path().filename());
				int idx = 0;
				if (name.size() >= 3 &&
					std::isdigit(static_cast<unsigned char>(name[0])) &&
					std::isdigit(static_cast<unsigned char>(name[1])) &&
					std::isdigit(static_cast<unsigned char>(name[2])))
				{
					idx = (name[0] - '0') * 100 + (name[1] - '0') * 10 + (name[2] - '0');
				}
				json f; f["file"] = name; f["lecture_index"] = idx;
				out["present"].push_back(std::move(f));
			}
		}
	}
	return { status::ok, out.dump() };
}

std::pair<boost::beast::http::status, std::string> RequestHandler::handleEstimate(const std::string& target) {
	using status = boost::beast::http::status;

	auto get_param = [&](const char* key)->std::string
	{
		auto qpos = target.find('?'); if (qpos == std::string::npos) return {};
		std::string qs = target.substr(qpos + 1);
		std::istringstream ss(qs); std::string kv;
		while (std::getline(ss, kv, '&'))
		{
			auto eq = kv.find('='); if (eq == std::string::npos) continue;
			auto k = kv.substr(0, eq), v = kv.substr(eq + 1);
			if (k == key) return v;
		}
		return {};
	};

	int course_id = 0;
	try { course_id = std::stoi(get_param("course_id")); }
	catch (...) { }
	std::string quality = get_param("quality"); if (quality.empty()) quality = "Highest";

	json out; out["ok"] = true;
	if (!course_id) { out["ok"] = false; out["error"] = "missing course_id"; return { status::bad_request, out.dump() }; }
	if (token_.empty()) { out["ok"] = false; out["error"] = "not authenticated"; return { status::unauthorized, out.dump() }; }

	long long total_bytes = 0;
	int sized = 0, unknown = 0, videos = 0;

	try
	{
		int page = 1;
		while (true)
		{
			std::ostringstream url;
			url << api_base_
				<< "/api-2.0/courses/" << course_id
				<< "/subscriber-curriculum-items/?page=" << page
				<< "&page_size=200"
				<< "&fields[lecture]=asset,title,object_index,asset_type,description,download_url,is_free,last_watched_second"
				<< "&fields[asset]=stream_urls,download_urls,download_url,filename,title,hls_url,media_sources,asset_type,length,media_license_token,course_is_drmed,thumbnail_sprite,slides,slide_urls,external_url"
				<< "&fields[chapter]=title,object_index";

			auto body = udemy_get(url.str(), 20000);
			json raw = json::parse(body);

			if (!(raw.contains("results") && raw["results"].is_array())) break;

			for (auto& it : raw["results"])
			{
				const std::string klass = it.value("_class", it.value("type", ""));
				if (klass != "lecture") continue;

				auto asset = it.contains("asset") ? it["asset"] : json {};
				std::string urlv = pick_from_asset_for_size(asset, quality);
				if (urlv.empty()) continue;
				++videos;

				long long bytes = -1; std::string emsg;
				std::vector<std::string> hdrs = {
										std::string("User-Agent: ") + kDefaultUserAgent,
										"Referer: https://www.udemy.com/",
										"Origin: https://www.udemy.com"
				};
				append_auth_headers_for_url(urlv, hdrs);

				if (probe_content_length(urlv, hdrs, bytes, emsg) && bytes >= 0)
				{
					total_bytes += bytes; ++sized;
				}
				else
				{
					++unknown;
				}
			}

			// next?
			if (raw.contains("next") && !raw["next"].is_null()) { ++page; }
			else break;
		}

		out["total_bytes"] = total_bytes;
		out["videos"] = videos;
		out["sized"] = sized;
		out["unknown"] = unknown;
		out["quality"] = quality;

		return { status::ok, out.dump() };
	}
	catch (const std::exception& e)
	{
		out["ok"] = false; out["error"] = e.what();
		return { status::bad_request, out.dump() };
	}
}


static size_t file_write(void* ptr, size_t size, size_t nmemb, void* userdata) {
	FILE* fp = (FILE*) userdata;
	return fwrite(ptr, size, nmemb, fp);
}

static int curl_xferinfo_trampoline(void* clientp,
									curl_off_t dltotal, curl_off_t dlnow,
									curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
	using CB = std::function<void(double, double)>;
	auto* cb = reinterpret_cast<CB*>(clientp);
	if (cb && *cb) (*cb)(static_cast<double>(dlnow), static_cast<double>(dltotal));
	return 0; // continue
}


bool RequestHandler::curl_download_file(const std::string& url, const std::string& out_path, const std::vector<std::string>& extra_headers, std::function<void(double, double)> on_progress, std::string& msg) {
	msg.clear();
	auto out_fs = std::filesystem::u8path(out_path);
	auto tmp_path = out_fs;
	tmp_path += ".part";

	auto tmp_utf8_u8 = tmp_path.u8string();
	std::string tmp_utf8(tmp_utf8_u8.begin(), tmp_utf8_u8.end());

	long long already = 0;
	{
		std::error_code ec;
		auto sz = std::filesystem::file_size(tmp_path, ec);
		if (!ec) already = static_cast<long long>(sz);
	}

	FILE* fp = Helper::xfopen(tmp_utf8.c_str(), already ? "ab" : "wb");
	if (!fp) { msg = "cannot open file"; return false; }

	CurlHandle ch;
	if (!ch.h) { fclose(fp); msg = "curl init failed"; return false; }

	// header list
	struct curl_slist* hdr = nullptr;
	for (auto& h : extra_headers) hdr = curl_slist_append(hdr, h.c_str());

	curl_easy_setopt(ch.h, CURLOPT_URL, url.c_str());
	curl_easy_setopt(ch.h, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(ch.h, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(ch.h, CURLOPT_USERAGENT, kDefaultUserAgent);
	curl_easy_setopt(ch.h, CURLOPT_ACCEPT_ENCODING, ""); // gzip
	curl_easy_setopt(ch.h, CURLOPT_HTTPHEADER, hdr);
	curl_easy_setopt(ch.h, CURLOPT_WRITEDATA, fp);
	curl_easy_setopt(ch.h, CURLOPT_WRITEFUNCTION, file_write);

	curl_easy_setopt(ch.h, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(ch.h, CURLOPT_XFERINFOFUNCTION, curl_xferinfo_trampoline);
	curl_easy_setopt(ch.h, CURLOPT_XFERINFODATA, &on_progress);

	curl_easy_setopt(ch.h, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
	curl_easy_setopt(ch.h, CURLOPT_TIMEOUT, 0L); // sınırsız
	curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYHOST, 2L);

	if (!proxy_.empty())
		curl_easy_setopt(ch.h, CURLOPT_PROXY, proxy_.c_str());

	// resume
	if (already > 0)
		curl_easy_setopt(ch.h, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(already));

	CURLcode rc = curl_easy_perform(ch.h);
	if (hdr) curl_slist_free_all(hdr);
	fclose(fp);

	if (rc != CURLE_OK)
	{
		msg = curl_easy_strerror(rc);
		return false;
	}

	std::error_code ec;
	std::filesystem::rename(tmp_path, out_fs, ec);
	if (ec) { msg = "rename failed"; return false; }

	return true;
}

void RequestHandler::append_auth_headers_for_url(const std::string& url, std::vector<std::string>& headers) const
{
	if (token_.empty()) return;

	headers.push_back(std::string("Authorization: Bearer ") + token_);
}

std::string RequestHandler::resolve_lecture_stream(int course_id, int lecture_id, const std::string& prefer_quality) {
	std::ostringstream url;
	url << api_base_
		<< "/api-2.0/users/me/subscribed-courses/" << course_id
		<< "/lectures/" << lecture_id
		<< "?fields[asset]=stream_urls,download_urls,download_url,captions,title,filename,data,body,hls_url,media_sources,asset_type,length,media_license_token,course_is_drmed,thumbnail_sprite,slides,slide_urls,external_url"
		<< "&fields[lecture]=asset,supplementary_assets,description,download_url,is_free,last_watched_second";

	auto body = udemy_get(url.str(), 15000);
	json j = json::parse(body);

	if (!j.contains("asset") || !j["asset"].is_object())
		throw std::runtime_error("lecture has no asset");

	auto& a = j["asset"];

	auto is_exact_1080 = [](const std::string& s) -> bool
	{
		if (s.empty()) return false;
		for (char c : s)
		{
			if (!std::isdigit(static_cast<unsigned char>(c))) return false;
		}
		return Helper::extract_quality_value(s) == 1080;
	};

	if (is_exact_1080(prefer_quality))
	{
		auto make_absolute = [](const std::string& base_url, const std::string& rel) -> std::string
		{
			if (rel.empty()) return base_url;
			if (rel.find("://") != std::string::npos) return rel;

			std::string base_no_query = base_url;
			std::string base_query;
			auto qpos = base_no_query.find('?');
			if (qpos != std::string::npos)
			{
				base_query = base_url.substr(qpos);
				base_no_query = base_no_query.substr(0, qpos);
			}

			auto append_query = [&](std::string result) -> std::string
			{
				if (!base_query.empty() && result.find('?') == std::string::npos)
					result += base_query;
				return result;
			};

			if (!rel.empty() && rel[0] == '/')
			{
				auto scheme_pos = base_no_query.find("://");
				if (scheme_pos == std::string::npos) return append_query(rel);
				auto host_end = base_no_query.find('/', scheme_pos + 3);
				std::string origin = (host_end == std::string::npos)
					? base_no_query
					: base_no_query.substr(0, host_end);
				return append_query(origin + rel);
			}

			std::string base_dir = base_no_query;
			auto slash = base_dir.rfind('/');
			if (slash == std::string::npos)
				base_dir += '/';
			else
				base_dir = base_dir.substr(0, slash + 1);

			return append_query(base_dir + rel);
		};

		auto fetch_with_headers = [&](const std::string& src) -> std::string
		{
			CurlHandle ch; if (!ch.h) throw std::runtime_error("curl init failed");

			struct curl_slist* hdr = nullptr;
			std::vector<std::string> headers = {
										std::string("User-Agent: ") + kDefaultUserAgent,
										"Referer: https://www.udemy.com/",
										"Origin: https://www.udemy.com"
			};
			append_auth_headers_for_url(src, headers);

			for (auto& h : headers) hdr = curl_slist_append(hdr, h.c_str());

			std::string out;
			curl_easy_setopt(ch.h, CURLOPT_URL, src.c_str());
			curl_easy_setopt(ch.h, CURLOPT_FOLLOWLOCATION, 1L);
			curl_easy_setopt(ch.h, CURLOPT_MAXREDIRS, 8L);
			curl_easy_setopt(ch.h, CURLOPT_USERAGENT, kDefaultUserAgent);
			curl_easy_setopt(ch.h, CURLOPT_ACCEPT_ENCODING, "");
			curl_easy_setopt(ch.h, CURLOPT_HTTPHEADER, hdr);
			curl_easy_setopt(ch.h, CURLOPT_WRITEFUNCTION, Helper::write_to_string);
			curl_easy_setopt(ch.h, CURLOPT_WRITEDATA, &out);
			curl_easy_setopt(ch.h, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
			curl_easy_setopt(ch.h, CURLOPT_TIMEOUT_MS, 20000L);
			curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYPEER, 1L);
			curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYHOST, 2L);
			if (!proxy_.empty()) curl_easy_setopt(ch.h, CURLOPT_PROXY, proxy_.c_str());

			CURLcode rc = curl_easy_perform(ch.h);
			if (hdr) curl_slist_free_all(hdr);

			if (rc != CURLE_OK)
			{
				throw std::runtime_error(std::string("curl: ") + curl_easy_strerror(rc));
			}

			long code = 0;
			curl_easy_getinfo(ch.h, CURLINFO_RESPONSE_CODE, &code);
			if (code < 200 || code >= 300)
			{
				throw std::runtime_error("http " + std::to_string(code));
			}

			return out;
		};

		auto pick_variant_from_master = [&](const std::string& master_url, const std::string& playlist) -> std::string
		{
			if (playlist.find("#EXT-X-KEY") != std::string::npos)
				throw std::runtime_error("Encrypted stream. Exiting.");

			struct Variant {
				std::string uri;
				int width = 0;
				int height = 0;
				long long bandwidth = 0;
			};

			std::vector<Variant> variants;
			std::istringstream ss(playlist);
			std::string line;
			while (std::getline(ss, line))
			{
				if (!line.empty() && line.back() == '\r') line.pop_back();
				std::string trimmed = Helper::trim(line);
				if (trimmed.rfind("#EXT-X-STREAM-INF", 0) != 0) continue;

				Variant v;
				auto colon = trimmed.find(':');
				std::string attrs = (colon == std::string::npos) ? std::string {} : trimmed.substr(colon + 1);

				auto find_attr = [&](const std::string& key) -> std::string
				{
					auto pos = attrs.find(key);
					if (pos == std::string::npos) return {};
					pos += key.size();
					auto end = attrs.find(',', pos);
					std::string val = (end == std::string::npos) ? attrs.substr(pos) : attrs.substr(pos, end - pos);
					return Helper::trim(val);
				};

				std::string res = find_attr("RESOLUTION=");
				auto x = res.find('x');
				if (x != std::string::npos)
				{
					try
					{
						v.width = std::stoi(res.substr(0, x));
						v.height = std::stoi(res.substr(x + 1));
					}
					catch (...)
					{
						v.width = v.height = 0;
					}
				}

				std::string bw = find_attr("BANDWIDTH=");
				if (!bw.empty())
				{
					try { v.bandwidth = std::stoll(bw); }
					catch (...) { v.bandwidth = 0; }
				}

				std::string next_line;
				while (std::getline(ss, next_line))
				{
					if (!next_line.empty() && next_line.back() == '\r') next_line.pop_back();
					std::string url_line = Helper::trim(next_line);
					if (url_line.empty()) continue;
					if (url_line.rfind("#", 0) == 0) continue;
					v.uri = url_line;
					break;
				}

				if (!v.uri.empty()) variants.push_back(std::move(v));
			}

			if (variants.empty()) return {};

			const Variant* exact = nullptr;
			const Variant* best_height = nullptr;
			const Variant* best_bandwidth = nullptr;
			for (auto& v : variants)
			{
				if (v.height == 1080 && v.width == 1920)
				{
					exact = &v;
					break;
				}
				if (!best_height || v.height > best_height->height)
					best_height = &v;
				if (!best_bandwidth || v.bandwidth > best_bandwidth->bandwidth)
					best_bandwidth = &v;
			}

			const Variant* chosen = exact;
			if (!chosen)
			{
				if (best_height && best_height->height > 0) chosen = best_height;
				else chosen = best_bandwidth;
			}

			if (!chosen) return {};
			return make_absolute(master_url, chosen->uri);
		};

		auto first_media_source = [&]() -> std::string
		{
			if (a.contains("media_sources") && a["media_sources"].is_array())
			{
				for (auto& m : a["media_sources"])
				{
					if (m.contains("src") && m["src"].is_string())
					{
						std::string src = m["src"].get<std::string>();
						if (!src.empty()) return src;
					}
				}
			}
			return {};
		};

		std::string master_src = first_media_source();
		if (!master_src.empty())
		{
			try
			{
				std::cout << "[HLS] Fetching master playlist: " << master_src << std::endl;
				std::string playlist = fetch_with_headers(master_src);
				std::string variant = pick_variant_from_master(master_src, playlist);
				if (!variant.empty())
				{
					std::cout << "[HLS] Selected variant playlist: " << variant << std::endl;
				}
				if (!variant.empty()) return variant;
			}
			catch (const std::runtime_error& e)
			{
				if (std::string(e.what()) == "Encrypted stream. Exiting.") throw;
			}
			catch (...)
			{
			}
		}
	}

	if (a.contains("stream_urls") && a["stream_urls"].is_object())
	{
		auto& su = a["stream_urls"];
		if (su.contains("Video") && su["Video"].is_array() && !su["Video"].empty())
		{
			std::map<int, std::string> qmap;
			std::string autoSrc;
			std::string hlsSrc;

			for (auto& v : su["Video"])
			{
				if (!v.contains("file")) continue;
				std::string file = v["file"].get<std::string>();
				if (file.empty()) continue;

				std::string label = v.value("label", "");
				std::string type = v.value("type", "");

				bool is_hls = (type == "application/x-mpegURL") || label == "Auto";
				if (is_hls)
				{
					if (hlsSrc.empty()) hlsSrc = file;
					if (autoSrc.empty()) autoSrc = file;
					continue;
				}

				int q = Helper::extract_quality_value(label);
				if (q > 0)
				{
					qmap[q] = file;
				}
				else if (autoSrc.empty())
				{
					autoSrc = file;
				}
			}

			if (a.contains("hls_url") && a["hls_url"].is_string() && hlsSrc.empty())
			{
				hlsSrc = a["hls_url"].get<std::string>();
			}

			if (hlsSrc.empty() && a.contains("media_sources") && a["media_sources"].is_array())
			{
				for (auto& m : a["media_sources"])
				{
					if (m.contains("src") && m["src"].is_string())
					{
						hlsSrc = m["src"].get<std::string>();
						if (!hlsSrc.empty()) break;
					}
				}
			}

			int highest_mp4 = qmap.empty() ? 0 : qmap.rbegin()->first;
			bool prefer_hls = false;
			if (!hlsSrc.empty())
			{
				if (prefer_quality == "Highest")
				{
					prefer_hls = true;
				}
				else
				{
					int wanted = Helper::extract_quality_value(prefer_quality);
					if (wanted >= 1080)
					{
						prefer_hls = true;
					}
					else if (wanted > 0 && highest_mp4 > 0 && wanted > highest_mp4)
					{
						prefer_hls = true;
					}
				}
			}

			if (prefer_hls)
			{
				if (!hlsSrc.empty())
				{
					std::cout << "[HLS] Using HLS playlist: " << hlsSrc << std::endl;
				}
				return hlsSrc;
			}

			if (prefer_quality == "Auto")
			{
				if (!autoSrc.empty()) return autoSrc;
				if (!qmap.empty()) return qmap.rbegin()->second;
				if (!hlsSrc.empty()) return hlsSrc;
			}

			if (prefer_quality == "Lowest")
			{
				if (!qmap.empty()) return qmap.begin()->second;
				if (!autoSrc.empty()) return autoSrc;
				if (!hlsSrc.empty()) return hlsSrc;
			}

			if (!qmap.empty())
			{
				if (prefer_quality == "Highest")
					return qmap.rbegin()->second;

				int wanted = Helper::extract_quality_value(prefer_quality);
				if (wanted > 0)
				{
					auto it = qmap.find(wanted);
					if (it != qmap.end()) return it->second;
					auto it_up = qmap.lower_bound(wanted);
					if (it_up != qmap.end()) return it_up->second;
					return qmap.rbegin()->second;
				}

				return qmap.rbegin()->second;
			}

			if (!autoSrc.empty()) return autoSrc;
			if (!hlsSrc.empty()) return hlsSrc;
		}
	}

	if (a.contains("hls_url") && a["hls_url"].is_string())
	{
		return a["hls_url"].get<std::string>();
	}

	if (a.contains("media_sources") && a["media_sources"].is_array())
	{
		for (auto& m : a["media_sources"])
		{
			if (m.contains("src") && m["src"].is_string())
			{
				return m["src"].get<std::string>();
			}
		}
	}

	throw std::runtime_error("no stream url found in lecture");
}


std::string RequestHandler::resolve_supplementary_asset(int course_id, int lecture_id, int asset_id) {
	std::ostringstream url;
	url << api_base_
		<< "/api-2.0/users/me/subscribed-courses/" << course_id
		<< "/lectures/" << lecture_id
		<< "/supplementary-assets/" << asset_id
		<< "?fields[supplementary_asset]=download_urls,external_url,asset_type,filename";

	auto body = udemy_get(url.str(), 15000);
	json j = json::parse(body);

	json* a = nullptr;
	if (j.contains("asset") && j["asset"].is_object())
	{
		a = &j["asset"];
	}
	else if (j.is_object())
	{
		a = &j;
	}

	if (!a) throw std::runtime_error("no asset in supplementary");

	if (a->contains("download_urls") && (*a)["download_urls"].is_object())
	{
		for (auto& [k, arr] : (*a)["download_urls"].items())
		{
			if (arr.is_array() && !arr.empty())
			{
				auto& first = arr[0];
				if (first.contains("file") && first["file"].is_string())
					return first["file"].get<std::string>();
				if (first.contains("url") && first["url"].is_string())
					return first["url"].get<std::string>();
			}
		}
	}
	if (a->contains("external_url") && (*a)["external_url"].is_string())
		return (*a)["external_url"].get<std::string>();

	throw std::runtime_error("no downloadable url in supplementary");
}


static inline double now_sec() {
	using namespace std::chrono;
	return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

void RequestHandler::worker_loop() {
	while (true)
	{
		Job j;

		{
			std::unique_lock<std::mutex> lk(mtx_);
			cv_.wait(lk, [&] { return !queue_.empty() || stop_; });
			if (stop_) break;

			auto it = std::find_if(queue_.begin(), queue_.end(),
								   [](const Job& x) { return x.state == Job::State::Queued; });
			if (it == queue_.end())
			{
				lk.unlock();
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				continue;
			}
			j = *it;
			it->state = Job::State::Downloading;
		}

		std::string lower_url = j.url;
		std::transform(lower_url.begin(), lower_url.end(), lower_url.begin(), [](unsigned char c) { return (char) std::tolower(c); });
		bool is_hls = lower_url.find(".m3u8") != std::string::npos;

		auto out_dir = std::filesystem::u8path(j.out_path_dir);
		std::filesystem::path target_path = out_dir / std::filesystem::u8path(j.filename);
		if (is_hls)
		{
			target_path.replace_extension(".ts");
			j.filename = Helper::path_to_utf8(target_path.filename());
			std::cout << "[HLS] Downloading HLS stream to TS: " << Helper::path_to_utf8(target_path) << std::endl;
		}

		j.out_path = Helper::path_to_utf8(target_path);

		std::atomic<double>     last_ts { now_sec() };
		std::atomic<long long>  last_bytes { 0 };

		auto on_progress = [this, &j, &last_ts, &last_bytes](double dlnow, double dltotal)
		{
			std::lock_guard<std::mutex> lk(mtx_);

			j.bytes_now = static_cast<long long>(dlnow);
			j.bytes_total = static_cast<long long>(dltotal);

			double ts = now_sec();
			const double dt = std::max(0.25, ts - last_ts.load());
			long long db = j.bytes_now - last_bytes.load();
			double inst = (double) db / dt; // B/s
			if (j.speed_bps <= 0) j.speed_bps = inst;
			else                  j.speed_bps = 0.25 * inst + 0.75 * j.speed_bps;

			last_ts.store(ts);
			last_bytes.store(j.bytes_now);

			if (j.bytes_total > 0)
				j.progress = (j.bytes_now * 100.0) / (double) j.bytes_total;

			for (auto& q : queue_) if (q.id == j.id)
			{
				q.bytes_now = j.bytes_now;
				q.bytes_total = j.bytes_total;
				q.speed_bps = j.speed_bps;
				q.progress = j.progress;
				q.filename = j.filename;
				q.out_path = j.out_path;
				break;
			}
		};

		{
			std::error_code ed;
			std::filesystem::create_directories(out_dir, ed);

			std::string msg = "";
			bool ok = false;
			if (is_hls)
			{
				ok = FFmpegHelper::convert_m3u8_to_ts(j.url, j.out_path, j.headers, proxy_, on_progress, msg);
			}
			else
			{
				ok = curl_download_file(j.url, j.out_path, j.headers, on_progress, msg);
			}

			{
				std::lock_guard<std::mutex> lk(mtx_);
				for (auto& q : queue_) if (q.id == j.id)
				{
					q.filename = j.filename;
					q.out_path = j.out_path;
					if (ok)
					{
						q.state = Job::State::Done;
						q.message = "ok";
						q.progress = 100.0;
						if (q.course_id)
						{
							auto itp = progress_.find(q.course_id);
							if (itp != progress_.end()) itp->second.done += 1;
						}
					}
					else
					{
						q.state = Job::State::Failed;
						q.message = msg.empty() ? "failed" : msg;
					}
					break;
				}
			}
		}
	}
}
