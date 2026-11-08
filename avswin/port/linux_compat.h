/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * linux_compat.h -- minimal Linux-kernel API shim for the ported Intel AVS
 * core (messages.c, topology.c, path.c) so those files can be compiled
 * with as few edits as possible in two environments:
 *
 *   1. Windows kernel mode (WDK/KMDF, MSVC, C compiled with /std:clatest
 *      so that `typeof` is available)                     -> AVS_PORT_KERNEL
 *   2. Linux/Windows user mode (gcc/clang), for the offline test harness
 *      that turns a real .tplg + NHLT into the exact IPC sequence the
 *      Linux driver would send                              -> otherwise
 *
 * Only what the ported files actually use is provided. Locking primitives
 * are deliberately no-ops: the glue layer serializes every call into the
 * ported core with one coarse lock, so the fine-grained Linux locks inside
 * are redundant (and guard()/scoped_guard() rely on GCC's cleanup
 * attribute, which MSVC does not have).
 */
#ifndef AVS_LINUX_COMPAT_H
#define AVS_LINUX_COMPAT_H

#if defined(_KERNEL_MODE)
#define AVS_PORT_KERNEL 1
#include <ntddk.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
/* No <stdint.h>/<stdbool.h>/<limits.h>: not guaranteed on the km include path. */
#ifndef _INTPTR_T_DEFINED
#define _INTPTR_T_DEFINED
typedef LONG_PTR intptr_t;
#endif
#ifndef _UINTPTR_T_DEFINED
#define _UINTPTR_T_DEFINED
typedef ULONG_PTR uintptr_t;
#endif
#if !defined(__cplusplus) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L)
#ifndef bool
#define bool  _Bool
#define true  1
#define false 0
#endif
#endif
#ifndef UINT_MAX
#define UINT_MAX 0xffffffffU
#endif
#ifndef INT_MAX
#define INT_MAX 0x7fffffff
#endif
#else
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <limits.h>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
/*
 * Upstream Linux code is written for gcc -Wall, not MSVC /W4: silence the
 * purely stylistic MSVC warnings for the ported translation units only
 * (only they include this header). Anything about layout is still caught
 * by the upstream static_asserts.
 */
/* implicit narrowing (u32->u8, s64->int, ...) as written upstream */
#pragma warning(disable: 4242 4244 4245 4254 4267 4305 4306 4309 4310 4334 4389 4018 4146)
/* GNU/C99-isms MSVC flags as "nonstandard" */
#pragma warning(disable: 4200 4201 4204 4214 4221 4232 4152 4295)
/* style: unused/shadowing/constant conditions/assignment in condition */
#pragma warning(disable: 4100 4127 4189 4456 4457 4459 4701 4702 4703 4706 4131 4115)
/* pointer qualifier/indirection differences between void * and typed pointers */
#pragma warning(disable: 4057 4090 4047 4024 4133)
/* misc: macro redefinition vs. ntdef.h min/max, padding, deprecated CRT */
#pragma warning(disable: 4005 4324 4996)
#endif

/* ---------------------------------------------------------------- types */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed char        s8;
typedef short              s16;
typedef int                s32;
typedef long long          s64;
typedef u8  __u8;
typedef u16 __u16;
typedef u32 __u32;
typedef u64 __u64;
typedef s32 __s32;
typedef u16 __le16;
typedef u32 __le32;
typedef u64 __le64;

#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le64_to_cpu(x) ((u64)(x))
#define cpu_to_le32(x) ((u32)(x))

#ifndef U8_MAX
#define U8_MAX  ((u8)~0U)
#endif
#ifndef U16_MAX
#define U16_MAX ((u16)~0U)
#endif
#ifndef U32_MAX
#define U32_MAX ((u32)~0U)
#endif
#ifndef S32_MAX
#define S32_MAX ((s32)(U32_MAX >> 1))
#endif
#define SZ_4K 0x00001000

/* ------------------------------------------------------- compiler stuff */
#if defined(_MSC_VER) && !defined(__clang__)
#define __maybe_unused
#define likely(x)   (x)
#define unlikely(x) (x)
#else
#define __maybe_unused __attribute__((unused))
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/*
 * __packed: MSVC has no per-declaration packing attribute. Packed IPC
 * structures in the ported headers are wrapped in
 * #pragma pack(push, 1) / #pragma pack(pop) instead, and every one of
 * them keeps its upstream static_assert(sizeof(...) == N), so any layout
 * difference fails the build instead of corrupting a payload.
 */
