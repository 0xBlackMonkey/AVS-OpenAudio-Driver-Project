// SPDX-License-Identifier: GPL-2.0-only
//
// avs_port_rt_km.c -- Windows kernel runtime for linux_compat.h.
// A user-mode build of port/ (e.g. an offline test harness) supplies its own
// implementation of the same functions.
//
// All allocations are NonPagedPoolNx (ExAllocatePool2 zero-fills). The core
// is only ever called at PASSIVE_LEVEL and serialized by the glue, so the
// devm list needs no lock.

#include "linux_compat.h"
#include <ntstrsafe.h>

#define AVS_PORT_POOL_TAG	'pvAA'
#define AVS_PORT_LOG_MAX	512

struct avs_alloc_hdr {
	size_t size;
	struct avs_alloc_hdr *devm_next;	/* 16 bytes: keeps 16-byte alignment */
};

static struct avs_alloc_hdr *avs_devm_head;

void *avs_port_zalloc(size_t size)
{
	struct avs_alloc_hdr *h;

	h = (struct avs_alloc_hdr *)ExAllocatePool2(POOL_FLAG_NON_PAGED,
						    sizeof(*h) + size, AVS_PORT_POOL_TAG);
	if (!h)
		return NULL;
	h->size = size;
	h->devm_next = NULL;
	return h + 1;
}

void avs_port_free(void *p)
{
	if (p)
		ExFreePoolWithTag((struct avs_alloc_hdr *)p - 1, AVS_PORT_POOL_TAG);
}

void *avs_port_realloc(void *p, size_t new_size)
{
	struct avs_alloc_hdr *old;
	void *n;

	if (!p)
		return avs_port_zalloc(new_size);
	old = (struct avs_alloc_hdr *)p - 1;
	n = avs_port_zalloc(new_size);
	if (!n)
		return NULL;
	memcpy(n, p, old->size < new_size ? old->size : new_size);
	avs_port_free(p);
	return n;
}

void *avs_port_devm_zalloc(size_t size)
{
	struct avs_alloc_hdr *h;
	void *p = avs_port_zalloc(size);

	if (!p)
		return NULL;
	h = (struct avs_alloc_hdr *)p - 1;
	h->devm_next = avs_devm_head;
	avs_devm_head = h;
	return p;
}

void avs_port_devm_release_all(void)
{
	while (avs_devm_head) {
		struct avs_alloc_hdr *h = avs_devm_head;

		avs_devm_head = h->devm_next;
		ExFreePoolWithTag(h, AVS_PORT_POOL_TAG);
	}
}

/*
 * Linux "%zu"/"%zx" -> MS "%Iu"/"%Ix" (pointer-sized), everything else is
 * passed through; the core only uses standard conversions otherwise.
 */
static void avs_port_fix_format(const char *in, char *out, size_t out_size)
{
	size_t o = 0;

	while (*in && o + 1 < out_size) {
		char c = *in++;

		out[o++] = c;
		if (c != '%')
			continue;
		/* copy flags/width/precision */
		while (*in && o + 1 < out_size && strchr("-+ #0123456789.*", *in))
			out[o++] = *in++;
		if (*in == 'z' && o + 1 < out_size) {
			out[o++] = 'I';
			in++;
		}
	}
	out[o] = '\0';
}

int avs_port_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	char fixed[256];
	NTSTATUS status;

	if (!size)
		return 0;
	avs_port_fix_format(fmt, fixed, sizeof(fixed));
	status = RtlStringCbVPrintfA(buf, size, fixed, ap);
	if (!NT_SUCCESS(status) && status != STATUS_BUFFER_OVERFLOW) {
		buf[0] = '\0';
		return -1;
	}
	return (int)strlen(buf);
}

int avs_port_log_level = AVS_LOG_INFO;

void avs_port_log(int level, const char *fmt, ...)
{
	char msg[AVS_PORT_LOG_MAX];
	va_list ap;
	size_t n;

	if (level > avs_port_log_level)
		return;

	va_start(ap, fmt);
	avs_port_vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	n = strlen(msg);
	DbgPrintEx(DPFLTR_IHVDRIVER_ID,
		   level == AVS_LOG_ERR ? DPFLTR_ERROR_LEVEL :
		   level == AVS_LOG_WARN ? DPFLTR_WARNING_LEVEL : DPFLTR_INFO_LEVEL,
		   (n && msg[n - 1] == '\n') ? "AVS-core: %s" : "AVS-core: %s\n", msg);
}
