cmake_minimum_required(VERSION 3.16)
project(extract_ext4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_path(EXT2FS_INCLUDE_DIR ext2fs/ext2fs.h)
find_path(COM_ERR_INCLUDE_DIR et/com_err.h)
find_library(EXT2FS_LIBRARY ext2fs)
find_library(COM_ERR_LIBRARY com_err)

if(NOT EXT2FS_INCLUDE_DIR OR NOT EXT2FS_LIBRARY)
    message(FATAL_ERROR "e2fsprogs development files not found (libext2fs-dev)")
endif()

add_executable(extract.ext4 main.cpp)
target_include_directories(extract.ext4 PRIVATE
    ${EXT2FS_INCLUDE_DIR}
    ${COM_ERR_INCLUDE_DIR}
)
target_link_libraries(extract.ext4 PRIVATE
    ${EXT2FS_LIBRARY}
    ${COM_ERR_LIBRARY}
    pthread
)
target_compile_options(extract.ext4 PRIVATE -O2 -Wall)