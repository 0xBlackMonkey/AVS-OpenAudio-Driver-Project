// SPDX-License-Identifier: GPL-2.0-only
//
// avs_port_dsp.c -- firmware-resource management for the ported AVS core.
//
// Module/instance-id bookkeeping is taken verbatim from upstream
// sound/soc/intel/avs/utils.c, and module/pipeline create/delete from
// sound/soc/intel/avs/dsp.c (Copyright(c) 2021-2022 Intel Corporation,
// Cezary Rojewski, Amadeusz Slawinski).
//
// AVS-PORT differences, all Skylake-specific simplifications:
//   - one DSP core (core_init_mask = 1), so avs_dsp_get_core()/put_core()
//     only validate core_id == 0 instead of powering secondary cores;
//   - loadable (non-built-in) modules are reported and refused: Skylake
//     base firmware 9.21 ships COPIER/PEAKVOL/MIXIN/... built in. Loading
//     via CLDMA (avs_cldma_transfer_modules) can be added if ever needed.

#include "avs.h"
#include "messages.h"

/* -------------------------------------------------- from utils.c (verbatim) */

/* Caller responsible for holding adev->modres_mutex. */
static int avs_module_entry_index(struct avs_dev *adev, const guid_t *uuid)
{
	int i;

	for (i = 0; i < adev->mods_info->count; i++) {
		struct avs_module_entry *module;

		module = &adev->mods_info->entries[i];
		if (guid_equal(&module->uuid, uuid))
			return i;
	}

	return -ENOENT;
}

/* Caller responsible for holding adev->modres_mutex. */
static int avs_module_id_entry_index(struct avs_dev *adev, u32 module_id)
{
	int i;

	for (i = 0; i < adev->mods_info->count; i++) {
		struct avs_module_entry *module;

		module = &adev->mods_info->entries[i];
		if (module->module_id == module_id)
			return i;
	}

	return -ENOENT;
}

int avs_get_module_entry(struct avs_dev *adev, const guid_t *uuid, struct avs_module_entry *entry)
{
	int idx;

	guard(mutex)(&adev->modres_mutex);

	idx = avs_module_entry_index(adev, uuid);
	if (idx >= 0)
		memcpy(entry, &adev->mods_info->entries[idx], sizeof(*entry));

	return (idx < 0) ? idx : 0;
}

int avs_get_module_id_entry(struct avs_dev *adev, u32 module_id, struct avs_module_entry *entry)
{
	int idx;

	guard(mutex)(&adev->modres_mutex);

	idx = avs_module_id_entry_index(adev, module_id);
	if (idx >= 0)
		memcpy(entry, &adev->mods_info->entries[idx], sizeof(*entry));

	return (idx < 0) ? idx : 0;
}

int avs_get_module_id(struct avs_dev *adev, const guid_t *uuid)
{
	struct avs_module_entry module;
	int ret;

	ret = avs_get_module_entry(adev, uuid, &module);
	return !ret ? module.module_id : -ENOENT;
}

bool avs_is_module_ida_empty(struct avs_dev *adev, u32 module_id)
{
	bool ret = false;
	int idx;

	guard(mutex)(&adev->modres_mutex);

	idx = avs_module_id_entry_index(adev, module_id);
	if (idx >= 0)
		ret = ida_is_empty(adev->mod_idas[idx]);

	return ret;
}

/* Caller responsible for holding adev->modres_mutex. */
static void avs_module_ida_destroy(struct avs_dev *adev)
{
	int i = adev->mods_info ? adev->mods_info->count : 0;

	while (i--) {
		ida_destroy(adev->mod_idas[i]);
		kfree(adev->mod_idas[i]);
	}
	kfree(adev->mod_idas);
}

