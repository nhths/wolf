cmake_minimum_required(VERSION 3.17)
project(wolf_bench_client C)

# moonlight-common-c проверяет CLOCK_MONOTONIC через CHECK_CXX_SOURCE_COMPILES,
# а сам объявляет только язык C — компилятор C++ должен быть включён заранее.
enable_language(CXX)

# Стенд — самостоятельный проект, он НЕ входит в сборку wolf: у wolf своя
# C++-реализация протокола, а клиенту нужен настоящий Moonlight-клиент
# (moonlight-common-c), на котором написан и Moonlight, и moonlight-qt.
#
# Зависимость тянется через FetchContent — ровно так же, как wolf сам
# подтягивает fmt/eventbus/nanors. Сабмодули enet и nanors нужны самому
# moonlight-common-c, поэтому GIT_SUBMODULES_RECURSE.

include(FetchContent)

FetchContent_Declare(
        moonlight-common-c
        GIT_REPOSITORY https://github.com/moonlight-stream/moonlight-common-c.git
        GIT_TAG e80a786afedeaccff63e839e68c8f3d8a057c13d
        GIT_SUBMODULES enet nanors
        GIT_SUBMODULES_RECURSE TRUE
)

# Статическая библиотека: клиент — самодостаточный бинарь, который можно
# скинуть на любой хост и запустить против wolf.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# moonlight-common-c собирается с -Werror, а в OpenSSL 3.0 часть
# используемых им функций помечена deprecated — без этого флага сборка
# падает на чужих предупреждениях.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-deprecated-declarations")

FetchContent_MakeAvailable(moonlight-common-c)

find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

add_executable(bench-client
        main.c
        util.c
        http.c
        identity.c
        pairing.c
)

target_include_directories(bench-client PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(bench-client PRIVATE moonlight-common-c OpenSSL::Crypto CURL::libcurl)
target_compile_options(bench-client PRIVATE -Wall -Wextra -Wno-unused-parameter)

install(TARGETS bench-client RUNTIME DESTINATION bin)
