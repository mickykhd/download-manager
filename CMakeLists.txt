cmake_minimum_required(VERSION 3.15)
project(cdm C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# On Windows there is no system libcurl dev package, so fetch+build it
# by default. On POSIX keep system curl by default.
if(WIN32)
    option(CDM_FETCH_CURL "Build libcurl from source (HTTP-only) via FetchContent" ON)
else()
    option(CDM_FETCH_CURL "Build libcurl from source (HTTP-only) via FetchContent" OFF)
endif()

# GUI needs OpenGL + GLEW + GLFW, rarely present on vanilla Windows.
# Default OFF on Windows, ON elsewhere. Missing deps skip the GUI with
# a warning instead of failing the whole configure.
if(WIN32)
    option(CDM_BUILD_GUI "Build the Nuklear/GLFW graphical front-end" OFF)
else()
    option(CDM_BUILD_GUI "Build the Nuklear/GLFW graphical front-end" ON)
endif()

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

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
    # Keep `cmake --install` / CPack app-only: skip libcurl headers/libs.
    set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(CURL_ENABLE_EXPORT_TARGET OFF CACHE BOOL "" FORCE)
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
# cdmcore itself uses Win32 threads on Windows and pthreads elsewhere,
# but libcurl's build and the GUI still need the Threads package located.
find_package(Threads QUIET)
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
if(CDM_BUILD_GUI)
    find_package(OpenGL QUIET)
    find_package(GLEW QUIET)
    find_library(GLFW_LIB glfw)
    if(NOT GLFW_LIB)
        find_package(glfw3 QUIET)
        if(TARGET glfw3::glfw3 OR TARGET glfw)
            # modern config package found; actual link target resolved below
        endif()
    endif()

    if(NOT OPENGL_FOUND OR NOT GLEW_FOUND OR (NOT GLFW_LIB AND NOT glfw3_FOUND))
        message(WARNING "CDM_BUILD_GUI=ON but OpenGL/GLEW/GLFW not found; skipping cdm-gui. Install GLFW+Glew or configure with -DCDM_BUILD_GUI=OFF.")
    else()
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
        # Resolve the GLFW link target: prefer the imported CMake target,
        # fall back to the raw library file found by find_library().
        set(CDM_GLFW_TARGET "")
        if(TARGET glfw3::glfw3)
            set(CDM_GLFW_TARGET glfw3::glfw3)
        elseif(TARGET glfw)
            set(CDM_GLFW_TARGET glfw)
        elseif(GLFW_LIB)
            set(CDM_GLFW_TARGET ${GLFW_LIB})
        endif()
        target_link_libraries(cdm-gui PRIVATE
            cdmcore
            OpenGL::GL
            GLEW::GLEW
            ${CDM_GLFW_TARGET}
        )
        if(NOT WIN32)
            target_link_libraries(cdm-gui PRIVATE Threads::Threads m)
        else()
            # Ship the third-party runtime DLLs (glfw3.dll, glew32.dll)
            # next to cdm-gui.exe so the installed app actually starts.
            install(IMPORTED_RUNTIME_ARTIFACTS GLEW::GLEW
                RUNTIME DESTINATION bin)
            if(TARGET glfw3::glfw3)
                install(IMPORTED_RUNTIME_ARTIFACTS glfw3::glfw3
                    RUNTIME DESTINATION bin)
            elseif(TARGET glfw)
                install(IMPORTED_RUNTIME_ARTIFACTS glfw
                    RUNTIME DESTINATION bin)
            endif()
        endif()
        install(TARGETS cdm-gui RUNTIME DESTINATION bin OPTIONAL)
    endif()
endif()

# --- Tests ---
enable_testing()
add_executable(test_range tests/test_range.c)
target_include_directories(test_range PRIVATE src/engine)
target_link_libraries(test_range PRIVATE cdmcore)
add_test(NAME test_range COMMAND test_range)

add_executable(test_settings tests/test_settings.c src/ui/manager.c)
target_include_directories(test_settings PRIVATE include src/engine src/ui)
target_link_libraries(test_settings PRIVATE cdmcore)
add_test(NAME test_settings COMMAND test_settings)

install(TARGETS cdm RUNTIME DESTINATION bin)

# --- Packaging (Windows installer via CPack: ZIP always, NSIS when available) ---
set(CPACK_PACKAGE_NAME "cdm-download-manager")
set(CPACK_PACKAGE_VENDOR "cdm")
set(CPACK_PACKAGE_VERSION "0.1.0")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "cdm - segmented download manager (CLI)")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
endif()
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
    find_program(NSIS_MAKENSIS makensis)
    if(NSIS_MAKENSIS)
        list(APPEND CPACK_GENERATOR "NSIS")
    endif()
    set(CPACK_NSIS_DISPLAY_NAME "cdm Download Manager")
    set(CPACK_NSIS_PACKAGE_NAME "cdm Download Manager")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
endif()
include(CPack)
