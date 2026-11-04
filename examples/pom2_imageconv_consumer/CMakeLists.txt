cmake_minimum_required(VERSION 3.16)
project(pom2_imageconv_consumer LANGUAGES CXX)

find_package(pom2_imageconv CONFIG REQUIRED)

add_executable(pom2_imageconv_consumer main.cpp)
target_compile_features(pom2_imageconv_consumer PRIVATE cxx_std_17)
target_link_libraries(pom2_imageconv_consumer PRIVATE POM2::imageconv POM2::prodosvolume)
