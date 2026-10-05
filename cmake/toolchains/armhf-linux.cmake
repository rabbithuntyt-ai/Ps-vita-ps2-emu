# ARM Linux (Cortex-A9 like the Vita) cross build, used to run the emulator,
# including Play!'s AArch32 recompiler, under qemu-arm on a development
# machine:
#
#   cmake -S . -B build-arm -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/armhf-linux.cmake
#   cmake --build build-arm && ctest --test-dir build-arm
#
# Needs g++-arm-linux-gnueabihf, qemu-user and armhf zlib/bzip2/openssl.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard")
set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf /usr/lib/arm-linux-gnueabihf)
set(CMAKE_LIBRARY_ARCHITECTURE arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-arm -L /usr/arm-linux-gnueabihf)
