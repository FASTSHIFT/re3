# CMake toolchain file for cross-compiling re3 to Raspberry Pi (armhf / 32-bit)
# Target: Raspberry Pi Zero 2 W, Raspbian 13 (trixie), armv7l, Cortex-A53
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=$(pwd)/rpi-armhf-toolchain.cmake ...

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# --- Sysroot synced from the Raspberry Pi ---
set(RPI_SYSROOT "/home/vifex/workpath/rpi-sysroot" CACHE PATH "Path to Raspberry Pi sysroot")
set(CMAKE_SYSROOT "${RPI_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${RPI_SYSROOT}")

# --- Cross compilers ---
set(CMAKE_C_COMPILER   arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

# --- CPU tuning for Cortex-A53 (Pi Zero 2 W / Pi 3), armv7 hard-float ---
set(RPI_ARCH_FLAGS "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a53")
set(CMAKE_C_FLAGS_INIT   "${RPI_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${RPI_ARCH_FLAGS}")

# --- Make the linker resolve libraries against the sysroot ---
# The multiarch triplet dir + rpath-link so transitive .so deps are found.
set(_rpi_triplet "arm-linux-gnueabihf")
set(_rpi_link_flags
    "-L${RPI_SYSROOT}/usr/lib/${_rpi_triplet} \
     -L${RPI_SYSROOT}/lib/${_rpi_triplet} \
     -Wl,-rpath-link,${RPI_SYSROOT}/usr/lib/${_rpi_triplet} \
     -Wl,-rpath-link,${RPI_SYSROOT}/lib/${_rpi_triplet}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_rpi_link_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_rpi_link_flags}")

# --- Search behaviour: programs from host, libs/headers/pkgconfig from sysroot ---
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# --- pkg-config against the sysroot ---
set(ENV{PKG_CONFIG_DIR} "")
set(ENV{PKG_CONFIG_LIBDIR} "${RPI_SYSROOT}/usr/lib/${_rpi_triplet}/pkgconfig:${RPI_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${RPI_SYSROOT}")