#define __packed
#define fallthrough do {} while (0)
/*
 * Upstream uses both static_assert(expr) and static_assert(expr, msg).
 * Implemented with the classic negative-array typedef so it works in every
 * MSVC C mode (legacy MSVC C knows no _Static_assert) and with or without
 * the conforming preprocessor. A failing assert shows up as
 * "negative subscript"/C2118 at the offending line.
 */
#undef static_assert
#define AVS_SA_CAT2(a, b) a##b
#define AVS_SA_CAT(a, b)  AVS_SA_CAT2(a, b)
#define static_assert(e, ...) \
	typedef char AVS_SA_CAT(avs_static_assert_, __COUNTER__)[(e) ? 1 : -1]
#define BUILD_BUG_ON(cond) ((void)sizeof(char[1 - 2 * !!(cond)]))
#define __counted_by(x)
/* Flexible array inside a union: zero-length array (MSVC C4200 / GNU ext). */
#define DECLARE_FLEX_ARRAY(TYPE, NAME) TYPE NAME[0]
#define __DECLARE_FLEX_ARRAY(TYPE, NAME) TYPE NAME[0]

#define BIT(n)      (1UL << (n))
#define BIT_ULL(n)  (1ULL << (n))
#define GENMASK(h, l) (((~0U) - (1U << (l)) + 1) & (~0U >> (31 - (h))))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

/* No GNU statement expressions: plain conditional forms (args evaluated twice). */
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define max_t(type, a, b) ((type)(a) > (type)(b) ? (type)(a) : (type)(b))
#undef min
#undef max
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

/* struct_size(p, member, n): sizeof(*p) + n * sizeof(p->member[0]) */
#define struct_size(p, member, n) \
	(sizeof(*(p)) + (size_t)(n) * sizeof((p)->member[0]))

/* --------------------------------------------------------------- errors */
#define EPERM    1
#define ENOENT   2
#define EIO      5
#define E2BIG    7
#define ENOMEM  12
#define EBUSY   16
#define EEXIST  17
#define ENODEV  19
#define EINVAL  22
#define ENOSPC  28
#define ERANGE  34
#define ENODATA 61
#define EPROTO  71
#define EOVERFLOW 75
#define EREMOTEIO 121
#define ETIMEDOUT 110
#define ENOTSUPP 524
#define ETOOSMALL 525
#define MAX_ERRNO 4095

#define IS_ERR_VALUE(x) ((unsigned long long)(void *)(x) >= (unsigned long long)-MAX_ERRNO)
static inline void *ERR_PTR(long long error) { return (void *)(intptr_t)error; }
static inline long long PTR_ERR(const void *ptr) { return (long long)(intptr_t)ptr; }
static inline bool IS_ERR(const void *ptr) { return IS_ERR_VALUE((uintptr_t)ptr); }
#define ERR_CAST(ptr) ((void *)(ptr))
static inline bool IS_ERR_OR_NULL(const void *ptr) { return !ptr || IS_ERR(ptr); }

/* -------------------------------------------------------------- logging */
struct device { const char *name; };

void avs_port_log(int level, const char *fmt, ...);
#define AVS_LOG_ERR  0
#define AVS_LOG_WARN 1
#define AVS_LOG_INFO 2
#define AVS_LOG_DBG  3

#define dev_err(dev, ...)   avs_port_log(AVS_LOG_ERR, __VA_ARGS__)
#define dev_warn(dev, ...)  avs_port_log(AVS_LOG_WARN, __VA_ARGS__)
#define dev_info(dev, ...)  avs_port_log(AVS_LOG_INFO, __VA_ARGS__)
#define dev_dbg(dev, ...)   avs_port_log(AVS_LOG_DBG, __VA_ARGS__)
#define dev_info_once dev_info
#define dev_err_ratelimited dev_err
#define pr_err(...)  avs_port_log(AVS_LOG_ERR, __VA_ARGS__)
#define pr_warn(...) avs_port_log(AVS_LOG_WARN, __VA_ARGS__)
#define pr_info(...) avs_port_log(AVS_LOG_INFO, __VA_ARGS__)
#define pr_debug(...) avs_port_log(AVS_LOG_DBG, __VA_ARGS__)
#define WARN_ON(cond) (!!(cond))
#define WARN_ON_ONCE(cond) (!!(cond))