/* Caller responsible for holding adev->modres_mutex. */
static int
avs_module_ida_alloc(struct avs_dev *adev, struct avs_mods_info *newinfo, bool purge)
{
	struct avs_mods_info *oldinfo = adev->mods_info;
	struct ida **ida_ptrs;
	u32 tocopy_count = 0;
	int i;

	if (!purge && oldinfo) {
		if (oldinfo->count >= newinfo->count) {
			dev_warn(adev->dev, "modules info underflow, old: %d, new: %d\n",
				 oldinfo->count, newinfo->count);
			return -EPROTO;
		}
		tocopy_count = oldinfo->count;
	}

	ida_ptrs = kzalloc_objs(*ida_ptrs, newinfo->count);
	if (!ida_ptrs)
		return -ENOMEM;

	if (tocopy_count)
		memcpy(ida_ptrs, adev->mod_idas, tocopy_count * sizeof(*ida_ptrs));

	for (i = tocopy_count; i < (int)newinfo->count; i++) {
		ida_ptrs[i] = kzalloc_obj(**ida_ptrs);
		if (!ida_ptrs[i]) {
			while (i--)
				kfree(ida_ptrs[i]);

			kfree(ida_ptrs);
			return -ENOMEM;
		}

		ida_init(ida_ptrs[i]);
	}

	/* If old elements have been reused, don't wipe them. */
	if (tocopy_count)
		kfree(adev->mod_idas);
	else
		avs_module_ida_destroy(adev);

	adev->mod_idas = ida_ptrs;
	return 0;
}

int avs_module_info_init(struct avs_dev *adev, bool purge)
{
	struct avs_mods_info *info;
	int ret;

	ret = avs_ipc_get_modules_info(adev, &info);
	if (ret)
		return AVS_IPC_RET(ret);

	guard(mutex)(&adev->modres_mutex);

	ret = avs_module_ida_alloc(adev, info, purge);
	if (ret < 0) {
		dev_err(adev->dev, "initialize module idas failed: %d\n", ret);
		return ret;
	}

	/* Refresh current information with newly received table. */
	kfree(adev->mods_info);
	adev->mods_info = info;

	return ret;
}

void avs_module_info_free(struct avs_dev *adev)
{
	guard(mutex)(&adev->modres_mutex);

	avs_module_ida_destroy(adev);
	kfree(adev->mods_info);
	adev->mods_info = NULL;
}

int avs_module_id_alloc(struct avs_dev *adev, u16 module_id)
{
	int idx, max_id;

	guard(mutex)(&adev->modres_mutex);

	idx = avs_module_id_entry_index(adev, module_id);
	if (idx == -ENOENT) {
		dev_err(adev->dev, "invalid module id: %d", module_id);
		return -EINVAL;
	}
	max_id = adev->mods_info->entries[idx].instance_max_count - 1;

	return ida_alloc_max(adev->mod_idas[idx], max_id, GFP_KERNEL);
}

void avs_module_id_free(struct avs_dev *adev, u16 module_id, u8 instance_id)
{
	int idx;

	guard(mutex)(&adev->modres_mutex);

	idx = avs_module_id_entry_index(adev, module_id);
	if (idx == -ENOENT)
		dev_err(adev->dev, "invalid module id: %d", module_id);
	else
		ida_free(adev->mod_idas[idx], instance_id);
}

/* ------------------------------------------------------------ from dsp.c */

/* AVS-PORT: Skylake has a single DSP core, powered by the boot sequence. */
static int avs_dsp_get_core(struct avs_dev *adev, u32 core_id)
{
	if (core_id != 0) {
		dev_err(adev->dev, "get core %d failed: only core 0 exists\n", core_id);
		return -EINVAL;
	}
	return 0;
}

static int avs_dsp_put_core(struct avs_dev *adev, u32 core_id)
{
	(void)adev;
	(void)core_id;
	return 0;
}

