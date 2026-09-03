# Kernel build environment for monterey (Quest 1, msm8998).
# Toolchain is GCC 4.9.x 20150123 (prerelease) -- byte-identical to the compiler string in the
# running kernel's /proc/version, so the build matches what Meta shipped.
export ARCH=arm64
export SUBARCH=arm64
export CROSS_COMPILE=/home/ryanm/diytech/quest/work/kbuild/aarch64-linux-android-4.9/bin/aarch64-linux-android-
export KSRC=/home/ryanm/diytech/quest/work/oculus-kernel
export KOUT=/home/ryanm/diytech/quest/work/kbuild/out
# 32-bit toolchain is required only for the arm64 compat vDSO; the kernel Makefile hard-errors
# without it even though nothing else in this build is 32-bit.
export CROSS_COMPILE_ARM32=/home/ryanm/diytech/quest/work/kbuild/arm-linux-androideabi-4.9/bin/arm-linux-androideabi-
# Host tools in this 2016-era tree predate GCC 10's -fno-common default, so scripts/dtc fails to
# link with "multiple definition of yylloc". Restore -fcommon for HOST tools only -- the kernel
# itself is built by GCC 4.9 and is unaffected.
# Fedora's system OpenSSL 3.x no longer ships openssl/engine.h (deprecated), which scripts/
# extract-cert.c still includes -- it is needed because CONFIG_SYSTEM_TRUSTED_KEYRING=y in the
# device's own config, and that config is kept byte-identical. Homebrew's openssl@3 still has it.
export KHOSTCFLAGS="-Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu89 -fcommon -Wno-error -I/home/linuxbrew/.linuxbrew/opt/openssl@3/include"
export KHOSTLDFLAGS="-L/home/linuxbrew/.linuxbrew/opt/openssl@3/lib"
