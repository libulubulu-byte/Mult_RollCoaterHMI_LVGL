# Roll Coater HMI —— 工程顶层 CMake
# 用法：
#   cd RollCoaterHMI && idf.py set-target esp32s3 && idf.py build
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(roll_coater_hmi)
