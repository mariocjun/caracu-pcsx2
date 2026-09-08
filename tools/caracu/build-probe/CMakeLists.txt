# SPDX-FileCopyrightText: 2026 Caracu contributors
# SPDX-License-Identifier: GPL-3.0+
cmake_minimum_required(VERSION 3.22)
project(caracu_toolchain_probe LANGUAGES CXX)
add_executable(probe main.cpp)
target_compile_features(probe PRIVATE cxx_std_20)
