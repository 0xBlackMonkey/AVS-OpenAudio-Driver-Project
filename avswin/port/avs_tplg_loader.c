// SPDX-License-Identifier: GPL-2.0-only
//
// avs_tplg_loader.c -- see avs_tplg_loader.h.
//
// Mirrors the parts of sound/soc/soc-topology.c that matter to AVS:
//   - blocks are processed MANIFEST first, then DAPM_WIDGET (soc-topology
//     walks the file in "passes" in that order, independent of block order);
//   - for every widget, its kcontrols are parsed first (mixer controls are
//     passed to ops->control_load, exactly like soc_tplg_control_dmixer_create)
//     and only then ops->widget_load() + ops->widget_ready() run;
//   - kcontrol name/max/min/invert/num_channels are copied into
//     struct soc_mixer_control the same way soc-topology does.
// Everything AVS-specific happens in the unmodified callbacks of topology.c.

#include "avs_tplg_loader.h"
#include "control.h"
#include "utils.h"

#define AVS_PORT_MAX_WIDGETS	64
#define AVS_PORT_MAX_KCTLS	64

struct avs_port_kctl {
	struct snd_kcontrol kctl;		/* id.name + private_value = &mc */
	struct soc_mixer_control mc;
};

struct avs_port_tplg {
	struct avs_port_device pdev;
	struct snd_soc_card card;
	struct snd_soc_component comp;
	struct avs_soc_component acomp;
	struct snd_soc_acpi_mach mach;
	struct avs_mach_pdata pdata;

	struct snd_soc_dapm_widget *widgets[AVS_PORT_MAX_WIDGETS];
	int num_widgets;
	struct avs_port_kctl *kctls[AVS_PORT_MAX_KCTLS];
	int num_kctls;
};

/* Every malformed-input exit logs where it happened (no silent -EINVAL). */
static int avs_port_tplg_bad(int line)
{
	dev_err(NULL, "tplg: malformed/unsupported input (avs_tplg_loader.c:%d) -- "
		"sizes: hdr=%u manifest=%u widget=%u mixer=%u private=%u\n", line,
		(unsigned int)sizeof(struct snd_soc_tplg_hdr),
		(unsigned int)sizeof(struct snd_soc_tplg_manifest),
		(unsigned int)sizeof(struct snd_soc_tplg_dapm_widget),
		(unsigned int)sizeof(struct snd_soc_tplg_mixer_control),
		(unsigned int)sizeof(struct snd_soc_tplg_private));
	return -EINVAL;
}

static bool in_blob(const u8 *base, size_t size, const void *p, size_t len)
{
	const u8 *q = (const u8 *)p;

	return q >= base && len <= size && (size_t)(q - base) <= size - len;
}

static int avs_port_load_mixer(struct avs_port_tplg *t, int index,
			       struct snd_soc_tplg_mixer_control *tmc,
			       struct snd_kcontrol **out)
{
	struct snd_kcontrol_new kcn;
	struct avs_port_kctl *k;
	int ret;

	if (t->num_kctls >= AVS_PORT_MAX_KCTLS)
		return -ENOSPC;
	if (strnlen(tmc->hdr.name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN) == SNDRV_CTL_ELEM_ID_NAME_MAXLEN)
		return avs_port_tplg_bad(__LINE__);

	k = kzalloc(sizeof(*k), GFP_KERNEL);
	if (!k)
		return -ENOMEM;

