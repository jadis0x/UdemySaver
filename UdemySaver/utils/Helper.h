#include <string>
#include <iomanip> 
#include <fstream>
#include <sstream>

extern "C" {
#include <libavutil/error.h>
}

namespace Helper
{
	inline FILE* xfopen(const char* path, const char* mode) {
	#ifdef _MSC_VER
		FILE* fp = nullptr;
		if (fopen_s(&fp, path, mode) != 0) return nullptr;
		return fp;
	#else
		return std::fopen(path, mode);
	#endif
	}

	inline std::string slugify(const std::string& s) {
		std::string out; out.reserve(s.size());
		for (unsigned char c : s)
		{
			if (std::isalnum(c)) out.push_back((char) std::tolower(c));
			else if (!out.empty() && out.back() != '-') out.push_back('-');
		}
		while (!out.empty() && out.back() == '-') out.pop_back();
		if (out.empty()) out = "course";
		return out;
	}

	inline std::string ff_errstr(int err) {
		char buf[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(err, buf, sizeof(buf));
		return std::string(buf);
	}

	inline std::string course_dir(int course_id, const std::string& title) {
		std::ostringstream ss;
		ss << "downloads/"
			<< slugify(title) << "-" << course_id;
		return ss.str();
	}

	inline std::string trim(std::string s) {
		auto ws = [](int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
		while (!s.empty() && ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
		while (!s.empty() && ws(static_cast<unsigned char>(s.back())))  s.pop_back();
		return s;
	}

	inline std::string read_file_utf8(const std::string& path) {
		std::ifstream f(path, std::ios::binary);
		if (!f) return {};
		std::ostringstream ss; ss << f.rdbuf(); return ss.str();
	}

	inline std::string zpad(int n, int width) {
		std::ostringstream ss; ss << std::setw(width) << std::setfill('0') << n; return ss.str();
	}

	inline std::string section_dir(int idx, const std::string& title) {
		return zpad(idx, 2) + " - " + slugify(title);
	}

	inline size_t write_to_string(void* ptr, size_t size, size_t nmemb, void* userdata) {
		auto* s = reinterpret_cast<std::string*>(userdata);
		size_t add = size * nmemb;
		s->append(static_cast<char*>(ptr), add);
		return add;
	}


	inline size_t write_discard(void* ptr, size_t size, size_t nmemb, void*) {
		return size * nmemb;
	}

	inline int extract_quality_value(const std::string& label) {
		std::string digits;
		digits.reserve(label.size());
		for (char c : label)
		{
			if (std::isdigit(static_cast<unsigned char>(c))) digits.push_back(c);
		}
		if (digits.empty()) return 0;
		try
		{
			return std::stoi(digits);
		}
		catch (...)
		{
			return 0;
		}
	}

}