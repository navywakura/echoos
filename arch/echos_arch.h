/*
 * echOS architecture identity. The shared Heap-0 kernel today ships
 * as x86_64 Multiboot2; i386 and aarch64 are first-class *targets* for
 * Complete-edition static ELFs (see docs/ECHOS_TARGET.md).
 */
#ifndef ECHOS_ARCH_H
#define ECHOS_ARCH_H

#if defined(__x86_64__) || defined(__amd64__)
#  define ECHOS_ARCH        "x86_64"
#  define ECHOS_ARCH_BITS   64
#  define ECHOS_ARCH_FAMILY "x86"
#elif defined(__i386__)
#  define ECHOS_ARCH        "x32"
#  define ECHOS_ARCH_BITS   32
#  define ECHOS_ARCH_FAMILY "x86"
#elif defined(__aarch64__)
#  define ECHOS_ARCH        "aarch64"
#  define ECHOS_ARCH_BITS   64
#  define ECHOS_ARCH_FAMILY "arm"
#elif defined(__arm__)
#  define ECHOS_ARCH        "arm"
#  define ECHOS_ARCH_BITS   32
#  define ECHOS_ARCH_FAMILY "arm"
#else
#  define ECHOS_ARCH        "unknown"
#  define ECHOS_ARCH_BITS   0
#  define ECHOS_ARCH_FAMILY "unknown"
#endif

#define ECHOS_TARGET_X86_64  "x86_64-unknown-echos"
#define ECHOS_TARGET_AARCH64 "aarch64-unknown-echos"
#define ECHOS_TARGET_X32     "i686-unknown-echos"

#endif