	memcpy(k->kctl.id.name, tmc->hdr.name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	k->kctl.private_value = (uintptr_t)&k->mc;
	k->mc.max = (int)le32_to_cpu(tmc->max);
	k->mc.min = (int)le32_to_cpu(tmc->min);
	k->mc.invert = le32_to_cpu(tmc->invert) ? 1 : 0;
	k->mc.platform_max = (int)le32_to_cpu(tmc->platform_max);
	k->mc.num_channels = le32_to_cpu(tmc->num_channels);

	kcn.private_value = (uintptr_t)&k->mc;
	ret = avs_tplg_ops.control_load(&t->comp, index, &kcn, &tmc->hdr);
	if (ret) {
		kfree(k);
		return ret;
	}

	t->kctls[t->num_kctls++] = k;
	*out = &k->kctl;
	return 0;
}

static int avs_port_load_widgets(struct avs_port_tplg *t, const u8 *base, size_t size,
				 const struct snd_soc_tplg_hdr *hdr)
{
	const u8 *pos = (const u8 *)(hdr + 1);
	const u8 *end = pos + le32_to_cpu(hdr->payload_size);
	u32 n, k;
	int ret;

	for (n = 0; n < le32_to_cpu(hdr->count); n++) {
		struct snd_soc_tplg_dapm_widget *dw = (struct snd_soc_tplg_dapm_widget *)pos;
		struct snd_soc_dapm_widget *w;
		u32 num_kctls;

		if (!in_blob(base, size, dw, sizeof(*dw)) || (const u8 *)(dw + 1) > end ||
		    le32_to_cpu(dw->size) != sizeof(*dw))
			return avs_port_tplg_bad(__LINE__);
		pos += sizeof(*dw) + le32_to_cpu(dw->priv.size);
		if (pos > end)
			return avs_port_tplg_bad(__LINE__);
		if (t->num_widgets >= AVS_PORT_MAX_WIDGETS)
			return -ENOSPC;

		w = kzalloc(sizeof(*w), GFP_KERNEL);
		if (!w)
			return -ENOMEM;
		t->widgets[t->num_widgets++] = w;

		w->name = kzalloc(SNDRV_CTL_ELEM_ID_NAME_MAXLEN, GFP_KERNEL);
		if (!w->name)
			return -ENOMEM;
		memcpy(w->name, dw->name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN - 1);
		w->ignore_suspend = le32_to_cpu(dw->ignore_suspend) ? 1 : 0;

		num_kctls = le32_to_cpu(dw->num_kcontrols);
		if (num_kctls) {
			w->kcontrols = kcalloc(num_kctls, sizeof(*w->kcontrols), GFP_KERNEL);
			if (!w->kcontrols)
				return -ENOMEM;
		}

		/* kcontrols first, as soc_tplg_dapm_widget_create() does */
		for (k = 0; k < num_kctls; k++) {
			struct snd_soc_tplg_ctl_hdr *ch = (struct snd_soc_tplg_ctl_hdr *)pos;

			if (!in_blob(base, size, ch, sizeof(*ch)) || (const u8 *)(ch + 1) > end)
				return avs_port_tplg_bad(__LINE__);

			switch (le32_to_cpu(ch->type)) {
			case SND_SOC_TPLG_TYPE_MIXER: {
				struct snd_soc_tplg_mixer_control *tmc =
					(struct snd_soc_tplg_mixer_control *)pos;

				if ((const u8 *)(tmc + 1) > end)
					return avs_port_tplg_bad(__LINE__);
				pos += sizeof(*tmc) + le32_to_cpu(tmc->priv.size);
				if (pos > end)
					return avs_port_tplg_bad(__LINE__);

				ret = avs_port_load_mixer(t, (int)le32_to_cpu(hdr->index), tmc,
							  &w->kcontrols[w->num_kcontrols]);
				if (ret) {
					dev_err(NULL, "widget %s: kcontrol %s load failed: %d\n",
						w->name, tmc->hdr.name, ret);
					return ret;
				}
				w->num_kcontrols++;
				break;
			}
			case SND_SOC_TPLG_TYPE_ENUM: {
				struct snd_soc_tplg_enum_control *ec =
					(struct snd_soc_tplg_enum_control *)pos;

				if ((const u8 *)(ec + 1) > end)
					return avs_port_tplg_bad(__LINE__);
				pos += sizeof(*ec) + le32_to_cpu(ec->priv.size);
				dev_warn(NULL, "widget %s: enum kcontrol %s ignored\n", w->name,
					 ec->hdr.name);
				break;
			}
			case SND_SOC_TPLG_TYPE_BYTES: {
				struct snd_soc_tplg_bytes_control *bc =
					(struct snd_soc_tplg_bytes_control *)pos;

				if ((const u8 *)(bc + 1) > end)
					return avs_port_tplg_bad(__LINE__);
				pos += sizeof(*bc) + le32_to_cpu(bc->priv.size);
				dev_warn(NULL, "widget %s: bytes kcontrol %s ignored\n", w->name,
					 bc->hdr.name);
				break;
			}
			default:
				dev_err(NULL, "widget %s: unknown kcontrol type %u\n", w->name,
					le32_to_cpu(ch->type));
				return avs_port_tplg_bad(__LINE__);
			}
			if (pos > end)
				return avs_port_tplg_bad(__LINE__);
		}

		ret = avs_tplg_ops.widget_load(&t->comp, (int)le32_to_cpu(hdr->index), w, dw);
		if (ret) {
			dev_err(NULL, "widget %s load failed: %d\n", dw->name, ret);
			return ret;
		}
		/* Widgets without private data carry no path template. */
		if (w->priv) {
			ret = avs_tplg_ops.widget_ready(&t->comp, (int)le32_to_cpu(hdr->index), w, dw);
			if (ret)
				return ret;
		}
	}

	return 0;
}

int avs_port_tplg_load(struct avs_dev *adev, const void *data, size_t size,
		       u32 i2s_link_mask, struct avs_port_tplg **out)
{
	const u8 *base = (const u8 *)data;
	const struct snd_soc_tplg_hdr *hdr;
	struct avs_port_tplg *t;
	bool manifest_done = false;
	size_t off;
	int pass, ret;

	*out = NULL;
	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;

	/* Machine description: what a board_selection.c entry would provide. */
	t->mach.mach_params.i2s_link_mask = i2s_link_mask;
	t->mach.pdata = &t->pdata;		/* pdata->tdms == NULL -> TDM slot 0 */
	t->pdev.base.name = "avs";
	t->pdev.mach = &t->mach;
	t->card.dev = &t->pdev.base;
	t->comp.dev = &t->pdev.base;
	t->comp.card = &t->card;
	t->comp.priv = &t->acomp;
	t->acomp.base = &t->comp;
	INIT_LIST_HEAD(&t->acomp.node);

	t->acomp.tplg = avs_tplg_new(&t->comp);
	if (!t->acomp.tplg) {
		kfree(t);
		return -ENOMEM;
	}

	/* pass 0: MANIFEST, pass 1: DAPM widgets -- like soc_tplg_load() */
	for (pass = 0; pass < 2; pass++) {
		for (off = 0; off < size; ) {
			hdr = (const struct snd_soc_tplg_hdr *)(base + off);
			if (!in_blob(base, size, hdr, sizeof(*hdr)) ||
			    le32_to_cpu(hdr->magic) != SND_SOC_TPLG_MAGIC ||
			    le32_to_cpu(hdr->size) != sizeof(*hdr) ||
			    !in_blob(base, size, hdr + 1, le32_to_cpu(hdr->payload_size))) {
				dev_err(NULL, "tplg: bad block header at offset 0x%x\n", (unsigned int)off);
				ret = -EINVAL;
				goto err;
			}

			if (pass == 0 && le32_to_cpu(hdr->type) == SND_SOC_TPLG_TYPE_MANIFEST) {
				struct snd_soc_tplg_manifest *m =
					(struct snd_soc_tplg_manifest *)(hdr + 1);

				if (le32_to_cpu(hdr->payload_size) < sizeof(*m) ||
				    !in_blob(base, size, m, sizeof(*m) + le32_to_cpu(m->priv.size))) {
					ret = avs_port_tplg_bad(__LINE__);
					goto err;
				}
				ret = avs_tplg_ops.manifest(&t->comp, (int)le32_to_cpu(hdr->index), m);
				if (ret) {
					dev_err(NULL, "tplg: manifest failed: %d\n", ret);
					goto err;
				}
				manifest_done = true;
			} else if (pass == 1 &&
				   le32_to_cpu(hdr->type) == SND_SOC_TPLG_TYPE_DAPM_WIDGET) {
				ret = avs_port_load_widgets(t, base, size, hdr);
				if (ret)
					goto err;
			}

			off += sizeof(*hdr) + le32_to_cpu(hdr->payload_size);
		}

		if (pass == 0 && !manifest_done) {
			dev_err(NULL, "tplg: no manifest block\n");
			ret = -EINVAL;
			goto err;
		}
	}

	guard(mutex)(&adev->comp_list_mutex);
	list_add_tail(&t->acomp.node, &adev->comp_list);
	*out = t;
	return 0;

err:
	avs_port_tplg_unload(adev, t);
	return ret;
}

void avs_port_tplg_unload(struct avs_dev *adev, struct avs_port_tplg *t)
{
	int i;

	(void)adev;
	if (!t)
		return;
	if (!list_empty(&t->acomp.node))
		list_del(&t->acomp.node);
	for (i = 0; i < t->num_widgets; i++) {
		kfree(t->widgets[i]->kcontrols);
		kfree(t->widgets[i]->name);
		kfree(t->widgets[i]);
	}
	for (i = 0; i < t->num_kctls; i++)
		kfree(t->kctls[i]);
	/* devm_* objects of the topology (templates, dictionaries) are released
	 * by the glue with avs_port_devm_release_all() on teardown. */
	kfree(t);
}

struct avs_tplg *avs_port_tplg_get(struct avs_port_tplg *t)
{
	return t->acomp.tplg;
}

struct avs_tplg_path_template *
avs_port_tplg_find_template(struct avs_port_tplg *t, const char *widget_name)
{
	struct avs_tplg_path_template *pos;

	list_for_each_entry(pos, &t->acomp.tplg->path_tmpl_list, node)
		if (pos->w && !strcmp(pos->w->name, widget_name))
			return pos;
	return NULL;
}

int avs_port_tplg_set_controls(struct avs_port_tplg *t, const char *name_part, long value)
{
	int i, ch, n = 0;

	for (i = 0; i < t->num_kctls; i++) {
		struct avs_port_kctl *k = t->kctls[i];
		struct avs_control_data *ctl_data = k->mc.dobj.private;

		if (!ctl_data || !strstr(k->kctl.id.name, name_part))
			continue;
		if (value < k->mc.min || value > k->mc.max) {
			dev_warn(NULL, "control %s: value %ld outside [%d, %d]\n",
				 k->kctl.id.name, value, k->mc.min, k->mc.max);
			continue;
		}
		for (ch = 0; ch < SND_SOC_TPLG_MAX_CHAN; ch++)
			ctl_data->values[ch] = value;
		n++;
	}
	return n;
}

int avs_port_tplg_set_controls_pct(struct avs_port_tplg *t, const char *name_part,
				   unsigned int percent)
{
	int i, ch, n = 0;

	for (i = 0; i < t->num_kctls; i++) {
		struct avs_port_kctl *k = t->kctls[i];
		struct avs_control_data *ctl_data = k->mc.dobj.private;
		long value;

		if (!ctl_data || !strstr(k->kctl.id.name, name_part))
			continue;
		{
			long long v = (long long)k->mc.min +
				      ((long long)k->mc.max - k->mc.min) * percent / 100;

			if (v > S32_MAX)	/* >100 % may exceed the range; cap */
				v = S32_MAX;
			value = (long)v;
		}
		if (percent > 100)
			dev_warn(NULL, "control \"%s\": %u%% is above the topology maximum 0x%x\n",
				 k->kctl.id.name, percent, (unsigned int)k->mc.max);
		for (ch = 0; ch < SND_SOC_TPLG_MAX_CHAN; ch++)
			ctl_data->values[ch] = value;
		dev_info(NULL, "control \"%s\" = 0x%x (%u%%)\n", k->kctl.id.name,
			 (unsigned int)value, percent);
		n++;
	}
	return n;
}

static void dump_fmt(const char *what, const struct avs_audio_format *f)
{
	if (!f) {
		dev_info(NULL, "        %s: (none)\n", what);
		return;
	}
	dev_info(NULL, "        %s: %u Hz %u/%u bit %uch map=0x%08X cfg=%u il=%u type=%u\n",
		 what, f->sampling_freq, f->bit_depth, f->valid_bit_depth, f->num_channels,
		 f->channel_map, f->channel_config, f->interleaving, f->sample_type);
}

void avs_port_tplg_dump(struct avs_port_tplg *t)
{
	struct avs_tplg *tplg = t->acomp.tplg;
	struct avs_tplg_path_template *tmpl;
	u32 i;

	dev_info(NULL, "topology \"%s\" version %u: %u fmts, %u modcfg_base, %u modcfg_ext, "
		 "%u pplcfgs, %u bindings, %u nhlt_configs\n",
		 tplg->name, tplg->version, tplg->num_fmts, tplg->num_modcfgs_base,
		 tplg->num_modcfgs_ext, tplg->num_pplcfgs, tplg->num_bindings,
		 tplg->num_nhlt_configs);

	for (i = 0; i < tplg->num_pplcfgs; i++)
		dev_info(NULL, "  pplcfg[%u]: req_size=%u priority=%u lp=%u attributes=0x%x trigger=%u\n",
			 i, tplg->pplcfgs[i].req_size, tplg->pplcfgs[i].priority,
			 tplg->pplcfgs[i].lp, tplg->pplcfgs[i].attributes,
			 (u32)tplg->pplcfgs[i].trigger);

	for (i = 0; i < tplg->num_bindings; i++) {
		struct avs_tplg_binding *b = &tplg->bindings[i];

		dev_info(NULL, "  binding[%u]: mod %u pin %u -> \"%s\" tmpl %u ppl %u mod %u pin %u "
			 "is_sink=%u\n", i, b->mod_id, b->mod_pin, b->target_tplg_name,
			 b->target_path_tmpl_id, b->target_ppl_id, b->target_mod_id,
			 b->target_mod_pin, b->is_sink);
	}

	for (i = 0; i < (u32)t->num_kctls; i++)
		dev_info(NULL, "  kcontrol \"%s\": min=%d max=%d channels=%u\n",
			 t->kctls[i]->kctl.id.name, t->kctls[i]->mc.min, t->kctls[i]->mc.max,
			 t->kctls[i]->mc.num_channels);

	list_for_each_entry(tmpl, &tplg->path_tmpl_list, node) {
		struct avs_tplg_path *path;

		dev_info(NULL, "  path template %u, widget \"%s\"\n", tmpl->id,
			 tmpl->w ? tmpl->w->name : "?");
		list_for_each_entry(path, &tmpl->path_list, node) {
			struct avs_tplg_pipeline *ppl;

			dev_info(NULL, "    path %u\n", path->id);
			dump_fmt("FE", path->fe_fmt);
			dump_fmt("BE", path->be_fmt);
			list_for_each_entry(ppl, &path->ppl_list, node) {
				struct avs_tplg_module *mod;

				dev_info(NULL, "      pipeline %u: req_size=%u, %u binding(s)\n",
					 ppl->id, ppl->cfg ? ppl->cfg->req_size : 0, ppl->num_bindings);
				list_for_each_entry(mod, &ppl->mod_list, node) {
					const u8 *g = mod->cfg_ext ? mod->cfg_ext->type.b : NULL;

					dev_info(NULL, "        module %u: cpc=%u ibs=%u obs=%u core=%u "
						 "domain=%u ctl_id=%u type=%02X%02X%02X%02X-...\n",
						 mod->id, mod->cfg_base ? mod->cfg_base->cpc : 0,
						 mod->cfg_base ? mod->cfg_base->ibs : 0,
						 mod->cfg_base ? mod->cfg_base->obs : 0,
						 mod->core_id, mod->domain, mod->ctl_id,
						 g ? g[3] : 0, g ? g[2] : 0, g ? g[1] : 0, g ? g[0] : 0);
					dump_fmt("in ", mod->in_fmt);
					if (mod->cfg_ext && guid_equal(&mod->cfg_ext->type,
								       &AVS_COPIER_MOD_UUID)) {
						dev_info(NULL, "          copier: dma_type=%d "
							 "dma_buf=%u vindex=0x%02X feature_mask=0x%x\n",
							 (int)mod->cfg_ext->copier.dma_type,
							 mod->cfg_ext->copier.dma_buffer_size,
							 mod->cfg_ext->copier.vindex.val,
							 mod->cfg_ext->copier.feature_mask);
						dump_fmt("out", mod->cfg_ext->copier.out_fmt);
						if (mod->cfg_ext->copier.blob_fmt)
							dump_fmt("blob", mod->cfg_ext->copier.blob_fmt);
					}
				}
			}
		}
	}
}
