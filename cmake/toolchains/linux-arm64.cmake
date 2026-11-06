# Cross-compiles for Linux on ARM64 (aarch64) from an x86-64 Debian/Ubuntu host with Clang.
#
# The target libraries come from the host's multiarch packages (dpkg --add-architecture arm64,
# then the :arm64 -dev packages listed in docs/PORTING.md), so no separate sysroot is needed.
# REX_CLANG_SUFFIX picks a versioned Clang ("-21" for clang-21); empty uses plain clang.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)

set(_triple aarch64-linux-gnu)
if(NOT DEFINED REX_CLANG_SUFFIX)
    set(REX_CLANG_SUFFIX "$ENV{REX_CLANG_SUFFIX}")
endif()
set(CMAKE_C_COMPILER "clang${REX_CLANG_SUFFIX}")
set(CMAKE_CXX_COMPILER "clang++${REX_CLANG_SUFFIX}")
set(CMAKE_C_COMPILER_TARGET ${_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_triple})
set(CMAKE_ASM_COMPILER_TARGET ${_triple})
# lld handles the long branches of a very large executable with range-extension thunks.
set(CMAKE_LINKER_TYPE LLD)
foreach(_kind EXE SHARED MODULE)
    set(CMAKE_${_kind}_LINKER_FLAGS_INIT "-fuse-ld=lld")
endforeach()

# Look for target libraries and headers only in the arm64 multiarch folders, programs on the host.
set(CMAKE_FIND_ROOT_PATH /)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/${_triple}/pkgconfig:/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")

# try_run() and the built tools run through qemu-user when they must run at configure time.
find_program(_qemu qemu-aarch64)
if(_qemu)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${_qemu};-L;/")
endif()
