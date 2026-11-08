/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * avs_tplg_loader.h -- replacement for the part of sound/soc/soc-topology.c
 * that the AVS driver depends on: walk a binary topology (.bin), hand the
 * MANIFEST block and every DAPM widget (with its mixer kcontrols) to the
 * unmodified AVS callbacks in topology.c (avs_tplg_ops), and keep the
 * resulting path templates addressable by widget name.
 */
#ifndef AVS_PORT_TPLG_LOADER_H
#define AVS_PORT_TPLG_LOADER_H

#include "avs.h"
#include "topology.h"

struct avs_port_tplg;

/*
 * @i2s_link_mask: which SSP port(s) the board wires this codec to, exactly
 *                 like snd_soc_acpi_mach.mach_params.i2s_link_mask upstream
 *                 (BIT(0) = SSP0). Expands "ssp%d" names and assigns the
 *                 I2S copier gateway instance.
 */
int avs_port_tplg_load(struct avs_dev *adev, const void *data, size_t size,
		       u32 i2s_link_mask, struct avs_port_tplg **out);
void avs_port_tplg_unload(struct avs_dev *adev, struct avs_port_tplg *t);

struct avs_tplg *avs_port_tplg_get(struct avs_port_tplg *t);
struct avs_tplg_path_template *
avs_port_tplg_find_template(struct avs_port_tplg *t, const char *widget_name);

/* Sets every mixer control whose name contains @name_part (e.g. "Volume")
 * to @value on all channels. Takes effect when PEAKVOL is (re)created;
 * see avs_peakvol_create() in path.c. Returns number of controls set. */
int avs_port_tplg_set_controls(struct avs_port_tplg *t, const char *name_part, long value);

/* Same, as 0..100 % of each control's own [min, max] range. */
int avs_port_tplg_set_controls_pct(struct avs_port_tplg *t, const char *name_part,
				   unsigned int percent);

/* Human-readable dump of everything parsed (formats, modcfgs, pplcfgs,
 * path templates -> paths -> pipelines -> modules). */
void avs_port_tplg_dump(struct avs_port_tplg *t);

#endif