/* ----------------------------------------------------------- allocation */
#define GFP_KERNEL 0
void *avs_port_zalloc(size_t size);
void  avs_port_free(void *p);
void *avs_port_realloc(void *p, size_t new_size); /* contents preserved up to old size */

#define kzalloc(size, gfp)        avs_port_zalloc(size)
#define kmalloc(size, gfp)        avs_port_zalloc(size)
#define kcalloc(n, size, gfp)     avs_port_zalloc((size_t)(n) * (size_t)(size))
#define kfree(p)                  avs_port_free((void *)(p))
#define kfree_const(p)            avs_port_free((void *)(p))
#define krealloc(p, size, gfp)    avs_port_realloc(p, size)
/* newer allocation helpers used upstream (kzalloc_obj / kzalloc_objs) */
#define kzalloc_obj(obj)          avs_port_zalloc(sizeof(obj))
#define kzalloc_objs(obj, n)      avs_port_zalloc(sizeof(obj) * (size_t)(n))
/*
 * devm_*: device-managed memory. Lifetime is the whole topology object in
 * our usage (freed when the topology is unloaded / driver unloads), so
 * plain allocations are equivalent; the topology loader frees through
 * avs_port_devm_release_all().
 */
void *avs_port_devm_zalloc(size_t size);
void  avs_port_devm_release_all(void);
#define devm_kzalloc(dev, size, gfp)     avs_port_devm_zalloc(size)
#define devm_kcalloc(dev, n, size, gfp)  avs_port_devm_zalloc((size_t)(n) * (size_t)(size))
static inline void *avs_port_devm_memdup(const void *src, size_t len)
{
	void *p = avs_port_devm_zalloc(len);

	if (p)
		memcpy(p, src, len);
	return p;
}
#define devm_kmemdup(dev, src, len, gfp) avs_port_devm_memdup(src, len)
static inline void *avs_port_kmemdup(const void *src, size_t len)
{
	void *p = avs_port_zalloc(len);

	if (p)
		memcpy(p, src, len);
	return p;
}
#define kmemdup(src, len, gfp) avs_port_kmemdup(src, len)

/* -------------------------------------------------------------- strings */
static inline size_t avs_port_strnlen(const char *s, size_t maxlen)
{
	size_t n = 0;

	while (n < maxlen && s[n])
		n++;
	return n;
}
#undef strnlen
#define strnlen avs_port_strnlen

int avs_port_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
static inline int avs_port_snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = avs_port_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return ret;
}
/* scnprintf: returns number of chars actually written (excl. NUL) */
static inline int avs_port_scnprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int ret;

	if (!size)
		return 0;
	va_start(ap, fmt);
	ret = avs_port_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	if (ret < 0)
		return 0;
	return (size_t)ret >= size ? (int)(size - 1) : ret;
}
#define snprintf  avs_port_snprintf
#define scnprintf avs_port_scnprintf

static inline bool sysfs_streq(const char *s1, const char *s2)
{
	while (*s1 && *s1 == *s2) {
		s1++;
		s2++;
	}
	if (*s1 == *s2)
		return true;
	if (!*s1 && *s2 == '\n' && !s2[1])
		return true;
	if (*s1 == '\n' && !s1[1] && !*s2)
		return true;
	return false;
}

/* ----------------------------------------------------------------- guid */
typedef struct {
	u8 b[16];
} guid_t;

/* Brace initializer: used only to initialize guid_t objects. */
#define GUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)			\
	{{ (u8)((a) & 0xff), (u8)(((a) >> 8) & 0xff), (u8)(((a) >> 16) & 0xff), \
	   (u8)(((a) >> 24) & 0xff), (u8)((b) & 0xff), (u8)(((b) >> 8) & 0xff),	\
	   (u8)((c) & 0xff), (u8)(((c) >> 8) & 0xff),				\
	   (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) }}

static inline bool guid_equal(const guid_t *u1, const guid_t *u2)
{
	return memcmp(u1, u2, sizeof(guid_t)) == 0;
}

static inline void guid_copy(guid_t *dst, const guid_t *src)
{
	memcpy(dst, src, sizeof(guid_t));
}

