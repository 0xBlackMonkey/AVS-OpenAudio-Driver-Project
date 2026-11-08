// SPDX-License-Identifier: GPL-2.0-only
//
// avs_port_core.c -- Linux-side half of the bridge (see avs_port_bridge.h).
//
//  1. IPC adapter: avs_dsp_send_*() with the exact contract of upstream
//     sound/soc/intel/avs/ipc.c avs_dsp_do_send_msg()/avs_dsp_receive_rx(),
//     on top of avs_bridge_ipc_send() provided by the Windows side.
//  2. A small playback-stream API that drives the unmodified path.c the way
//     upstream pcm.c + ASoC DPCM do for FE<->BE pairs (several at once).

#include "avs.h"
#include "path.h"
#include "topology.h"
#include "avs_tplg_loader.h"
#include "avs_port_bridge.h"

#define AVS_PORT_MAX_TPLGS	4
#define AVS_PORT_MAX_STREAMS	8

struct avs_core_stream {
	struct avs_path *fe;
	struct avs_path *be;
	struct avs_tplg_path_template *be_tmpl;
	bool in_use;
};

struct avs_core {
	struct avs_dev adev;
	struct device dev;
	void *host;
	struct avs_port_tplg *tplgs[AVS_PORT_MAX_TPLGS];
	int num_tplgs;

	struct avs_core_stream streams[AVS_PORT_MAX_STREAMS];
};

static const struct avs_spec avs_port_skl_spec = {
	.name = "skl",
	.core_init_mask = 1,
	.attributes = AVS_PLATATTR_CLDMA,
};

/* ------------------------------------------------------------ IPC adapter */

#define AVS_IPC_TIMEOUT_MS	300	/* upstream: AVS_IPC_TIMEOUT_MS */

int avs_dsp_send_msg_timeout(struct avs_dev *adev, struct avs_ipc_msg *request,
			     struct avs_ipc_msg *reply, int timeout, const char *name)
{
	struct avs_core *core = container_of(adev, struct avs_core, adev);
	unsigned int rprimary = 0, rext = 0, rx_size = 0;
	union avs_reply_msg rsp;
	int ret;

	ret = avs_bridge_ipc_send(core->host, request->glb.primary, request->glb.ext.val,
				  request->data, (unsigned int)request->size,
				  reply ? (unsigned int)reply->size : 0,
				  reply ? reply->data : NULL, &rx_size,
				  &rprimary, &rext,
				  (unsigned int)(timeout > 0 ? timeout : AVS_IPC_TIMEOUT_MS));
	if (ret) {
		dev_err(adev->dev, "%s (0x%08x 0x%08x) transport failed: %d\n",
			name, request->glb.primary, request->glb.ext.val, ret);
		return ret;
	}

	rsp.primary = rprimary & ~0x80000000u;	/* strip HIPCT BUSY */
	rsp.ext.val = rext;
	ret = rsp.status;

	if (ret == AVS_IPC_NOT_SUPPORTED)
		dev_dbg(adev->dev, "%s (0x%08x 0x%08x) failed: %d\n",
			name, request->glb.primary, request->glb.ext.val, ret);
	else if (ret)
		dev_err(adev->dev, "%s (0x%08x 0x%08x) failed: %d\n",
			name, request->glb.primary, request->glb.ext.val, ret);

	if (reply) {
		reply->header = (u64)rsp.primary | ((u64)rsp.ext.val << 32);
		/* payload is copied only for successful replies (avs_dsp_receive_rx) */
		reply->size = ret ? 0 : rx_size;
	}

	return ret;
}

int avs_dsp_send_msg(struct avs_dev *adev, struct avs_ipc_msg *request,
		     struct avs_ipc_msg *reply, const char *name)
{
	return avs_dsp_send_msg_timeout(adev, request, reply, AVS_IPC_TIMEOUT_MS, name);
}

