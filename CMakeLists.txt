cmake_minimum_required(VERSION 3.18)
project(papp LANGUAGES C CXX)


find_package(msquic CONFIG REQUIRED)



add_executable(myprotocol
    "gameprotocol.cpp"
 "protocol.hpp")

set_property(TARGET myprotocol PROPERTY C_STANDARD 11)
set_property(TARGET myprotocol PROPERTY LINKER_LANGUAGE C)

target_link_libraries(myprotocol
    PRIVATE msquic::msquic
)

if(UNIX AND NOT APPLE)
    target_link_libraries(myprotocol PRIVATE pthread dl)
endif()