/* --------------------------------------------------------------- lists */
struct list_head {
	struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline void __list_add(struct list_head *entry, struct list_head *prev,
			      struct list_head *next)
{
	next->prev = entry;
	entry->next = next;
	entry->prev = prev;
	prev->next = entry;
}

static inline void list_add(struct list_head *entry, struct list_head *head)
{
	__list_add(entry, head, head->next);
}

static inline void list_add_tail(struct list_head *entry, struct list_head *head)
{
	__list_add(entry, head->prev, head);
}

static inline void list_del(struct list_head *entry)
{
	entry->next->prev = entry->prev;
	entry->prev->next = entry->next;
	entry->next = entry;
	entry->prev = entry;
}

static inline int list_empty(const struct list_head *head)
{
	return head->next == head;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)
#define list_last_entry(ptr, type, member) list_entry((ptr)->prev, type, member)
/*
 * typeof-free variants (MSVC C has no typeof before /std:clatest): the
 * containing object is computed as void * and converted by the assignment
 * to pos. avs_member_offset() is offsetof() taken through pos's own type:
 * "(1 ? 0 : (pos))" is a null pointer of pos's type and pos is NOT
 * evaluated (same trick MSVC's own offsetof is built on).
 */
#define avs_member_offset(pos, member) \
	((size_t)(char *)&((1 ? 0 : (pos))->member))
#define avs_entry_of(ptr, pos, member) \
	((void *)((char *)(ptr) - avs_member_offset(pos, member)))

#define list_next_entry(pos, member) avs_entry_of((pos)->member.next, pos, member)
#define list_prev_entry(pos, member) avs_entry_of((pos)->member.prev, pos, member)
#define list_entry_is_head(pos, head, member) (&(pos)->member == (head))

#define list_for_each_entry(pos, head, member)				\
	for (pos = avs_entry_of((head)->next, pos, member);		\
	     !list_entry_is_head(pos, head, member);			\
	     pos = list_next_entry(pos, member))

#define list_for_each_entry_reverse(pos, head, member)			\
	for (pos = avs_entry_of((head)->prev, pos, member);		\
	     !list_entry_is_head(pos, head, member);			\
	     pos = list_prev_entry(pos, member))

#define list_for_each_entry_safe(pos, n, head, member)			\
	for (pos = avs_entry_of((head)->next, pos, member),		\
		n = list_next_entry(pos, member);			\
	     !list_entry_is_head(pos, head, member);			\
	     pos = n, n = list_next_entry(n, member))

/* ------------------------------------------------------------- locking */
/* No-ops: the glue layer holds one coarse lock around every core call. */
struct mutex { int unused; };
typedef struct { int unused; } spinlock_t;
#define mutex_init(m)      ((void)(m))
#define mutex_lock(m)      ((void)(m))
#define mutex_unlock(m)    ((void)(m))
#define spin_lock_init(l)  ((void)(l))
#define spin_lock(l)       ((void)(l))
#define spin_unlock(l)     ((void)(l))
/* guard(mutex)(&x); -> (void)(&x); */
#define guard(type) (void)
#define scoped_guard(type, lock) if ((void)(lock), 1)

/* ----------------------------------------------------------------- ida */
/* Tiny bitmap-based replacement for the kernel IDA (ids 0..255). */
struct ida {
	u32 bits[8];
};
static inline void ida_init(struct ida *ida) { memset(ida, 0, sizeof(*ida)); }
static inline void ida_destroy(struct ida *ida) { memset(ida, 0, sizeof(*ida)); }
static inline bool ida_is_empty(const struct ida *ida)
{
	int i;

	for (i = 0; i < 8; i++)
		if (ida->bits[i])
			return false;
	return true;
}
static inline int ida_alloc_max(struct ida *ida, unsigned int max, int gfp)
{
	unsigned int i;

	(void)gfp;
	for (i = 0; i <= max && i < 256; i++) {
		if (!(ida->bits[i / 32] & (1U << (i % 32)))) {
			ida->bits[i / 32] |= 1U << (i % 32);
			return (int)i;
		}
	}
	return -ENOSPC;
}
static inline void ida_free(struct ida *ida, unsigned int id)
{
	if (id < 256)
		ida->bits[id / 32] &= ~(1U << (id % 32));
}

#endif /* AVS_LINUX_COMPAT_H */