/* No D0ix on this port: PM variants are plain sends. */
int avs_dsp_send_pm_msg_timeout(struct avs_dev *adev, struct avs_ipc_msg *request,
				struct avs_ipc_msg *reply, int timeout, bool wake_d0i0,
				const char *name)
{
	(void)wake_d0i0;
	return avs_dsp_send_msg_timeout(adev, request, reply, timeout, name);
}

int avs_dsp_send_pm_msg(struct avs_dev *adev, struct avs_ipc_msg *request,
			struct avs_ipc_msg *reply, bool wake_d0i0, const char *name)
{
	return avs_dsp_send_pm_msg_timeout(adev, request, reply, AVS_IPC_TIMEOUT_MS,
					   wake_d0i0, name);
}

int avs_dsp_send_rom_msg_timeout(struct avs_dev *adev, struct avs_ipc_msg *request, int timeout,
				 const char *name)
{
	return avs_dsp_send_msg_timeout(adev, request, NULL, timeout, name);
}

int avs_dsp_send_rom_msg(struct avs_dev *adev, struct avs_ipc_msg *request, const char *name)
{
	return avs_dsp_send_rom_msg_timeout(adev, request, AVS_IPC_TIMEOUT_MS, name);
}

/* ------------------------------------------------------------- lifecycle */

void avs_core_set_nhlt(const void *nhlt, unsigned long long size)
{
	acpi_nhlt_set_table(nhlt, (size_t)size);
}

int avs_core_create(void *host, struct avs_core **out)
{
	struct avs_core *core;
	struct avs_dev *adev;
	int ret;

	*out = NULL;
	core = kzalloc(sizeof(*core), GFP_KERNEL);
	if (!core)
		return -ENOMEM;

	core->host = host;
	core->dev.name = "avs";
	adev = &core->adev;
	adev->dev = &core->dev;
	adev->spec = &avs_port_skl_spec;
	adev->port_ctx = host;
	ida_init(&adev->ppl_ida);
	INIT_LIST_HEAD(&adev->comp_list);
	INIT_LIST_HEAD(&adev->path_list);

	adev->modcfg_buf = kzalloc(AVS_MAILBOX_SIZE, GFP_KERNEL);
	if (!adev->modcfg_buf) {
		ret = -ENOMEM;
		goto err;
	}

	/* Same queries as upstream avs_dsp_first_boot_firmware(). */
	ret = avs_ipc_get_hw_config(adev, &adev->hw_cfg);
	if (ret) {
		ret = AVS_IPC_RET(ret);
		dev_err(adev->dev, "get hw cfg failed: %d\n", ret);
		goto err;
	}

	ret = avs_ipc_get_fw_config(adev, &adev->fw_cfg);
	if (ret) {
		ret = AVS_IPC_RET(ret);
		dev_err(adev->dev, "get fw cfg failed: %d\n", ret);
		goto err;
	}

	ret = avs_module_info_init(adev, true);
	if (ret) {
		dev_err(adev->dev, "init module info failed: %d\n", ret);
		goto err;
	}

	dev_info(adev->dev, "firmware %u.%u.%u.%u: max_ppl_count=%u max_mod_inst=%u, "
		 "%u module(s), dsp_cores=%u\n",
		 adev->fw_cfg.fw_version.major, adev->fw_cfg.fw_version.minor,
		 adev->fw_cfg.fw_version.hotfix, adev->fw_cfg.fw_version.build,
		 adev->fw_cfg.max_ppl_count, adev->fw_cfg.max_mod_inst_count,
		 adev->mods_info->count, adev->hw_cfg.dsp_cores);

	*out = core;
	return 0;

err:
	kfree(adev->modcfg_buf);
	kfree(core);
	return ret;
}

void avs_core_destroy(struct avs_core *core)
{
	int i;

	if (!core)
		return;

	for (i = 0; i < AVS_PORT_MAX_STREAMS; i++)
		if (core->streams[i].in_use)
			avs_core_stream_close(core, &core->streams[i]);
	for (i = 0; i < core->num_tplgs; i++)
		avs_port_tplg_unload(&core->adev, core->tplgs[i]);
	avs_module_info_free(&core->adev);
	kfree(core->adev.modcfg_buf);
	kfree(core);
	/* topology dictionaries/templates (devm_*) */
	avs_port_devm_release_all();
}

