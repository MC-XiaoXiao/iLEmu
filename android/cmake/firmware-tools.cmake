# Host-side firmware tools, pinned to audited upstream sources. These run as
# separate Android processes; guest/runtime code does not depend on them.
include(FetchContent)
find_program(ILEMU_PATCH patch REQUIRED)
FetchContent_Declare(firmware_dmg2img
    URL https://codeload.github.com/Lekensteyn/dmg2img/tar.gz/a3e413489ccdd05431401357bf21690536425012
    URL_HASH SHA256=87240ddede144913f8300d2fe8fe81ee41026e1dc6514b475ae4ad69fb701268 DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR disabled)
FetchContent_Declare(firmware_hfsfuse
    URL https://codeload.github.com/0x09/hfsfuse/tar.gz/f5b3b80eec8c6ef7bb4113c755b29e2d31d3d746
    URL_HASH SHA256=22a21c79375e8bc329b11067c4990cbb6efb7afe3b344db2f39d92bef4cef4fb
    PATCH_COMMAND "${ILEMU_PATCH}" -p1 -i "${CMAKE_CURRENT_LIST_DIR}/patches/hfstar-guest-metadata.patch"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR disabled)
FetchContent_Declare(firmware_bzip2
    URL https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz
    URL_HASH SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269 DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR disabled)
FetchContent_Declare(firmware_lzfse
    URL https://codeload.github.com/lzfse/lzfse/tar.gz/e634ca58b4821d9f3d560cdc6df5dec02ffc93fd
    URL_HASH SHA256=ca98aa6644d44500e3315858daa747ce15bd06d49e3edb12a5458e5525e8ebdb DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR disabled)
# SOURCE_SUBDIR prevents upstream CMake's host auto-discovery; compile only the
# libraries/tools needed for read-only preparation with NDK dependencies.
FetchContent_MakeAvailable(firmware_dmg2img firmware_hfsfuse firmware_bzip2 firmware_lzfse)

set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_TAR OFF CACHE BOOL "" FORCE)
set(ENABLE_CPIO OFF CACHE BOOL "" FORCE)
set(ENABLE_CAT OFF CACHE BOOL "" FORCE)
set(ENABLE_ACL OFF CACHE BOOL "" FORCE)
set(ENABLE_XATTR OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBB2 OFF CACHE BOOL "" FORCE)
set(ENABLE_LZMA OFF CACHE BOOL "" FORCE)
set(ENABLE_ZSTD OFF CACHE BOOL "" FORCE)
set(ENABLE_BZip2 OFF CACHE BOOL "" FORCE)
set(ENABLE_LZ4 OFF CACHE BOOL "" FORCE)
set(ENABLE_EXPAT OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBXML2 OFF CACHE BOOL "" FORCE)
set(ENABLE_PCREPOSIX OFF CACHE BOOL "" FORCE)
set(ENABLE_PCRE2POSIX OFF CACHE BOOL "" FORCE)
FetchContent_Declare(firmware_archive
    URL https://codeload.github.com/libarchive/libarchive/tar.gz/refs/tags/v3.8.9
    URL_HASH SHA256=744346f6bca195c8f894f847bb32a16e9bcae6002624a58fadc81e80f595b3cb
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(firmware_archive)

set(bz ${firmware_bzip2_SOURCE_DIR})
add_library(firmware_bz STATIC ${bz}/blocksort.c ${bz}/huffman.c ${bz}/crctable.c
    ${bz}/randtable.c ${bz}/compress.c ${bz}/decompress.c ${bz}/bzlib.c)
target_include_directories(firmware_bz PUBLIC ${bz})
set(lz ${firmware_lzfse_SOURCE_DIR}/src)
add_library(firmware_lz STATIC ${lz}/lzfse_decode.c ${lz}/lzfse_decode_base.c
    ${lz}/lzvn_decode_base.c ${lz}/lzfse_fse.c)
target_include_directories(firmware_lz PUBLIC ${lz})

set(dmg ${firmware_dmg2img_SOURCE_DIR})
add_executable(firmware_dmg ${dmg}/dmg2img.c ${dmg}/base64.c ${dmg}/adc.c)
target_compile_definitions(firmware_dmg PRIVATE HAVE_LZFSE)
target_link_libraries(firmware_dmg PRIVATE firmware_bz firmware_lz z)
add_executable(firmware_decrypt ${dmg}/vfdecrypt.c)
target_link_libraries(firmware_decrypt PRIVATE OpenSSL::Crypto)

set(hfs ${firmware_hfsfuse_SOURCE_DIR})
file(GLOB hfs_sources CONFIGURE_DEPENDS ${hfs}/lib/libhfs/*.c ${hfs}/lib/libhfsuser/*.c)
add_library(firmware_hfs STATIC ${hfs_sources} ${hfs}/lib/utf8proc/utf8proc.c
    ${hfs}/lib/LZVN/lzvn_decode.c)
foreach(unit libhfs unicode)
    set_source_files_properties(${hfs}/lib/libhfs/${unit}.c PROPERTIES
        COMPILE_OPTIONS "-D__KERNEL_RCSID(sec,string)=const char hfs_rcsid_${unit}[]=string")
endforeach()
target_include_directories(firmware_hfs PUBLIC ${hfs}/lib/libhfs ${hfs}/lib/libhfsuser
    ${hfs}/lib/uthash ${hfs}/lib/utf8proc ${hfs}/lib/LZVN)
target_compile_definitions(firmware_hfs PUBLIC HAVE_ZLIB HAVE_UTF8PROC HAVE_LZVN
    HAVE_BEXXTOH_ENDIAN_H HAVE_STAT_BLKSIZE HAVE_STAT_BLOCKS HAVE_PREAD
    HFSFUSE_VERSION_STRING="f5b3b80" _GNU_SOURCE)
target_link_libraries(firmware_hfs PUBLIC z)
add_executable(firmware_hfstar ${hfs}/src/hfstar.c)
target_compile_definitions(firmware_hfstar PRIVATE XATTR_NAMESPACE=user.)
target_include_directories(firmware_hfstar PRIVATE ${firmware_archive_SOURCE_DIR}/libarchive ${firmware_archive_BINARY_DIR})
target_link_libraries(firmware_hfstar PRIVATE firmware_hfs archive_static)

# Package PIE executables in the native library directory so Android's exec
# policy permits running them. AGP packages the .so target outputs as JNI artifacts.
foreach(tool firmware_dmg firmware_decrypt firmware_hfstar)
    set_target_properties(${tool} PROPERTIES OUTPUT_NAME "lib${tool}" SUFFIX ".so"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
    target_link_options(${tool} PRIVATE -Wl,-z,max-page-size=16384)
endforeach()
