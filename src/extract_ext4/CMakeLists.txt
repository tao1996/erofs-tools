set(TARGET extract.ext4)

set(TARGET_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

file(GLOB EXTRACT_EXT4_SRCS "*.cpp")

# Prefer static archives so extract.ext4 links cleanly under the project-wide
# "-static" flag used on non-Darwin platforms.
set(_extract_ext4_saved_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
find_library(EXT2FS_STATIC_LIBRARY NAMES ext2fs)
find_library(COM_ERR_STATIC_LIBRARY NAMES com_err)
set(CMAKE_FIND_LIBRARY_SUFFIXES ${_extract_ext4_saved_suffixes})

find_path(EXT2FS_INCLUDE_DIR NAMES ext2fs/ext2fs.h)
find_path(COM_ERR_INCLUDE_DIR NAMES et/com_err.h)

if(NOT EXT2FS_STATIC_LIBRARY OR NOT COM_ERR_STATIC_LIBRARY
   OR NOT EXT2FS_INCLUDE_DIR OR NOT COM_ERR_INCLUDE_DIR)
    message(FATAL_ERROR "extract.ext4 requires e2fsprogs development files (libext2fs-dev, comerr-dev)")
endif()

add_executable(${TARGET} ${EXTRACT_EXT4_SRCS})

target_include_directories(${TARGET} PRIVATE
    ${EXT2FS_INCLUDE_DIR}
    ${COM_ERR_INCLUDE_DIR}
)

target_link_libraries(${TARGET} PRIVATE
    ${EXT2FS_STATIC_LIBRARY}
    ${COM_ERR_STATIC_LIBRARY}
    pthread
)

if (CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_STRIP} -w "$<TARGET_FILE:${TARGET}>"
    )
endif ()