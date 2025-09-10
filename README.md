# UdemySaver
UdemySaver is a lightweight C++ application that lets you download Udemy courses you own for offline use.
It ships with a small HTTP server and a modern web UI so you can browse your library, queue downloads, and monitor progress from your browser.

# Configuration
Create a settings.ini alongside the executable:
```bash
# UdemySaver settings
udemy_access_token=PASTE_YOUR_TOKEN_HERE
udemy_api_base=https://www.udemy.com
# http_proxy=http://127.0.0.1:8080   ; optional
```
You can also start the program without a token and paste it via the web interface; the file will be created automatically.

# Requirements
| Tool/Library           | Notes                     |
| ---------------------- | ------------------------- |
| C++20 compiler         | MSVC / Clang / GCC        |
| CMake ≥ 3.8            | Build system              |
| Boost (system, thread, beast) | Networking primitives     |
| libcurl                | HTTP requests & downloads |
| nlohmann\_json         | JSON parsing              |




# Usage

1. Run the application:

```bash
./UdemySaver
```

2. Open a browser at http://127.0.0.1:8080.
3. If prompted, paste your Udemy access token.
4. Browse your library, choose a course, and click Download.
5. Files are saved under a downloads/ directory (auto‑named per course/lecture).

# Legal
This tool is intended for downloading courses you have legally purchased.
Respect Udemy’s Terms of Service and local laws when using it.

# License
This project is distributed under the MIT License—see the LICENSE file for details.
