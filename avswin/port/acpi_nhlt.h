/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * acpi_nhlt.h -- port of the NHLT pieces the AVS core needs:
 *   - table layout from include/acpi/actbl2.h (ACPICA, byte-packed)
 *   - iteration helpers from include/acpi/nhlt.h
 *   - endpoint/format lookup from drivers/acpi/nhlt.c
 *
 * Upstream reads the table through ACPI (acpi_gbl_nhlt). Here the glue hands
 * us the blob it got from sklhdaudbus (GetResources) or from a file (offline
 * harness) via acpi_nhlt_set_table().
 */
#ifndef AVS_PORT_ACPI_NHLT_H
#define AVS_PORT_ACPI_NHLT_H

#include "linux_compat.h"

#pragma pack(push, 1)

struct acpi_table_header {
	char signature[4];
	u32 length;
	u8 revision;
	u8 checksum;
	char oem_id[6];
	char oem_table_id[8];
	u32 oem_revision;
	char asl_compiler_id[4];
	u32 asl_compiler_revision;
};
static_assert(sizeof(struct acpi_table_header) == 36, "acpi_table_header");

struct acpi_table_nhlt {
	struct acpi_table_header header;
	u8 endpoints_count;
};

struct acpi_nhlt_endpoint {
	u32 length;
	u8 link_type;
	u8 instance_id;
	u16 vendor_id;
	u16 device_id;
	u16 revision_id;
	u32 subsystem_id;
	u8 device_type;
	u8 direction;
	u8 virtual_bus_id;
};
static_assert(sizeof(struct acpi_nhlt_endpoint) == 19, "acpi_nhlt_endpoint");

#define ACPI_NHLT_LINKTYPE_HDA		0
#define ACPI_NHLT_LINKTYPE_DSP		1
#define ACPI_NHLT_LINKTYPE_PDM		2
#define ACPI_NHLT_LINKTYPE_SSP		3

#define ACPI_NHLT_DEVICETYPE_PDM	0
#define ACPI_NHLT_DEVICETYPE_PDM_SKL	1
#define ACPI_NHLT_DEVICETYPE_BT		0
#define ACPI_NHLT_DEVICETYPE_FM		1
#define ACPI_NHLT_DEVICETYPE_MODEM	2
#define ACPI_NHLT_DEVICETYPE_CODEC	4

#define ACPI_NHLT_DIR_RENDER		0
#define ACPI_NHLT_DIR_CAPTURE		1

struct acpi_nhlt_config {
	u32 capabilities_size;
	u8 capabilities[0];	/* AVS-PORT: [0], nested in acpi_nhlt_format_config */
};

struct acpi_nhlt_wave_formatext {
	u16 format_tag;
	u16 channel_count;
	u32 samples_per_sec;
	u32 avg_bytes_per_sec;
	u16 block_align;
	u16 bits_per_sample;
	u16 extra_format_size;
	u16 valid_bits_per_sample;
	u32 channel_mask;
	u8 subformat[16];
};
static_assert(sizeof(struct acpi_nhlt_wave_formatext) == 40, "wave_formatext");

struct acpi_nhlt_format_config {
	struct acpi_nhlt_wave_formatext format;
	struct acpi_nhlt_config config;
};

struct acpi_nhlt_formats_config {
	u8 formats_count;
	/* struct acpi_nhlt_format_config formats[]; (variable-size entries) */
};

#pragma pack(pop)

/* --- iteration helpers (include/acpi/nhlt.h) --- */
#define __acpi_nhlt_endpoint_config(ep)	((void *)((ep) + 1))
#define __acpi_nhlt_config_caps(cfg)	((void *)((cfg) + 1))

static inline struct acpi_nhlt_formats_config *
acpi_nhlt_endpoint_fmtscfg(const struct acpi_nhlt_endpoint *ep)
{
	struct acpi_nhlt_config *cfg = (struct acpi_nhlt_config *)__acpi_nhlt_endpoint_config(ep);

	return (struct acpi_nhlt_formats_config *)((u8 *)(cfg + 1) + cfg->capabilities_size);
}

/* --- lookup (drivers/acpi/nhlt.c) --- */
void acpi_nhlt_set_table(const void *table, size_t size);
const struct acpi_table_nhlt *acpi_nhlt_get_table(void);

bool acpi_nhlt_endpoint_match(const struct acpi_nhlt_endpoint *ep,
			      int link_type, int dev_type, int dir, int bus_id);
struct acpi_nhlt_endpoint *
acpi_nhlt_find_endpoint(int link_type, int dev_type, int dir, int bus_id);
struct acpi_nhlt_format_config *
acpi_nhlt_endpoint_find_fmtcfg(const struct acpi_nhlt_endpoint *ep,
			       u16 ch, u32 rate, u16 vbps, u16 bps);
struct acpi_nhlt_format_config *
acpi_nhlt_find_fmtcfg(int link_type, int dev_type, int dir, int bus_id,
		      u16 ch, u32 rate, u16 vbps, u16 bps);

#endif