int avs_core_load_tplg(struct avs_core *core, const void *data, unsigned long size,
		       unsigned int i2s_link_mask)
{
	struct avs_port_tplg *t;
	int ret;

	if (core->num_tplgs >= AVS_PORT_MAX_TPLGS)
		return -ENOSPC;

	ret = avs_port_tplg_load(&core->adev, data, size, i2s_link_mask, &t);
	if (ret)
		return ret;

	avs_port_tplg_dump(t);
	core->tplgs[core->num_tplgs++] = t;
	return 0;
}

/* --------------------------------------------------------------- lookups */

static struct avs_tplg_path_template *
avs_core_find_template(struct avs_core *core, const char *widget_name)
{
	struct avs_tplg_path_template *tmpl;
	int i;

	for (i = 0; i < core->num_tplgs; i++) {
		tmpl = avs_port_tplg_find_template(core->tplgs[i], widget_name);
		if (tmpl)
			return tmpl;
	}
	return NULL;
}

static struct avs_tplg_path_template *
avs_core_find_template_by_id(struct avs_core *core, const char *tplg_name, u32 id)
{
	struct avs_tplg_path_template *tmpl;
	int i;

	for (i = 0; i < core->num_tplgs; i++) {
		struct avs_tplg *tplg = avs_port_tplg_get(core->tplgs[i]);

		if (strcmp(tplg->name, tplg_name))
			continue;
		list_for_each_entry(tmpl, &tplg->path_tmpl_list, node)
			if (tmpl->id == id)
				return tmpl;
	}
	return NULL;
}

/*
 * The FE<->BE pairing upstream comes from ASoC DAI links. The topology
 * encodes the same thing as the FE pipeline's binding target (that is what
 * avs_path_bind() connects to), so we follow that.
 */
static struct avs_tplg_path_template *
avs_core_find_be_for(struct avs_core *core, struct avs_tplg_path_template *fe)
{
	struct avs_tplg_pipeline *ppl;
	struct avs_tplg_path *path;
	u32 i;

	if (list_empty(&fe->path_list))
		return NULL;
	path = list_first_entry(&fe->path_list, struct avs_tplg_path, node);

	list_for_each_entry(ppl, &path->ppl_list, node) {
		for (i = 0; i < ppl->num_bindings; i++) {
			struct avs_tplg_binding *b = ppl->bindings[i];
			struct avs_tplg_path_template *t;

			t = avs_core_find_template_by_id(core, b->target_tplg_name,
							 b->target_path_tmpl_id);
			if (t && t != fe)
				return t;
		}
	}
	return NULL;
}

static bool ends_with(const char *s, const char *suffix)
{
	size_t n = strlen(s), m = strlen(suffix);

	return n >= m && !strcmp(s + n - m, suffix);
}

static void fmt_to_params(const struct avs_audio_format *f, struct snd_pcm_hw_params *p)
{
	p->rate = f->sampling_freq;
	p->channels = f->num_channels;
	p->physical_width = f->bit_depth;
	p->msbits = f->valid_bit_depth;
}

int avs_core_find_playback(struct avs_core *core, const char *fe_name,
			   char *name_out, unsigned int name_len,
			   struct avs_core_fmt *fe_fmt)
{
	struct avs_tplg_path_template *tmpl = NULL;
	struct avs_tplg_path *path;
	int i;

