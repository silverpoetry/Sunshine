# load common dependencies
# this file will also load platform specific dependencies

# Resolve OpenSSL before subprojects run their own find_package(OpenSSL) calls.
# This ensures a user-provided OPENSSL_ROOT_DIR is honored consistently.
find_package(OpenSSL REQUIRED)

# boost, this should be before Simple-Web-Server as it also depends on boost
include(dependencies/Boost_Sunshine)

# submodules
# moonlight common library
set(ENET_NO_INSTALL ON CACHE BOOL "Don't install any libraries built for enet")
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/enet")

# The pinned upstream head still declares compatibility with CMake 3.1. Keep
# its deprecation warning scoped to that dependency so Sunshine's own CMake
# warnings remain visible. CMake reads this diagnostic setting from the cache,
# so preserve and restore the caller's exact state around add_subdirectory().
function(add_simple_web_server_dependency)
    get_property(warn_deprecated_was_set CACHE CMAKE_WARN_DEPRECATED PROPERTY TYPE SET)
    if(warn_deprecated_was_set)
        get_property(warn_deprecated_value CACHE CMAKE_WARN_DEPRECATED PROPERTY VALUE)
    endif()
    set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "Emit CMake deprecation warnings" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/Simple-Web-Server")
    if(warn_deprecated_was_set)
        set(CMAKE_WARN_DEPRECATED "${warn_deprecated_value}" CACHE BOOL
                "Emit CMake deprecation warnings" FORCE)
    else()
        unset(CMAKE_WARN_DEPRECATED CACHE)
    endif()
endfunction()
add_simple_web_server_dependency()

# libdisplaydevice
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/libdisplaydevice")

if(SUNSHINE_ENABLE_TRAY)
    add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/tray")
endif()

# common dependencies
include("${CMAKE_MODULE_PATH}/dependencies/nlohmann_json.cmake")
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(CURL REQUIRED libcurl)

# miniupnp
pkg_check_modules(MINIUPNP miniupnpc REQUIRED)
include_directories(SYSTEM ${MINIUPNP_INCLUDE_DIRS})

# ffmpeg pre-compiled binaries
include("${CMAKE_MODULE_PATH}/dependencies/ffmpeg.cmake")

# Opus
# Homebrew provides opus as a dynamic library only, so disable static linking for Homebrew builds
if(SUNSHINE_BUILD_HOMEBREW)
    set(OPUS_USE_STATIC OFF CACHE BOOL "Static linking for libopus")
else()
    set(OPUS_USE_STATIC ON CACHE BOOL "Static linking for libopus")
endif()
include("${CMAKE_MODULE_PATH}/dependencies/FindOpus.cmake")

# platform specific dependencies
if(WIN32)
    include("${CMAKE_MODULE_PATH}/dependencies/windows.cmake")
elseif(UNIX)
    include("${CMAKE_MODULE_PATH}/dependencies/unix.cmake")

    if(APPLE)
        include("${CMAKE_MODULE_PATH}/dependencies/macos.cmake")
    else()
        include("${CMAKE_MODULE_PATH}/dependencies/linux.cmake")
    endif()
endif()
