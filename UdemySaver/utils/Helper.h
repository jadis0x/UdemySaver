#include <string>
#include <iomanip> 
#include <fstream>
#include <sstream>
#include <cctype>
#include <algorithm>

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
        std::string out;
        out.reserve(s.size());

        auto push_dash = [&]()
        {
            if (!out.empty() && out.back() != '-') out.push_back('-');
        };

        for (std::size_t i = 0; i < s.size();)
        {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80)
            {
                if (std::isalnum(c))
                {
                    out.push_back(static_cast<char>(std::tolower(c)));
                }
                else
                {
                    push_dash();
                }
                ++i;
                continue;
            }

            std::size_t len = 1;
            if ((c & 0xE0u) == 0xC0u) len = 2;
            else if ((c & 0xF0u) == 0xE0u) len = 3;
            else if ((c & 0xF8u) == 0xF0u) len = 4;

            if (i + len > s.size()) break;

            bool valid = true;
            for (std::size_t k = 1; k < len; ++k)
            {
                unsigned char cc = static_cast<unsigned char>(s[i + k]);
                if ((cc & 0xC0u) != 0x80u)
                {
                    valid = false;
                    break;
                }
            }

            if (valid)
            {
                out.append(s, i, len);
                i += len;
            }
            else
            {
                ++i;
            }
        }

        if (!out.empty())
        {
            auto first = out.find_first_not_of('-');
            if (first == std::string::npos)
            {
                out.clear();
            }
            else if (first > 0)
            {
                out.erase(0, first);
            }
        }

        if (!out.empty())
        {
            auto last = out.find_last_not_of('-');
            if (last != std::string::npos && last + 1 < out.size())
            {
                out.erase(last + 1);
            }
        }

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

    inline std::string extract_host(const std::string& url) {
        std::string working = url;
        auto scheme = working.find("://");
        std::size_t host_start = 0;
        if (scheme != std::string::npos)
        {
            host_start = scheme + 3;
        }

        if (host_start >= working.size()) return {};
        working = working.substr(host_start);

        auto at = working.find('@');
        if (at != std::string::npos)
        {
            working = working.substr(at + 1);
        }

        auto slash = working.find_first_of("/?#");
        if (slash != std::string::npos)
        {
            working = working.substr(0, slash);
        }

        auto colon = working.find(':');
        if (colon != std::string::npos)
        {
            working = working.substr(0, colon);
        }

        std::string lower = working;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower;
    }
}