# ESP-IDF project for BasmioTwo / JC3248W535EN (ESP32-S3 + AXS15231B QSPI LCD)
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSION" PROJECT_VER)
string(STRIP "${PROJECT_VER}" PROJECT_VER)
project(BasmioTwo)
