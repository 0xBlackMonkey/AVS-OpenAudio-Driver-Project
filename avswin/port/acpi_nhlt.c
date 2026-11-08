// SPDX-License-Identifier: GPL-2.0-only
//
// acpi_nhlt.c -- port of the lookup half of drivers/acpi/nhlt.c
// (Copyright(c) 2023-2024 Intel Corporation, Cezary Rojewski,
// Amadeusz Slawinski). Matching logic is unchanged; the only difference is
// where the table comes from and that every step is bounds-checked against
// the blob size, since on Windows the blob arrives from another driver.

#include "acpi_nhlt.h"

static const struct acpi_table_nhlt *avs_nhlt_tb;
static size_t avs_nhlt_size;

void acpi_nhlt_set_table(const void *table, size_t size)
{
	avs_nhlt_tb = (const struct acpi_table_nhlt *)table;
	avs_nhlt_size = size;
}

const struct acpi_table_nhlt *acpi_nhlt_get_table(void)
{
	return avs_nhlt_tb;
}

static bool avs_nhlt_in_bounds(const void *p, size_t len)
{
	const u8 *base = (const u8 *)avs_nhlt_tb;
	const u8 *q = (const u8 *)p;

	return base && q >= base && q + len <= base + avs_nhlt_size;
}

bool acpi_nhlt_endpoint_match(const struct acpi_nhlt_endpoint *ep,
			      int link_type, int dev_type, int dir, int bus_id)
{
	return ep &&
	       (link_type < 0 || ep->link_type == link_type) &&
	       (dev_type < 0 || ep->device_type == dev_type) &&
	       (bus_id < 0 || ep->virtual_bus_id == bus_id) &&
	       (dir < 0 || ep->direction == dir);
}

/* for_each_nhlt_endpoint(), with bounds checks */
#define avs_for_each_nhlt_endpoint(tb, ep, i)					\
	for (i = 0, ep = (struct acpi_nhlt_endpoint *)((tb) + 1);		\
	     i < (tb)->endpoints_count && avs_nhlt_in_bounds(ep, sizeof(*ep)) &&	\
		avs_nhlt_in_bounds(ep, ep->length);				\
	     i++, ep = (struct acpi_nhlt_endpoint *)((u8 *)ep + ep->length))

struct acpi_nhlt_endpoint *
acpi_nhlt_find_endpoint(int link_type, int dev_type, int dir, int bus_id)
{
	struct acpi_nhlt_endpoint *ep;
	unsigned int i;

	if (!avs_nhlt_tb)
		return NULL;

	avs_for_each_nhlt_endpoint(avs_nhlt_tb, ep, i)
		if (acpi_nhlt_endpoint_match(ep, link_type, dev_type, dir, bus_id))
			return ep;
	return NULL;
}

struct acpi_nhlt_format_config *
acpi_nhlt_endpoint_find_fmtcfg(const struct acpi_nhlt_endpoint *ep,
			       u16 ch, u32 rate, u16 vbps, u16 bps)
{
	struct acpi_nhlt_formats_config *fmts;
	struct acpi_nhlt_format_config *fmt;
	struct acpi_nhlt_wave_formatext *wav;
	unsigned int i;

	fmts = acpi_nhlt_endpoint_fmtscfg(ep);
	if (!avs_nhlt_in_bounds(fmts, sizeof(*fmts)))
		return NULL;

	fmt = (struct acpi_nhlt_format_config *)(fmts + 1);
	for (i = 0; i < fmts->formats_count; i++) {
		if (!avs_nhlt_in_bounds(fmt, sizeof(*fmt)) ||
		    !avs_nhlt_in_bounds(fmt, sizeof(*fmt) + fmt->config.capabilities_size))
			return NULL;

		wav = &fmt->format;
		if (wav->valid_bits_per_sample == vbps &&
		    wav->samples_per_sec == rate &&
		    wav->bits_per_sample == bps &&
		    wav->channel_count == ch)
			return fmt;

		fmt = (struct acpi_nhlt_format_config *)
			((u8 *)(fmt + 1) + fmt->config.capabilities_size);
	}

	return NULL;
}

struct acpi_nhlt_format_config *
acpi_nhlt_find_fmtcfg(int link_type, int dev_type, int dir, int bus_id,
		      u16 ch, u32 rate, u16 vbps, u16 bps)
{
	struct acpi_nhlt_format_config *fmt;
	struct acpi_nhlt_endpoint *ep;
	unsigned int i;

	if (!avs_nhlt_tb)
		return NULL;

	avs_for_each_nhlt_endpoint(avs_nhlt_tb, ep, i) {
		if (!acpi_nhlt_endpoint_match(ep, link_type, dev_type, dir, bus_id))
			continue;

		fmt = acpi_nhlt_endpoint_find_fmtcfg(ep, ch, rate, vbps, bps);
		if (fmt)
			return fmt;
	}

	return NULL;
}
