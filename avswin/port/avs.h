/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * avs.h -- port of sound/soc/intel/avs/avs.h, reduced to what the ported
 * core (messages.c, topology.c, path.c, avs_port_dsp.c) uses.
 *
 * Upstream: Copyright(c) 2021-2022 Intel Corporation,
 *           Cezary Rojewski, Amadeusz Slawinski.
 *
 * What is gone and why:
 *   - hda_bus / hdac_ext: the HDA controller belongs to sklhdaudbus.sys on
 *     Windows; we only get BAR pointers through ADSP_BUS_INTERFACE.
 *   - avs_dsp_ops / avs_spec function tables: boot, CLDMA and the HIPC
 *     transport stay in the already hardware-proven Windows code; the core
 *     reaches them through avs_dsp_send_msg*() implemented by the glue.
 *   - debugfs, probes, trace fifo, firmware list, work items, d0ix.
 */
#ifndef __SOUND_SOC_INTEL_AVS_H
#define __SOUND_SOC_INTEL_AVS_H

#include "linux_compat.h"
#include "soc_compat.h"
#include "acpi_nhlt.h"
#include "messages.h"

struct avs_dev;
struct avs_tplg;
struct avs_tplg_library;
struct avs_ipc_msg;

/* Skylake: no S0ix-specific link handling in this port. */
#define AVS_S0IX_SUPPORTED false

#define AVS_PLATATTR_CLDMA		BIT_ULL(0)
#define AVS_PLATATTR_IMR		BIT_ULL(1)
#define AVS_PLATATTR_ACE		BIT_ULL(2)
#define AVS_PLATATTR_ALTHDA		BIT_ULL(3)

#define avs_platattr_test(adev, attr) \
	((adev)->spec->attributes & AVS_PLATATTR_##attr)

/* Platform specific descriptor (reduced). */
struct avs_spec {
	const char *name;
	const u32 core_init_mask;
	const u64 attributes;		/* bitmask of AVS_PLATATTR_* */
};

/*
 * struct avs_dev - Intel HD-Audio driver data (reduced)
 *
 * @dev:          logging handle
 * @spec:         platform descriptor (Skylake: CLDMA)
 * @fw_cfg:       FIRMWARE_CONFIG as reported by base firmware
 * @mods_info:    MODULES_INFO as reported by base firmware
 * @mod_idas:     per module-type instance-id allocators
 * @modcfg_buf:   scratch buffer for INIT_INSTANCE payloads (AVS_MAILBOX_SIZE)
 * @ppl_ida:      pipeline-id allocator
 * @comp_list:    loaded topologies (struct avs_soc_component)
 * @path_list:    instantiated paths
 * @port_ctx:     opaque pointer for the glue (Windows device context / harness)
 */
struct avs_dev {
	struct device *dev;
	const struct avs_spec *spec;

	struct avs_fw_cfg fw_cfg;
	struct avs_hw_cfg hw_cfg;
	struct avs_mods_info *mods_info;
	struct ida **mod_idas;
	struct mutex modres_mutex;
	void *modcfg_buf;
	struct ida ppl_ida;
	int *core_refs;

	struct list_head comp_list;
	struct mutex comp_list_mutex;
	struct list_head path_list;
	spinlock_t path_list_lock;
	struct mutex path_mutex;

	void *port_ctx;
};

/* Inter Process Communication */

struct avs_ipc_msg {
	union {
		u64 header;
		union avs_global_msg glb;
		union avs_reply_msg rsp;
	};
	void *data;
	size_t size;
};

#define AVS_EIPC	EREMOTEIO
/*
 * IPC handlers may return positive value (firmware error code) what denotes
 * successful HOST <-> DSP communication yet failure to process specific request.
 *
 * Below macro converts returned value to linux kernel error code.
 * All IPC callers MUST use it as soon as firmware error code is consumed.
 */
#define AVS_IPC_RET(ret) \
	(((ret) <= 0) ? (ret) : -AVS_EIPC)

/*
 * Transport, implemented by the glue (Windows: our HIPC code;
 * harness: logging mock). Same contracts as upstream ipc.c:
 *   - request->header/data/size describe what to send,
 *   - on success *reply (if non-NULL) gets header + payload; reply->data
 *     must be preallocated by the caller with reply->size bytes,
 *   - return: <0 transport error, 0 OK, >0 firmware status code.
 */
int avs_dsp_send_msg_timeout(struct avs_dev *adev, struct avs_ipc_msg *request,
			     struct avs_ipc_msg *reply, int timeout, const char *name);
int avs_dsp_send_msg(struct avs_dev *adev, struct avs_ipc_msg *request,
		     struct avs_ipc_msg *reply, const char *name);
int avs_dsp_send_pm_msg_timeout(struct avs_dev *adev, struct avs_ipc_msg *request,
				struct avs_ipc_msg *reply, int timeout, bool wake_d0i0,
				const char *name);
int avs_dsp_send_pm_msg(struct avs_dev *adev, struct avs_ipc_msg *request,
			struct avs_ipc_msg *reply, bool wake_d0i0, const char *name);
int avs_dsp_send_rom_msg_timeout(struct avs_dev *adev, struct avs_ipc_msg *request, int timeout,
				 const char *name);
int avs_dsp_send_rom_msg(struct avs_dev *adev, struct avs_ipc_msg *request, const char *name);

/* Firmware resources management (avs_port_dsp.c, from utils.c/dsp.c) */

int avs_get_module_entry(struct avs_dev *adev, const guid_t *uuid, struct avs_module_entry *entry);
int avs_get_module_id_entry(struct avs_dev *adev, u32 module_id, struct avs_module_entry *entry);
int avs_get_module_id(struct avs_dev *adev, const guid_t *uuid);
bool avs_is_module_ida_empty(struct avs_dev *adev, u32 module_id);

int avs_module_info_init(struct avs_dev *adev, bool purge);
void avs_module_info_free(struct avs_dev *adev);
int avs_module_id_alloc(struct avs_dev *adev, u16 module_id);
void avs_module_id_free(struct avs_dev *adev, u16 module_id, u8 instance_id);

int avs_dsp_init_module(struct avs_dev *adev, u16 module_id, u8 ppl_instance_id,
			u8 core_id, u8 domain, void *param, u32 param_size,
			u8 *instance_id);
void avs_dsp_delete_module(struct avs_dev *adev, u16 module_id, u8 instance_id,
			   u8 ppl_instance_id, u8 core_id);
int avs_dsp_create_pipeline(struct avs_dev *adev, u16 req_size, u8 priority,
			    bool lp, u16 attributes, u8 *instance_id);
int avs_dsp_delete_pipeline(struct avs_dev *adev, u8 instance_id);

/* Soc component members */

struct avs_soc_component {
	struct snd_soc_component *base;
	struct avs_tplg *tplg;

	struct list_head node;
};

/* Referenced by topology.c avs_dai_load(); FE DAI ops are ALSA-only. */
extern const struct snd_soc_dai_ops avs_dai_fe_ops;

#define to_avs_soc_component(comp) \
	((struct avs_soc_component *)snd_soc_component_to_priv(comp))

#endif /* __SOUND_SOC_INTEL_AVS_H */