	if (fe_name && fe_name[0]) {
		tmpl = avs_core_find_template(core, fe_name);
	} else {
		for (i = 0; i < core->num_tplgs && !tmpl; i++) {
			struct avs_tplg_path_template *t;

			list_for_each_entry(t, &avs_port_tplg_get(core->tplgs[i])->path_tmpl_list, node)
				if (t->w && ends_with(t->w->name, "p_fe")) {
					tmpl = t;
					break;
				}
		}
	}
	if (!tmpl || list_empty(&tmpl->path_list) || !tmpl->w) {
		dev_err(core->adev.dev, "FE template \"%s\" not found\n",
			fe_name && fe_name[0] ? fe_name : "*p_fe");
		/* porting aid: what the loaded topologies do offer */
		for (i = 0; i < core->num_tplgs; i++) {
			struct avs_tplg_path_template *t;

			list_for_each_entry(t, &avs_port_tplg_get(core->tplgs[i])->path_tmpl_list, node)
				if (t->w)
					dev_warn(core->adev.dev, "  path template in topology %d: \"%s\"\n",
						 i, t->w->name);
		}
		return -ENOENT;
	}

	path = list_first_entry(&tmpl->path_list, struct avs_tplg_path, node);
	if (!path->fe_fmt)
		return -EINVAL;

	if (name_out && name_len) {
		strncpy(name_out, tmpl->w->name, name_len - 1);
		name_out[name_len - 1] = '\0';
	}
	fe_fmt->rate = path->fe_fmt->sampling_freq;
	fe_fmt->channels = path->fe_fmt->num_channels;
	fe_fmt->bits = path->fe_fmt->bit_depth;
	fe_fmt->valid_bits = path->fe_fmt->valid_bit_depth;
	return 0;
}

/* Sets every topology control whose name contains @name_part (e.g. "DSP
 * Volume", "DMIC Volume") to @percent of its range; applied by the paths
 * created afterwards (path.c reads ctl_data->values at module init).
 * AVS-PORT: up to 200 % of the topology's max is allowed (Windows users
 * asked for more than the max98927 topology's 0x3FFFFFFF), but never past
 * S32_MAX -- the PEAKVOL value upstream path.c itself starts modules with. */
int avs_core_set_control_percent(struct avs_core *core, const char *name_part,
				 unsigned int percent)
{
	int i, n = 0;

	if (percent > 200)
		percent = 200;
	for (i = 0; i < core->num_tplgs; i++)
		n += avs_port_tplg_set_controls_pct(core->tplgs[i], name_part, percent);
	return n;
}

int avs_core_set_volume_percent(struct avs_core *core, unsigned int percent)
{
	int i, n = 0;

	if (percent > 100)
		percent = 100;
	for (i = 0; i < core->num_tplgs; i++)
		n += avs_port_tplg_set_controls_pct(core->tplgs[i], "Volume", percent);
	return n;
}

/* ---------------------------------------------------------- stream control */

int avs_core_stream_open(struct avs_core *core, const char *fe_name, unsigned int dma_id,
			 struct avs_core_stream **out)
{
	struct avs_tplg_path_template *fe_t, *be_t;
	struct snd_pcm_hw_params fe_params, be_params;
	struct avs_tplg_path *variant;
	struct avs_core_stream *s = NULL;
	struct avs_path *path;
	int i, ret;

	*out = NULL;
	fe_t = avs_core_find_template(core, fe_name);
	if (!fe_t || list_empty(&fe_t->path_list))
		return -ENOENT;
	be_t = avs_core_find_be_for(core, fe_t);
	if (!be_t) {
		dev_err(core->adev.dev, "no BE bound to FE \"%s\"\n", fe_name);
		return -ENOENT;
	}

	/* ASoC would share a running BE between FEs (DPCM refcounting); this
	 * port does not, so one stream per BE. */
	for (i = 0; i < AVS_PORT_MAX_STREAMS; i++) {
		if (core->streams[i].in_use) {
			if (core->streams[i].be_tmpl == be_t) {
				dev_err(core->adev.dev, "BE of \"%s\" already in use\n", fe_name);
				return -EBUSY;
			}
		} else if (!s) {
			s = &core->streams[i];
		}
	}
	if (!s)
		return -ENOSPC;

	/* First variant of the FE template defines both sides' formats. */
	variant = list_first_entry(&fe_t->path_list, struct avs_tplg_path, node);
	fmt_to_params(variant->fe_fmt, &fe_params);
	fmt_to_params(variant->be_fmt ? variant->be_fmt : variant->fe_fmt, &be_params);

