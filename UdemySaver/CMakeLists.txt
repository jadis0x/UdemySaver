# Add source to this project's executable.
set (SOURCES 
    "src/main.cpp"
    "src/server/HttpServer.cpp"
    "src/server/HttpServer.h"
    "src/server/RequestHandler.h"
    "src/server/RequestHandler.cpp"
)

if (WIN32)
    list(APPEND SOURCES "../resources/app.rc")
endif()

add_executable (UdemySaver ${SOURCES} "utils/helper.h")

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET UdemySaver PROPERTY CXX_STANDARD 20)
endif()

target_include_directories(UdemySaver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)

# Curl
find_package(CURL REQUIRED)
target_link_libraries(UdemySaver PRIVATE CURL::libcurl)

# boost-beast
find_package(Boost REQUIRED COMPONENTS system thread)
target_link_libraries(UdemySaver PRIVATE Boost::system Boost::thread)

# nlohmann_json
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(UdemySaver PRIVATE nlohmann_json::nlohmann_json)

find_package(FFMPEG REQUIRED)
target_include_directories(UdemySaver PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(UdemySaver PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(UdemySaver PRIVATE ${FFMPEG_LIBRARIES})


set(WEB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/www)
file(MAKE_DIRECTORY ${WEB_ROOT})

add_custom_command(TARGET UdemySaver POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:UdemySaver>/www
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${WEB_ROOT}
            $<TARGET_FILE_DIR:UdemySaver>/www
    COMMENT "Copying www to output dir"
)


set_property(TARGET UdemySaver PROPERTY
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:UdemySaver>")