/* AVS-PORT: upstream calls avs_dsp_op(adev, transfer_mods, ...). */
static int avs_port_transfer_mods(struct avs_dev *adev, bool load,
				  struct avs_module_entry *mentry)
{
	dev_err(adev->dev, "%s of loadable module %d (%.8s) not supported by this port\n",
		load ? "load" : "unload", mentry->module_id, mentry->name);
	return -ENOTSUPP;
}

int avs_dsp_init_module(struct avs_dev *adev, u16 module_id, u8 ppl_instance_id,
			u8 core_id, u8 domain, void *param, u32 param_size,
			u8 *instance_id)
{
	struct avs_module_entry mentry;
	bool was_loaded = false;
	int ret, id;

	id = avs_module_id_alloc(adev, module_id);
	if (id < 0)
		return id;

	ret = avs_get_module_id_entry(adev, module_id, &mentry);
	if (ret)
		goto err_mod_entry;

	ret = avs_dsp_get_core(adev, core_id);
	if (ret)
		goto err_mod_entry;

	/* Load code into memory if this is the first instance. */
	if (!id && !avs_module_entry_is_loaded(&mentry)) {
		ret = avs_port_transfer_mods(adev, true, &mentry);
		if (ret) {
			dev_err(adev->dev, "load modules failed: %d\n", ret);
			goto err_mod_entry;
		}
		was_loaded = true;
	}

	ret = avs_ipc_init_instance(adev, module_id, id, ppl_instance_id,
				    core_id, domain, param, param_size);
	if (ret) {
		ret = AVS_IPC_RET(ret);
		goto err_ipc;
	}

	*instance_id = id;
	return 0;

err_ipc:
	if (was_loaded)
		avs_port_transfer_mods(adev, false, &mentry);
	avs_dsp_put_core(adev, core_id);
err_mod_entry:
	avs_module_id_free(adev, module_id, id);
	return ret;
}

void avs_dsp_delete_module(struct avs_dev *adev, u16 module_id, u8 instance_id,
			   u8 ppl_instance_id, u8 core_id)
{
	struct avs_module_entry mentry;
	int ret;

	/* Modules not owned by any pipeline need to be freed explicitly. */
	if (ppl_instance_id == INVALID_PIPELINE_ID)
		avs_ipc_delete_instance(adev, module_id, instance_id);

	avs_module_id_free(adev, module_id, instance_id);

	ret = avs_get_module_id_entry(adev, module_id, &mentry);
	/* Unload occupied memory if this was the last instance. */
	if (!ret && mentry.type.load_type == AVS_MODULE_LOAD_TYPE_LOADABLE) {
		if (avs_is_module_ida_empty(adev, module_id)) {
			ret = avs_port_transfer_mods(adev, false, &mentry);
			if (ret)
				dev_err(adev->dev, "unload modules failed: %d\n", ret);
		}
	}

	avs_dsp_put_core(adev, core_id);
}

int avs_dsp_create_pipeline(struct avs_dev *adev, u16 req_size, u8 priority,
			    bool lp, u16 attributes, u8 *instance_id)
{
	struct avs_fw_cfg *fw_cfg = &adev->fw_cfg;
	int ret, id;

	id = ida_alloc_max(&adev->ppl_ida, fw_cfg->max_ppl_count - 1, GFP_KERNEL);
	if (id < 0)
		return id;

	ret = avs_ipc_create_pipeline(adev, req_size, priority, id, lp, attributes);
	if (ret) {
		ida_free(&adev->ppl_ida, id);
		return AVS_IPC_RET(ret);
	}

	*instance_id = id;
	return 0;
}

int avs_dsp_delete_pipeline(struct avs_dev *adev, u8 instance_id)
{
	int ret;

	ret = avs_ipc_delete_pipeline(adev, instance_id);
	if (ret)
		ret = AVS_IPC_RET(ret);

	ida_free(&adev->ppl_ida, instance_id);
	return ret;
}

/* ALSA FE DAI ops are never registered on Windows; topology.c only takes
 * the address. */
const struct snd_soc_dai_ops avs_dai_fe_ops = { 0 };
