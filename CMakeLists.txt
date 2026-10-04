cmake_minimum_required(VERSION 3.15)
project(cdm C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(CDM_FETCH_CURL "Build libcurl from source (HTTP-only) via FetchContent" OFF)

# --- Platform sources ---
if(WIN32)
    set(CDM_PLATFORM_SRC src/platform/platform_win.c)
else()
    set(CDM_PLATFORM_SRC src/platform/platform_posix.c)
endif()

set(CDM_ENGINE_SRC
    src/engine/download.c
    src/engine/httpinfo.c
    src/engine/segment.c
    src/engine/scheduler.c
    src/engine/retry.c
    src/engine/resume.c
    src/engine/speedmeter.c
    src/engine/integrity.c
)

add_library(cdmcore STATIC ${CDM_ENGINE_SRC} ${CDM_PLATFORM_SRC})
target_include_directories(cdmcore PUBLIC include PRIVATE src/engine)

# --- libcurl ---
if(CDM_FETCH_CURL)
    include(FetchContent)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(HTTP_ONLY ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
    if(WIN32)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(curl
        GIT_REPOSITORY https://github.com/curl/curl.git
        GIT_TAG curl-8_11_0)
    FetchContent_MakeAvailable(curl)
    target_link_libraries(cdmcore PUBLIC libcurl_static)
    target_compile_definitions(cdmcore PUBLIC CURL_STATICLIB)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(cdmcore PUBLIC CURL::libcurl)
endif()

# --- Threads ---
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(cdmcore PUBLIC Threads::Threads)
endif()
if(WIN32)
    target_link_libraries(cdmcore PUBLIC ws2_32 crypt32)
endif()

# --- CLI executable ---
add_executable(cdm src/cli/main.c)
target_link_libraries(cdm PRIVATE cdmcore)

# --- GUI executable (Nuklear + GLFW/OpenGL) ---
option(CDM_BUILD_GUI "Build the Nuklear/GLFW graphical front-end" ON)
if(CDM_BUILD_GUI)
    find_package(OpenGL REQUIRED)
    find_package(GLEW REQUIRED)
    find_library(GLFW_LIB glfw)
    if(NOT GLFW_LIB)
        find_package(glfw3 QUIET)
    endif()

    add_executable(cdm-gui
        src/ui/gui.c
        src/ui/manager.c
    )
    target_include_directories(cdm-gui PRIVATE
        include
        src/engine
        src/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nuklear
    )
    target_link_libraries(cdm-gui PRIVATE
        cdmcore
        OpenGL::GL
        GLEW::GLEW
        glfw
        Threads::Threads
        m
    )
    install(TARGETS cdm-gui RUNTIME DESTINATION bin OPTIONAL)
endif()

# --- Tests ---
enable_testing()
add_executable(test_range tests/test_range.c)
target_include_directories(test_range PRIVATE src/engine)
target_link_libraries(test_range PRIVATE cdmcore)
add_test(NAME test_range COMMAND test_range)

add_executable(test_settings tests/test_settings.c src/ui/manager.c)
target_include_directories(test_settings PRIVATE src/engine src/ui)
target_link_libraries(test_settings PRIVATE cdmcore)
add_test(NAME test_settings COMMAND test_settings)

install(TARGETS cdm RUNTIME DESTINATION bin)
