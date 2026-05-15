/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_COMPILER_TYPES_H
#error "Please don't include <linux/compiler-clang.h> directly, include <linux/compiler.h> instead."
#endif

/* Compiler specific definitions for Clang compiler */

#define uninitialized_var(x) x = *(&(x))

/* same as gcc, this was present in clang-2.6 so we can assume it works
 * with any version that can compile the kernel
 */
#define __UNIQUE_ID(prefix) __PASTE(__PASTE(__UNIQUE_ID_, prefix), __COUNTER__)

/* all clang versions usable with the kernel support KASAN ABI version 5 */
#define KASAN_ABI_VERSION 5

#if __has_feature(address_sanitizer) || __has_feature(hwaddress_sanitizer)
/* emulate gcc's __SANITIZE_ADDRESS__ flag */
#define __SANITIZE_ADDRESS__
#define __no_sanitize_address \
		__attribute__((no_sanitize("address", "hwaddress")))
#else
#define __no_sanitize_address
#endif

/*
 * Not all versions of clang implement the the type-generic versions
 * of the builtin overflow checkers. Fortunately, clang implements
 * __has_builtin allowing us to avoid awkward version
 * checks. Unfortunately, we don't know which version of gcc clang
 * pretends to be, so the macro may or may not be defined.
 */
#if __has_builtin(__builtin_mul_overflow) && \
    __has_builtin(__builtin_add_overflow) && \
    __has_builtin(__builtin_sub_overflow)
#define COMPILER_HAS_GENERIC_BUILTIN_OVERFLOW 1
#endif

/*
 * Turn individual warnings and errors on and off locally, depending
 * on version.
 */
#define __diag_clang(version, severity, s) \
	__diag_clang_ ## version(__diag_clang_ ## severity s)

/* Severity used in pragma directives */
#define __diag_clang_ignore	ignored
#define __diag_clang_warn	warning
#define __diag_clang_error	error

#define __diag_str1(s)		#s
#define __diag_str(s)		__diag_str1(s)
#define __diag(s)		_Pragma(__diag_str(clang diagnostic s))

#if CONFIG_CLANG_VERSION >= 110000
#define __diag_clang_11(s)	__diag(s)
#else
#define __diag_clang_11(s)
#endif

#if CONFIG_CLANG_VERSION >= 230000
#define __diag_clang_23(s)	__diag(s)
#else
#define __diag_clang_23(s)
#endif