	dev_info(core->adev.dev, "stream open: FE \"%s\" (dma_id %u) -> BE \"%s\", "
		 "fe %u/%u/%u/%uch be %u/%u/%u/%uch\n",
		 fe_t->w->name, dma_id, be_t->w ? be_t->w->name : "?",
		 fe_params.rate, fe_params.physical_width, fe_params.msbits, fe_params.channels,
		 be_params.rate, be_params.physical_width, be_params.msbits, be_params.channels);

	/* BE hw_params: non-HDA BE paths use dma_id 0 (pcm.c) */
	path = avs_path_create(&core->adev, 0, be_t, &fe_params, &be_params);
	if (IS_ERR_OR_NULL(path))
		return path ? (int)PTR_ERR(path) : -EINVAL;
	s->be = path;

	/* FE hw_params: dma_id = host stream_tag - 1 (pcm.c) */
	path = avs_path_create(&core->adev, dma_id, fe_t, &fe_params, &be_params);
	if (IS_ERR_OR_NULL(path)) {
		ret = path ? (int)PTR_ERR(path) : -EINVAL;
		goto err_be;
	}
	s->fe = path;

	ret = avs_path_bind(s->fe);
	if (ret)
		goto err_fe;

	/* prepare (dpcm_fe_dai_prepare): BE first, then FE */
	ret = avs_path_reset(s->be);
	if (!ret)
		ret = avs_path_pause(s->be);
	if (!ret)
		ret = avs_path_reset(s->fe);
	if (!ret)
		ret = avs_path_pause(s->fe);
	if (ret)
		goto err_unbind;

	s->be_tmpl = be_t;
	s->in_use = true;
	*out = s;
	return 0;

err_unbind:
	avs_path_unbind(s->fe);
err_fe:
	avs_path_free(s->fe);
	s->fe = NULL;
err_be:
	avs_path_free(s->be);
	s->be = NULL;
	return ret;
}

int avs_core_stream_run(struct avs_core *core, struct avs_core_stream *s)
{
	int ret;

	(void)core;
	if (!s || !s->in_use)
		return -EINVAL;

	/* START with SND_SOC_DPCM_TRIGGER_PRE: FE before BE. */
	ret = avs_path_pause(s->fe);
	if (!ret)
		ret = avs_path_run(s->fe, AVS_TPLG_TRIGGER_AUTO);
	if (!ret)
		ret = avs_path_pause(s->be);
	if (!ret)
		ret = avs_path_run(s->be, AVS_TPLG_TRIGGER_AUTO);
	return ret;
}

int avs_core_stream_stop(struct avs_core *core, struct avs_core_stream *s)
{
	int ret, r;

	(void)core;
	if (!s || !s->in_use)
		return -EINVAL;

	/* STOP/PAUSE_PUSH with SND_SOC_DPCM_TRIGGER_PRE: BE first
	 * (avs_dai_nonhda_be_trigger: pause, reset), then FE
	 * (avs_dai_fe_trigger: pause, host DMA stop, reset). */
	ret = avs_path_pause(s->be);
	r = avs_path_reset(s->be);
	if (r && !ret)
		ret = r;
	r = avs_path_pause(s->fe);
	if (r && !ret)
		ret = r;
	return ret;
}

int avs_core_stream_reset(struct avs_core *core, struct avs_core_stream *s)
{
	(void)core;
	if (!s || !s->in_use)
		return -EINVAL;
	return avs_path_reset(s->fe);
}

void avs_core_stream_close(struct avs_core *core, struct avs_core_stream *s)
{
	(void)core;
	if (!s || !s->in_use)
		return;
	/* dpcm_fe_dai_hw_free: FE first, then BE */
	if (s->fe) {
		avs_path_unbind(s->fe);
		avs_path_free(s->fe);
		s->fe = NULL;
	}
	if (s->be) {
		avs_path_free(s->be);
		s->be = NULL;
	}
	s->be_tmpl = NULL;
	s->in_use = false;
}
