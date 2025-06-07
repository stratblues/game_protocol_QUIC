cmake_minimum_required(VERSION 3.18)
project(papp LANGUAGES C CXX)

# ─── Find the MsQuic package (installed under /usr/local/share/msquic) ───
find_package(msquic CONFIG REQUIRED)

# ─── Your app ────────────────────────────────────────────────────────────
add_executable(papp
    papp.cpp
 "protocol.hpp")
target_link_libraries(papp
    PRIVATE msquic::msquic
)
set_property(TARGET papp PROPERTY CXX_STANDARD 20)

# ─── MsQuic sample (pure C) ─────────────────────────────────────────────
add_executable(quicsample
    "sample.cpp"
 "protocol.hpp")
# Tell CMake it’s ANSI C, not C++
set_property(TARGET quicsample PROPERTY C_STANDARD 11)
set_property(TARGET quicsample PROPERTY LINKER_LANGUAGE C)

target_link_libraries(quicsample
    PRIVATE msquic::msquic
)

# ─── Linux/WSL needs pthread + dl ────────────────────────────────────────
if(UNIX AND NOT APPLE)
    target_link_libraries(papp       PRIVATE pthread dl)
    target_link_libraries(quicsample PRIVATE pthread dl)
endif()
