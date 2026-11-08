// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020, Linaro Limited

#include <linux/cleanup.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/control.h>
#include <sound/asound.h>
#include <linux/firmware.h>
#include <sound/soc-topology.h>
#include <sound/soc-dpcm.h>
#include <uapi/sound/snd_ar_tokens.h>
#include <linux/kernel.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include "q6apm.h"
#include "audioreach.h"

struct snd_ar_control {
	u32 graph_id; /* Graph ID */
	u32 sgid; /* Sub Graph ID */
	u32 module_instance_id; /* Connected Module Instance ID */
	struct audioreach_route route;
	struct snd_soc_dapm_widget *w;
	struct list_head node;
	struct snd_soc_component *scomp;
	void *query;
	size_t query_size;
};

static struct audioreach_graph_info *audioreach_tplg_alloc_graph_info(struct q6apm *apm,
								      uint32_t graph_id,
								      bool *found)
{
	struct audioreach_graph_info *info;
	int ret;

	mutex_lock(&apm->lock);
	info = idr_find(&apm->graph_info_idr, graph_id);
	mutex_unlock(&apm->lock);

	if (info) {
		*found = true;
		return info;
	}

	*found = false;
	info = kzalloc_obj(*info);
	if (!info)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&info->sg_list);

	mutex_lock(&apm->lock);
	ret = idr_alloc_u32(&apm->graph_info_idr, info, &graph_id, graph_id, GFP_KERNEL);
	mutex_unlock(&apm->lock);

	if (ret < 0) {
		dev_err(apm->dev, "Failed to allocate Graph ID (%x)\n", graph_id);
		kfree(info);
		return ERR_PTR(ret);
	}

	info->id = graph_id;

	return info;
}

static void audioreach_tplg_add_sub_graph(struct audioreach_sub_graph *sg,
					  struct audioreach_graph_info *info)
{
	list_add_tail(&sg->node, &info->sg_list);
	sg->info = info;
	info->num_sub_graphs++;
}

static struct audioreach_sub_graph *audioreach_tplg_alloc_sub_graph(struct q6apm *apm,
								    uint32_t sub_graph_id,
								    bool *found)
{
	struct audioreach_sub_graph *sg;
	int ret;

	if (!sub_graph_id)
		return ERR_PTR(-EINVAL);

	/* Find if there is already a matching sub-graph */
	mutex_lock(&apm->lock);
	sg = idr_find(&apm->sub_graphs_idr, sub_graph_id);
	mutex_unlock(&apm->lock);

	if (sg) {
		*found = true;
		return sg;
	}

	*found = false;
	sg = kzalloc_obj(*sg);
	if (!sg)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&sg->container_list);

	mutex_lock(&apm->lock);
	ret = idr_alloc_u32(&apm->sub_graphs_idr, sg, &sub_graph_id, sub_graph_id, GFP_KERNEL);
	mutex_unlock(&apm->lock);

	if (ret < 0) {
		dev_err(apm->dev, "Failed to allocate Sub-Graph Instance ID (%x)\n", sub_graph_id);
		kfree(sg);
		return ERR_PTR(ret);
	}

	sg->sub_graph_id = sub_graph_id;

	return sg;
}

static struct audioreach_container *audioreach_tplg_alloc_container(struct q6apm *apm,
							    struct audioreach_sub_graph *sg,
							    uint32_t container_id,
							    bool *found)
{
	struct audioreach_container *cont;
	int ret;

	if (!container_id)
		return ERR_PTR(-EINVAL);

	mutex_lock(&apm->lock);
	cont = idr_find(&apm->containers_idr, container_id);
	mutex_unlock(&apm->lock);

	if (cont) {
		*found = true;
		return cont;
	}
	*found = false;

	cont = kzalloc_obj(*cont);
	if (!cont)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&cont->modules_list);

	mutex_lock(&apm->lock);
	ret = idr_alloc_u32(&apm->containers_idr, cont, &container_id, container_id, GFP_KERNEL);
	mutex_unlock(&apm->lock);

	if (ret < 0) {
		dev_err(apm->dev, "Failed to allocate Container Instance ID (%x)\n", container_id);
		kfree(cont);
		return ERR_PTR(ret);
	}

	cont->container_id = container_id;
	cont->sub_graph = sg;
	/* add to container list */
	list_add_tail(&cont->node, &sg->container_list);
	sg->num_containers++;

	return cont;
}

static struct audioreach_module *audioreach_tplg_alloc_module(struct q6apm *apm,
							      struct audioreach_container *cont,
							      struct snd_soc_dapm_widget *w,
							      uint32_t module_id, bool *found)
{
	struct audioreach_module *mod;
	int ret;

	mutex_lock(&apm->lock);
	mod = idr_find(&apm->modules_idr, module_id);
	mutex_unlock(&apm->lock);

	if (mod) {
		*found = true;
		return mod;
	}
	*found = false;
	mod = kzalloc_obj(*mod);
	if (!mod)
		return ERR_PTR(-ENOMEM);

	mutex_lock(&apm->lock);
	if (!module_id) { /* alloc module id dynamically */
		ret = idr_alloc_cyclic(&apm->modules_idr, mod,
				       AR_MODULE_DYNAMIC_INSTANCE_ID_START,
				       AR_MODULE_DYNAMIC_INSTANCE_ID_END, GFP_KERNEL);
	} else {
		ret = idr_alloc_u32(&apm->modules_idr, mod, &module_id, module_id, GFP_KERNEL);
	}
	mutex_unlock(&apm->lock);

	if (ret < 0) {
		dev_err(apm->dev, "Failed to allocate Module Instance ID (%x)\n", module_id);
		kfree(mod);
		return ERR_PTR(ret);
	}

	mod->instance_id = module_id;
	/* add to module list */
	list_add_tail(&mod->node, &cont->modules_list);
	mod->container = cont;
	mod->widget = w;
	cont->num_modules++;

	return mod;
}

static const struct snd_soc_tplg_vendor_array *
audioreach_get_sg_array(const struct snd_soc_tplg_private *private)
{
	const struct snd_soc_tplg_vendor_array *sg_array = NULL;
	bool found = false;
	int sz;

	for (sz = 0; !found && (sz < le32_to_cpu(private->size)); ) {
		const struct snd_soc_tplg_vendor_value_elem *sg_elem;
		int tkn_count = 0;

		sg_array = (struct snd_soc_tplg_vendor_array *)((u8 *)private->array + sz);
		sg_elem = sg_array->value;
		sz = sz + le32_to_cpu(sg_array->size);
		while (!found && tkn_count <= (le32_to_cpu(sg_array->num_elems) - 1)) {
			switch (le32_to_cpu(sg_elem->token)) {
			case AR_TKN_U32_SUB_GRAPH_INSTANCE_ID:
				found = true;
				break;
			default:
				break;
			}
			tkn_count++;
			sg_elem++;
		}
	}

	if (found)
		return sg_array;

	return NULL;
}

static const struct snd_soc_tplg_vendor_array *
audioreach_get_cont_array(const struct snd_soc_tplg_private *private)
{
	const struct snd_soc_tplg_vendor_array *cont_array = NULL;
	bool found = false;
	int sz;

	for (sz = 0; !found && (sz < le32_to_cpu(private->size)); ) {
		const struct snd_soc_tplg_vendor_value_elem *cont_elem;
		int tkn_count = 0;

		cont_array = (struct snd_soc_tplg_vendor_array *)((u8 *)private->array + sz);
		cont_elem = cont_array->value;
		sz = sz + le32_to_cpu(cont_array->size);
		while (!found && tkn_count <= (le32_to_cpu(cont_array->num_elems) - 1)) {
			switch (le32_to_cpu(cont_elem->token)) {
			case AR_TKN_U32_CONTAINER_INSTANCE_ID:
				found = true;
				break;
			default:
				break;
			}
			tkn_count++;
			cont_elem++;
		}
	}

	if (found)
		return cont_array;

	return NULL;
}

static const struct snd_soc_tplg_vendor_array *
audioreach_get_module_array(const struct snd_soc_tplg_private *private)
{
	const struct snd_soc_tplg_vendor_array *mod_array = NULL;
	bool found = false;
	int sz = 0;

	for (sz = 0; !found && (sz < le32_to_cpu(private->size)); ) {
		const struct snd_soc_tplg_vendor_value_elem *mod_elem;
		int tkn_count = 0;

		mod_array = (struct snd_soc_tplg_vendor_array *)((u8 *)private->array + sz);
		mod_elem = mod_array->value;
		sz = sz + le32_to_cpu(mod_array->size);
		while (!found && tkn_count <= (le32_to_cpu(mod_array->num_elems) - 1)) {
			switch (le32_to_cpu(mod_elem->token)) {
			case AR_TKN_U32_MODULE_INSTANCE_ID:
				found = true;
				break;
			default:
				break;
			}
			tkn_count++;
			mod_elem++;
		}
	}

	if (found)
		return mod_array;

	return NULL;
}

static struct audioreach_module_priv_data *
audioreach_get_module_priv_data(const struct snd_soc_tplg_private *private)
{
	const char *data = private->data;
	u32 remaining = le32_to_cpu(private->size);

	while (remaining) {
		const struct snd_soc_tplg_vendor_array *mod_array;
		u32 size;

		if (remaining < sizeof(*mod_array))
			return ERR_PTR(-EINVAL);

		mod_array = (const void *)data;
		size = le32_to_cpu(mod_array->size);
		if (size < sizeof(*mod_array) || size > remaining)
			return ERR_PTR(-EINVAL);

		if (le32_to_cpu(mod_array->type) == SND_SOC_AR_TPLG_MODULE_CFG_TYPE) {
			struct audioreach_module_priv_data *pdata;

			if (size < sizeof(*pdata))
				return ERR_PTR(-EINVAL);

			pdata = kmemdup(mod_array, size, GFP_KERNEL);
			if (!pdata)
				return ERR_PTR(-ENOMEM);

			return pdata;
		}

		data += size;
		remaining -= size;
	}

	return NULL;
}

static struct audioreach_sub_graph *audioreach_parse_sg_tokens(struct q6apm *apm,
							       const struct snd_soc_tplg_private *private)
{
	const struct snd_soc_tplg_vendor_value_elem *sg_elem;
	const struct snd_soc_tplg_vendor_array *sg_array;
	struct audioreach_graph_info *info = NULL;
	int graph_id, sub_graph_id, tkn_count = 0;
	struct audioreach_sub_graph *sg;
	bool found;

	sg_array = audioreach_get_sg_array(private);
	sg_elem = sg_array->value;

	while (tkn_count <= (le32_to_cpu(sg_array->num_elems) - 1)) {
		switch (le32_to_cpu(sg_elem->token)) {
		case AR_TKN_U32_SUB_GRAPH_INSTANCE_ID:
			sub_graph_id = le32_to_cpu(sg_elem->value);
			sg = audioreach_tplg_alloc_sub_graph(apm, sub_graph_id, &found);
			if (IS_ERR(sg)) {
				return sg;
			} else if (found) {
				/* Already parsed data for this sub-graph */
				return sg;
			}
			break;
		case AR_TKN_DAI_INDEX:
			/* Sub graph is associated with predefined graph */
			graph_id = le32_to_cpu(sg_elem->value);
			info = audioreach_tplg_alloc_graph_info(apm, graph_id, &found);
			if (IS_ERR(info))
				return ERR_CAST(info);
			break;
		case AR_TKN_U32_SUB_GRAPH_PERF_MODE:
			sg->perf_mode = le32_to_cpu(sg_elem->value);
			break;
		case AR_TKN_U32_SUB_GRAPH_DIRECTION:
			sg->direction = le32_to_cpu(sg_elem->value);
			break;
		case AR_TKN_U32_SUB_GRAPH_SCENARIO_ID:
			sg->scenario_id = le32_to_cpu(sg_elem->value);
			break;
		default:
			dev_err(apm->dev, "Not a valid token %d for graph\n", sg_elem->token);
			break;

		}
		tkn_count++;
		sg_elem++;
	}

	/* Sub graph is associated with predefined graph */
	if (info)
		audioreach_tplg_add_sub_graph(sg, info);

	return sg;
}

static struct audioreach_container *audioreach_parse_cont_tokens(struct q6apm *apm,
							 struct audioreach_sub_graph *sg,
							 const struct snd_soc_tplg_private *private)
{
	const struct snd_soc_tplg_vendor_value_elem *cont_elem;
	const struct snd_soc_tplg_vendor_array *cont_array;
	struct audioreach_container *cont;
	int container_id, tkn_count = 0;
	bool found = false;

	cont_array = audioreach_get_cont_array(private);
	cont_elem = cont_array->value;

	while (tkn_count <= (le32_to_cpu(cont_array->num_elems) - 1)) {
		switch (le32_to_cpu(cont_elem->token)) {
		case AR_TKN_U32_CONTAINER_INSTANCE_ID:
			container_id = le32_to_cpu(cont_elem->value);
			cont = audioreach_tplg_alloc_container(apm, sg, container_id, &found);
			if (IS_ERR(cont) || found)/* Error or Already parsed container data */
				return cont;
			break;
		case AR_TKN_U32_CONTAINER_CAPABILITY_ID:
			cont->capability_id = le32_to_cpu(cont_elem->value);
			break;
		case AR_TKN_U32_CONTAINER_STACK_SIZE:
			cont->stack_size = le32_to_cpu(cont_elem->value);
			break;
		case AR_TKN_U32_CONTAINER_GRAPH_POS:
			cont->graph_pos = le32_to_cpu(cont_elem->value);
			break;
		case AR_TKN_U32_CONTAINER_PROC_DOMAIN:
			cont->proc_domain = le32_to_cpu(cont_elem->value);
			break;
		default:
			dev_err(apm->dev, "Not a valid token %d for graph\n", cont_elem->token);
			break;

		}
		tkn_count++;
		cont_elem++;
	}

	return cont;
}

static struct audioreach_module *audioreach_parse_common_tokens(struct q6apm *apm,
							struct audioreach_container *cont,
							const struct snd_soc_tplg_private *private,
							struct snd_soc_dapm_widget *w)
{
	uint32_t max_ip_port = 0, max_op_port = 0;
	uint32_t src_mod_op_port_id[AR_MAX_MOD_LINKS] = { 0, };
	uint32_t dst_mod_inst_id[AR_MAX_MOD_LINKS] = { 0, };
	uint32_t dst_mod_ip_port_id[AR_MAX_MOD_LINKS] = { 0, };
	uint32_t src_mod_inst_id = 0;

	int module_id = 0, instance_id = 0, tkn_count = 0;
	const struct snd_soc_tplg_vendor_value_elem *mod_elem;
	const struct snd_soc_tplg_vendor_array *mod_array;
	struct audioreach_module *mod = NULL;
	uint32_t token;
	bool found;
	int max_tokens;

	mod_array = audioreach_get_module_array(private);
	mod_elem = mod_array->value;
	max_tokens = le32_to_cpu(mod_array->num_elems);
	while (tkn_count <= (max_tokens - 1)) {
		token = le32_to_cpu(mod_elem->token);
		switch (token) {
		/* common module info */
		case AR_TKN_U32_MODULE_ID:
			module_id = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_INSTANCE_ID:
			instance_id = le32_to_cpu(mod_elem->value);
			mod = audioreach_tplg_alloc_module(apm, cont, w,
							   instance_id, &found);
			if (IS_ERR(mod)) {
				return mod;
			} else if (found) {
				dev_err(apm->dev, "Duplicate Module Instance ID 0x%08x found\n",
					instance_id);
				return ERR_PTR(-EINVAL);
			}

			break;
		case AR_TKN_U32_MODULE_MAX_IP_PORTS:
			max_ip_port = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_MAX_OP_PORTS:
			max_op_port = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_SRC_INSTANCE_ID:
			src_mod_inst_id = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_SRC_OP_PORT_ID:
			src_mod_op_port_id[0] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_SRC_OP_PORT_ID1:
			src_mod_op_port_id[1] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_SRC_OP_PORT_ID2:
			src_mod_op_port_id[2] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_SRC_OP_PORT_ID3:
			src_mod_op_port_id[3] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_SRC_OP_PORT_ID4:
			src_mod_op_port_id[4] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_SRC_OP_PORT_ID5:
			src_mod_op_port_id[5] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_SRC_OP_PORT_ID6:
			src_mod_op_port_id[6] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_SRC_OP_PORT_ID7:
			src_mod_op_port_id[7] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_DST_INSTANCE_ID:
			dst_mod_inst_id[0] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_DST_INSTANCE_ID1:
			dst_mod_inst_id[1] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_DST_INSTANCE_ID2:
			dst_mod_inst_id[2] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_DST_INSTANCE_ID3:
			dst_mod_inst_id[3] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_DST_INSTANCE_ID4:
			dst_mod_inst_id[4] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_DST_INSTANCE_ID5:
			dst_mod_inst_id[5] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_DST_INSTANCE_ID6:
			dst_mod_inst_id[6] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_DST_INSTANCE_ID7:
			dst_mod_inst_id[7] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_DST_IN_PORT_ID:
			dst_mod_ip_port_id[0] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_DST_IN_PORT_ID1:
			dst_mod_ip_port_id[1] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_DST_IN_PORT_ID2:
			dst_mod_ip_port_id[2] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_DST_IN_PORT_ID3:
			dst_mod_ip_port_id[3] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_DST_IN_PORT_ID4:
			dst_mod_ip_port_id[4] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_DST_IN_PORT_ID5:
			dst_mod_ip_port_id[5] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_DST_IN_PORT_ID6:
			dst_mod_ip_port_id[6] = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_DST_IN_PORT_ID7:
			dst_mod_ip_port_id[7] = le32_to_cpu(mod_elem->value);
			break;
		default:
			break;

		}
		tkn_count++;
		mod_elem++;
	}

	if (mod) {
		int pn, id = 0;

		mod->module_id = module_id;
		mod->max_ip_port = max_ip_port;
		mod->max_op_port = max_op_port;
		mod->src_mod_inst_id = src_mod_inst_id;
		for (pn = 0; pn < ARRAY_SIZE(src_mod_op_port_id); pn++) {
			if (src_mod_op_port_id[pn] && dst_mod_inst_id[pn] &&
			    dst_mod_ip_port_id[pn]) {
				mod->src_mod_op_port_id[id] = src_mod_op_port_id[pn];
				mod->dst_mod_inst_id[id] = dst_mod_inst_id[pn];
				mod->dst_mod_ip_port_id[id] = dst_mod_ip_port_id[pn];
				id++;
			}
		}
		mod->num_connections = id;
	}

	return mod;
}

static int audioreach_widget_load_module_common(struct snd_soc_component *component,
						int index, struct snd_soc_dapm_widget *w,
						const struct snd_soc_tplg_dapm_widget *tplg_w)
{
	struct q6apm *apm = dev_get_drvdata(component->dev);
	struct audioreach_container *cont;
	struct audioreach_sub_graph *sg;
	struct audioreach_module *mod;
	struct snd_soc_dobj *dobj;
	struct audioreach_module_priv_data *data __free(kfree) =
		audioreach_get_module_priv_data(&tplg_w->priv);

	if (IS_ERR(data))
		return PTR_ERR(data);

	sg = audioreach_parse_sg_tokens(apm, &tplg_w->priv);
	if (IS_ERR(sg))
		return PTR_ERR(sg);

	cont = audioreach_parse_cont_tokens(apm, sg, &tplg_w->priv);
	if (IS_ERR(cont))
		return PTR_ERR(cont);

	mod = audioreach_parse_common_tokens(apm, cont, &tplg_w->priv, w);
	if (IS_ERR_OR_NULL(mod))
		return mod ? PTR_ERR(mod) : -ENODEV;

	mod->data = no_free_ptr(data);

	dobj = &w->dobj;
	dobj->private = mod;

	return 0;
}

static int audioreach_widget_load_enc_dec_cnv(struct snd_soc_component *component,
					      int index, struct snd_soc_dapm_widget *w,
					      const struct snd_soc_tplg_dapm_widget *tplg_w)
{
	const struct snd_soc_tplg_vendor_value_elem *mod_elem;
	const struct snd_soc_tplg_vendor_array *mod_array;
	struct audioreach_module *mod;
	struct snd_soc_dobj *dobj;
	int tkn_count = 0;
	int ret;

	ret = audioreach_widget_load_module_common(component, index, w, tplg_w);
	if (ret)
		return ret;

	dobj = &w->dobj;
	mod = dobj->private;
	mod_array = audioreach_get_module_array(&tplg_w->priv);
	mod_elem = mod_array->value;

	while (tkn_count <= (le32_to_cpu(mod_array->num_elems) - 1)) {
		switch (le32_to_cpu(mod_elem->token)) {
		case AR_TKN_U32_MODULE_FMT_INTERLEAVE:
			mod->interleave_type = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_FMT_SAMPLE_RATE:
			mod->rate = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_FMT_BIT_DEPTH:
			mod->bit_depth = le32_to_cpu(mod_elem->value);
			break;
		default:
			break;
		}
		tkn_count++;
		mod_elem++;
	}

	return 0;
}

static int audioreach_widget_log_module_load(struct audioreach_module *mod,
					     const struct snd_soc_tplg_vendor_array *mod_array)
{
	const struct snd_soc_tplg_vendor_value_elem *mod_elem;
	int tkn_count = 0;

	mod_elem = mod_array->value;

	while (tkn_count <= (le32_to_cpu(mod_array->num_elems) - 1)) {
		switch (le32_to_cpu(mod_elem->token)) {

		case AR_TKN_U32_MODULE_LOG_CODE:
			mod->log_code = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_LOG_TAP_POINT_ID:
			mod->log_tap_point_id = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_LOG_MODE:
			mod->log_mode = le32_to_cpu(mod_elem->value);
			break;
		default:
			break;
		}
		tkn_count++;
		mod_elem++;
	}

	return 0;
}

static int audioreach_widget_dma_module_load(struct audioreach_module *mod,
					     const struct snd_soc_tplg_vendor_array *mod_array)
{
	const struct snd_soc_tplg_vendor_value_elem *mod_elem;
	int tkn_count = 0;

	mod_elem = mod_array->value;

	while (tkn_count <= (le32_to_cpu(mod_array->num_elems) - 1)) {
		switch (le32_to_cpu(mod_elem->token)) {
		case AR_TKN_U32_MODULE_HW_IF_IDX:
			mod->hw_interface_idx = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_FMT_DATA:
			mod->data_format = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_HW_IF_TYPE:
			mod->hw_interface_type = le32_to_cpu(mod_elem->value);
			break;
		default:
			break;
		}
		tkn_count++;
		mod_elem++;
	}

	return 0;
}

static int audioreach_widget_i2s_module_load(struct audioreach_module *mod,
					     const struct snd_soc_tplg_vendor_array *mod_array)
{
	const struct snd_soc_tplg_vendor_value_elem *mod_elem;
	int tkn_count = 0;

	mod_elem = mod_array->value;

	while (tkn_count <= (le32_to_cpu(mod_array->num_elems) - 1)) {
		switch (le32_to_cpu(mod_elem->token)) {
		case AR_TKN_U32_MODULE_HW_IF_IDX:
			mod->hw_interface_idx = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_FMT_DATA:
			mod->data_format = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_HW_IF_TYPE:
			mod->hw_interface_type = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_SD_LINE_IDX:
			mod->sd_line_idx = le32_to_cpu(mod_elem->value);
			break;
		case AR_TKN_U32_MODULE_WS_SRC:
			mod->ws_src = le32_to_cpu(mod_elem->value);
			break;
		default:
			break;
		}
		tkn_count++;
		mod_elem++;
	}

	return 0;
}

static int audioreach_widget_audio_if_module_load(struct audioreach_module *mod,
						  const struct snd_soc_tplg_vendor_array *mod_array)
{
	const struct snd_soc_tplg_vendor_value_elem *mod_elem;
	int tkn_count = 0;
	u32 val;

	mod_elem = mod_array->value;

	while (tkn_count < le32_to_cpu(mod_array->num_elems)) {
		val = le32_to_cpu(mod_elem->value);
		switch (le32_to_cpu(mod_elem->token)) {
		case AR_TKN_U32_MODULE_HW_IF_IDX:
			mod->hw_interface_idx = val;
			break;
		case AR_TKN_U32_MODULE_FMT_DATA:
			mod->data_format = val;
			break;
		case AR_TKN_U16_MODULE_SYNC_SRC:
			if (val > U16_MAX)
				return -EINVAL;
			mod->sync_src = (u16)val;
			break;
		case AR_TKN_U16_MODULE_CTRL_DATA_OUT_ENABLE:
			if (val > U16_MAX)
				return -EINVAL;
			mod->ctrl_data_out_enable = (u16)val;
			break;
		case AR_TKN_U32_MODULE_SLOT_MASK:
			mod->slot_mask = val;
			break;
		case AR_TKN_U16_MODULE_NSLOTS_PER_FRAME:
			if (val > U16_MAX)
				return -EINVAL;
			mod->nslots_per_frame = (u16)val;
			break;
		case AR_TKN_U16_MODULE_SLOT_WIDTH:
			if (val > U16_MAX)
				return -EINVAL;
			mod->slot_width = (u16)val;
			break;
		case AR_TKN_U16_MODULE_INTF_MODE:
			if (val > U16_MAX)
				return -EINVAL;
			mod->intf_mode = (u16)val;
			break;
		case AR_TKN_U16_MODULE_SYNC_MODE:
			if (val > U16_MAX)
				return -EINVAL;
			mod->sync_mode = (u16)val;
			break;
		case AR_TKN_U16_MODULE_CTRL_INVERT_SYNC_PULSE:
			if (val > U16_MAX)
				return -EINVAL;
			mod->ctrl_invert_sync_pulse = (u16)val;
			break;
		case AR_TKN_U16_MODULE_CTRL_SYNC_DATA_DELAY:
			if (val > U16_MAX)
				return -EINVAL;
			mod->ctrl_sync_data_delay = (u16)val;
			break;
		case AR_TKN_U16_MODULE_QAIF_TYPE:
			if (val > U16_MAX)
				return -EINVAL;
			mod->qaif_type = (u16)val;
			break;
		case AR_TKN_U32_MODULE_ACTIVE_LANE_MASK:
			mod->active_lane_mask = val;
			break;
		case AR_TKN_U32_MODULE_FRAME_SYNC_RATE:
			mod->frame_sync_rate = val;
			break;
		case AR_TKN_U16_MODULE_BIT_CLK_TYPE:
			if (val > U16_MAX)
				return -EINVAL;
			mod->bit_clk_type = (u16)val;
			break;
		case AR_TKN_U8_MODULE_INV_INT_BIT_CLK:
			if (val > U8_MAX)
				return -EINVAL;
			mod->inv_int_bit_clk = (u8)val;
			break;
		case AR_TKN_U8_MODULE_INV_EXT_BIT_CLK:
			if (val > U8_MAX)
				return -EINVAL;
			mod->inv_ext_bit_clk = (u8)val;
			break;
		default:
			break;
		}
		tkn_count++;
		mod_elem++;
	}

	return 0;
}

static int audioreach_widget_dp_module_load(struct audioreach_module *mod,
					    const struct snd_soc_tplg_vendor_array *mod_array)
{
	const struct snd_soc_tplg_vendor_value_elem *mod_elem;
	int tkn_count = 0;

	mod_elem = mod_array->value;

	while (tkn_count <= (le32_to_cpu(mod_array->num_elems) - 1)) {
		switch (le32_to_cpu(mod_elem->token)) {
		case AR_TKN_U32_MODULE_FMT_DATA:
			mod->data_format = le32_to_cpu(mod_elem->value);
			break;
		default:
			break;
		}
		tkn_count++;
		mod_elem++;
	}

	return 0;
}

static int audioreach_widget_load_buffer(struct snd_soc_component *component,
					 int index, struct snd_soc_dapm_widget *w,
					 const struct snd_soc_tplg_dapm_widget *tplg_w)
{
	const struct snd_soc_tplg_vendor_array *mod_array;
	struct audioreach_module *mod;
	struct snd_soc_dobj *dobj;
	int ret;

	ret = audioreach_widget_load_module_common(component, index, w, tplg_w);
	if (ret)
		return ret;

	dobj = &w->dobj;
	mod = dobj->private;

	mod_array = audioreach_get_module_array(&tplg_w->priv);

	switch (mod->module_id) {
	case MODULE_ID_CODEC_DMA_SINK:
	case MODULE_ID_CODEC_DMA_SOURCE:
	case MODULE_ID_SLIMBUS_SINK:
	case MODULE_ID_SLIMBUS_SOURCE:
		audioreach_widget_dma_module_load(mod, mod_array);
		break;
	case MODULE_ID_DATA_LOGGING:
		audioreach_widget_log_module_load(mod, mod_array);
		break;
	case MODULE_ID_I2S_SINK:
	case MODULE_ID_I2S_SOURCE:
		audioreach_widget_i2s_module_load(mod, mod_array);
		break;
	case MODULE_ID_AUDIO_IF_SINK:
	case MODULE_ID_AUDIO_IF_SOURCE:
		ret = audioreach_widget_audio_if_module_load(mod, mod_array);
		if (ret)
			return ret;
		break;
	case MODULE_ID_DISPLAY_PORT_SINK:
	case MODULE_ID_USB_AUDIO_SINK:
	case MODULE_ID_USB_AUDIO_SOURCE:
		audioreach_widget_dp_module_load(mod, mod_array);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int audioreach_widget_load_mixer(struct snd_soc_component *component,
					int index, struct snd_soc_dapm_widget *w,
					const struct snd_soc_tplg_dapm_widget *tplg_w)
{
	const struct snd_soc_tplg_vendor_value_elem *w_elem;
	const struct snd_soc_tplg_vendor_array *w_array;
	struct snd_ar_control *scontrol;
	struct q6apm *data = dev_get_drvdata(component->dev);
	struct snd_soc_dobj *dobj;
	int tkn_count = 0;

	w_array = &tplg_w->priv.array[0];

	scontrol = kzalloc_obj(*scontrol);
	if (!scontrol)
		return -ENOMEM;

	scontrol->scomp = component;
	dobj = &w->dobj;
	dobj->private = scontrol;

	w_elem = w_array->value;
	while (tkn_count <= (le32_to_cpu(w_array->num_elems) - 1)) {
		switch (le32_to_cpu(w_elem->token)) {
		case AR_TKN_U32_SUB_GRAPH_INSTANCE_ID:
			scontrol->sgid = le32_to_cpu(w_elem->value);
			break;
		case AR_TKN_DAI_INDEX:
			scontrol->graph_id = le32_to_cpu(w_elem->value);
			break;
		default: /* ignore other tokens */
			break;
		}
		tkn_count++;
		w_elem++;
	}

	scontrol->w = w;
	list_add_tail(&scontrol->node, &data->widget_list);

	return 0;
}

static int audioreach_pga_event(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *kcontrol, int event)

{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	struct audioreach_module *mod = w->dobj.private;
	struct q6apm *apm = dev_get_drvdata(c->dev);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		/* apply gain after power up of widget */
		return audioreach_gain_set_vol_ctrl(apm, mod, mod->gain);
	default:
		break;
	}

	return 0;
}

static const struct snd_soc_tplg_widget_events audioreach_widget_ops[] = {
	{ AR_PGA_DAPM_EVENT, audioreach_pga_event },
};

static int audioreach_widget_load_pga(struct snd_soc_component *component,
				      int index, struct snd_soc_dapm_widget *w,
				      const struct snd_soc_tplg_dapm_widget *tplg_w)
{
	struct audioreach_module *mod;
	struct snd_soc_dobj *dobj;
	int ret;

	ret = audioreach_widget_load_module_common(component, index, w, tplg_w);
	if (ret)
		return ret;

	dobj = &w->dobj;
	mod = dobj->private;
	mod->gain = VOL_CTRL_DEFAULT_GAIN;

	ret = snd_soc_tplg_widget_bind_event(w, audioreach_widget_ops,
					     ARRAY_SIZE(audioreach_widget_ops),
					     le16_to_cpu(tplg_w->event_type));
	if (ret) {
		dev_err(component->dev, "matching event handlers NOT found for %d\n",
			le16_to_cpu(tplg_w->event_type));
		return -EINVAL;
	}

	return 0;
}

static int audioreach_widget_ready(struct snd_soc_component *component,
				   int index, struct snd_soc_dapm_widget *w,
				   struct snd_soc_tplg_dapm_widget *tplg_w)
{
	switch (w->id) {
	case snd_soc_dapm_aif_in:
	case snd_soc_dapm_aif_out:
		audioreach_widget_load_buffer(component, index, w, tplg_w);
		break;
	case snd_soc_dapm_decoder:
	case snd_soc_dapm_encoder:
	case snd_soc_dapm_src:
		audioreach_widget_load_enc_dec_cnv(component, index, w, tplg_w);
		break;
	case snd_soc_dapm_buffer:
		audioreach_widget_load_buffer(component, index, w, tplg_w);
		break;
	case snd_soc_dapm_mixer:
		return audioreach_widget_load_mixer(component, index, w, tplg_w);
	case snd_soc_dapm_pga:
		return audioreach_widget_load_pga(component, index, w, tplg_w);
	case snd_soc_dapm_dai_link:
	case snd_soc_dapm_scheduler:
	case snd_soc_dapm_out_drv:
	default:
		dev_err(component->dev, "Widget type (0x%x) not yet supported\n", w->id);
		break;
	}

	return 0;
}

static struct audioreach_graph_info *audioreach_get_widget_graph(struct q6apm *apm,
								 struct snd_soc_dapm_widget *w)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct audioreach_module *module;

	if (!component || component->dev != apm->dev || !w->dobj.private)
		return NULL;

	switch (w->id) {
	case snd_soc_dapm_mixer: {
		struct snd_ar_control *control = w->dobj.private;

		guard(mutex)(&apm->lock);

		return idr_find(&apm->graph_info_idr, control->graph_id);
	}
	case snd_soc_dapm_aif_in:
	case snd_soc_dapm_aif_out:
	case snd_soc_dapm_buffer:
	case snd_soc_dapm_decoder:
	case snd_soc_dapm_encoder:
	case snd_soc_dapm_src:
	case snd_soc_dapm_pga:
		module = w->dobj.private;
		return module->container->sub_graph->info;
	default:
		return NULL;
	}
}

int audioreach_get_dai_graph_id(struct snd_soc_pcm_runtime *be, struct snd_soc_dai *dai,
				int stream)
{
	struct q6apm *apm = dev_get_drvdata(dai->dev->parent);
	struct snd_soc_dapm_widget *port = snd_soc_dai_get_widget(dai, stream);
	enum snd_soc_dapm_direction dir = stream == SNDRV_PCM_STREAM_PLAYBACK ?
					 SND_SOC_DAPM_DIR_OUT : SND_SOC_DAPM_DIR_IN;
	struct snd_soc_dpcm *dpcm;
	int graph_id = -ENOENT;

	snd_soc_dpcm_mutex_assert_held(be);

	for_each_dpcm_fe(be, stream, dpcm) {
		struct snd_soc_dapm_widget_list *list;
		struct snd_soc_dapm_path *path;
		struct snd_soc_dapm_widget *w;
		struct snd_soc_dai *fe_dai = snd_soc_rtd_to_cpu(dpcm->fe, 0);
		int i, ret;

		if (dpcm->state == SND_SOC_DPCM_LINK_STATE_FREE)
			continue;

		ret = snd_soc_dapm_dai_get_connected_widgets(fe_dai, stream, &list,
							     dpcm_end_walk_at_be);
		if (ret < 0)
			return ret;

		ret = 0;

		snd_soc_dapm_mutex_lock(be->card);

		snd_soc_dapm_widget_for_each_path(port, dir, path) {
			struct snd_soc_dapm_widget *node = stream == SNDRV_PCM_STREAM_PLAYBACK ?
							 path->source : path->sink;
			struct audioreach_graph_info *info;

			if (path->is_supply || !path->connect)
				continue;

			info = audioreach_get_widget_graph(apm, node);
			if (!info)
				continue;

			for_each_dapm_widgets(list, i, w) {
				if (w != node)
					continue;

				if (graph_id >= 0 && graph_id != info->id) {
					ret = -EBUSY;
					goto out;
				}

				graph_id = info->id;
				break;
			}
		}

out:
		snd_soc_dapm_mutex_unlock(be->card);
		snd_soc_dapm_dai_free_widgets(&list);

		if (ret)
			return ret;
	}

	return graph_id < 0 ? dai->id : graph_id;
}
EXPORT_SYMBOL_GPL(audioreach_get_dai_graph_id);

static bool audioreach_graph_has_dai(struct snd_soc_card *card, struct q6apm *apm, u32 id)
{
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_dai *dai;
	int i;

	for_each_card_rtds(card, rtd) {
		for_each_rtd_cpu_dais(rtd, i, dai) {
			if ((dai->dev == apm->dev || dai->dev->parent == apm->dev) &&
			    dai->driver->id == id)
				return true;
		}
	}

	return false;
}

static bool audioreach_graph_is_connected(struct q6apm_graph *graph,
					  const struct snd_soc_dapm_widget_list *list, int stream)
{
	enum snd_soc_dapm_direction dir = stream == SNDRV_PCM_STREAM_PLAYBACK ?
					 SND_SOC_DAPM_DIR_OUT : SND_SOC_DAPM_DIR_IN;
	struct snd_soc_dapm_widget *w;
	int i;

	for_each_dapm_widgets(list, i, w) {
		struct audioreach_graph_info *info = audioreach_get_widget_graph(graph->apm, w);

		if (!info || info->id != graph->id)
			continue;

		if (snd_soc_dapm_widget_get_connected_paths(w, dir))
			return true;
	}

	return false;
}

void audioreach_stop_graphs(struct q6apm_graph *root, unsigned int first)
{
	while (root->num_started > first) {
		struct q6apm_graph *graph = root->connected[--root->num_started];
		int ret = q6apm_graph_stop(graph);

		if (ret)
			dev_err(graph->dev, "Failed to stop graph %u: %d\n", graph->id, ret);
	}
}
EXPORT_SYMBOL_GPL(audioreach_stop_graphs);

int audioreach_prepare_graphs(struct q6apm_graph *root,
			      const struct snd_soc_dapm_widget_list *list, int stream)
{
	unsigned int first = root->num_started;
	int i, ret;

	for (i = root->num_started; i < root->num_connected; i++) {
		struct q6apm_graph *graph = root->connected[i];

		if (!audioreach_graph_is_connected(graph, list, stream))
			continue;

		ret = q6apm_graph_prepare(graph);
		if (ret)
			goto err;

		ret = q6apm_graph_start(graph);
		if (ret)
			goto err;

		swap(root->connected[i], root->connected[root->num_started]);
		root->num_started++;
	}

	return 0;

err:
	audioreach_stop_graphs(root, first);

	return ret;
}
EXPORT_SYMBOL_GPL(audioreach_prepare_graphs);

int audioreach_open_graphs(struct q6apm_graph *root, struct snd_soc_pcm_runtime *rtd,
			   int stream, const struct snd_soc_dapm_widget_list *list)
{
	unsigned int first = root->num_connected;
	struct q6apm_graph **graphs;
	struct snd_soc_dapm_widget *w;
	int i, j, ret;

	graphs = krealloc_array(root->connected, first + list->num_widgets,
				sizeof(*graphs), GFP_KERNEL);
	if (!graphs)
		return -ENOMEM;

	root->connected = graphs;

	for_each_dapm_widgets(list, i, w) {
		struct audioreach_graph_info *info = audioreach_get_widget_graph(root->apm, w);
		struct q6apm_graph *graph;

		if (!info || audioreach_graph_has_dai(rtd->card, root->apm, info->id))
			continue;

		for (j = 0; j < root->num_connected; j++) {
			if (root->connected[j]->id == info->id)
				break;
		}

		if (j < root->num_connected)
			continue;

		graph = q6apm_graph_open(root->dev, NULL, NULL, info->id, stream);
		if (IS_ERR(graph)) {
			ret = PTR_ERR(graph);
			goto err;
		}

		root->connected[root->num_connected++] = graph;
	}

	return 0;

err:
	while (root->num_connected > first)
		q6apm_graph_close(root->connected[--root->num_connected]);

	return ret;
}
EXPORT_SYMBOL_GPL(audioreach_open_graphs);

static int audioreach_widget_unload(struct snd_soc_component *scomp,
				    struct snd_soc_dobj *dobj)
{
	const struct snd_soc_dapm_widget *w = container_of(dobj, struct snd_soc_dapm_widget, dobj);
	struct q6apm *apm = dev_get_drvdata(scomp->dev);
	struct audioreach_container *cont;
	struct audioreach_module *mod;

	if (w->id == snd_soc_dapm_mixer) {
		/* virtual widget */
		struct snd_ar_control *scontrol = dobj->private;

		list_del(&scontrol->node);
		kfree(scontrol);
		return 0;
	}
	mod = dobj->private;
	if (!mod)
		return 0;

	cont = mod->container;

	mutex_lock(&apm->lock);
	idr_remove(&apm->modules_idr, mod->instance_id);
	cont->num_modules--;

	list_del(&mod->node);
	kfree(mod->data);
	kfree(mod);
	/* Graph Info has N sub-graphs, sub-graph has N containers, Container has N Modules */
	if (list_empty(&cont->modules_list)) { /* if no modules in the container then remove it */
		struct audioreach_sub_graph *sg = cont->sub_graph;

		idr_remove(&apm->containers_idr, cont->container_id);
		list_del(&cont->node);
		sg->num_containers--;
		kfree(cont);
		/* check if there are no more containers in the sub graph and remove it */
		if (list_empty(&sg->container_list)) {
			struct audioreach_graph_info *info = sg->info;

			idr_remove(&apm->sub_graphs_idr, sg->sub_graph_id);
			list_del(&sg->node);
			info->num_sub_graphs--;
			kfree(sg);
			/* Check if there are no more sub-graphs left then remove graph info */
			if (list_empty(&info->sg_list)) {
				idr_remove(&apm->graph_info_idr, info->id);
				kfree(info);
			}
		}
	}

	mutex_unlock(&apm->lock);

	return 0;
}

static struct snd_ar_control *audioreach_find_widget(struct snd_soc_component *comp,
						     const char *name)
{
	struct q6apm *apm = dev_get_drvdata(comp->dev);
	struct snd_ar_control *control;

	list_for_each_entry(control, &apm->widget_list, node) {
		if (control->w && !strcmp(name, control->w->name))
			return control;
	}

	return NULL;
}

static struct audioreach_module *audioreach_find_module(struct snd_soc_component *comp,
							const char *name)
{
	struct q6apm *apm = dev_get_drvdata(comp->dev);
	struct audioreach_module *module;
	int id;

	idr_for_each_entry(&apm->modules_idr, module, id) {
		if (!strcmp(name, module->widget->name))
			return module;
	}

	return NULL;
}

static int audioreach_route_load(struct snd_soc_component *scomp, int index,
				 struct snd_soc_dapm_route *route)
{
	const struct audioreach_module *src_module, *sink_module;
	struct snd_ar_control *control;
	struct snd_soc_dapm_widget *w;
	int i;

	/* check if these are actual modules */
	src_module = audioreach_find_module(scomp, route->source);
	sink_module = audioreach_find_module(scomp, route->sink);

	if (sink_module && !src_module) {
		control = audioreach_find_widget(scomp, route->source);
		if (control)
			control->module_instance_id = sink_module->instance_id;

	} else if (!sink_module && src_module && route->control) {
		/* check if this is a virtual mixer */
		control = audioreach_find_widget(scomp, route->sink);
		if (!control || !control->w)
			return 0;

		w = control->w;

		for (i = 0; i < w->num_kcontrols; i++) {
			if (!strcmp(route->control, w->kcontrol_news[i].name)) {
				struct soc_mixer_control *sm;
				struct snd_soc_dobj *dobj;
				struct snd_ar_control *scontrol;

				sm = (struct soc_mixer_control *)w->kcontrol_news[i].private_value;
				dobj = &sm->dobj;
				scontrol = dobj->private;
				scontrol->module_instance_id = src_module->instance_id;
			}
		}

	}

	return 0;
}

static int audioreach_route_unload(struct snd_soc_component *scomp,
				   struct snd_soc_dobj *dobj)
{
	return 0;
}

static int audioreach_tplg_complete(struct snd_soc_component *component)
{
	/* TBD */
	return 0;
}

/* DAI link - used for any driver specific init */
static int audioreach_link_load(struct snd_soc_component *component, int index,
				struct snd_soc_dai_link *link,
				struct snd_soc_tplg_link_config *cfg)
{
	link->nonatomic = true;
	link->dynamic = true;
	link->platforms->name = NULL;
	link->platforms->of_node = of_get_compatible_child(component->dev->of_node,
							   "qcom,q6apm-dais");
	return 0;
}

static int audioreach_connect_route(struct q6apm *apm, struct audioreach_route *route,
				    u32 src_graph_id, u32 graph_id, bool connect)
{
	int ret;

	guard(mutex)(&apm->graph_lock);

	if (connect == !list_empty(&route->node))
		return 0;

	route->src_graph_id = src_graph_id;
	route->graph_id = graph_id;

	ret = q6apm_send_route(apm, route, connect ? APM_CMD_GRAPH_OPEN : APM_CMD_GRAPH_CLOSE);
	if (ret)
		return ret;

	if (connect)
		list_add_tail(&route->node, &apm->route_list);
	else
		list_del_init(&route->node);

	return 1;
}

static int audioreach_connect_sub_graphs(struct q6apm *apm,
					 struct snd_ar_control *m1,
					 const struct snd_ar_control *m2,
					 bool connect)
{
	struct audioreach_route *route = &m1->route;
	struct audioreach_graph_info *info;
	int ret;

	mutex_lock(&apm->lock);
	info = idr_find(&apm->graph_info_idr, m2->graph_id);
	mutex_unlock(&apm->lock);

	if (route->num_links) {
		ret = audioreach_connect_route(apm, route, m1->graph_id, m2->graph_id, connect);
		return ret < 0 ? ret : 0;
	}

	if (connect) {
		info->src_mod_inst_id = m1->module_instance_id;
		info->src_mod_op_port_id = 1;
		info->dst_mod_inst_id = m2->module_instance_id;
		info->dst_mod_ip_port_id = 2;

	} else {
		info->src_mod_inst_id = 0;
		info->src_mod_op_port_id = 0;
		info->dst_mod_inst_id = 0;
		info->dst_mod_ip_port_id = 0;
	}

	return 0;
}

static bool audioreach_is_vmixer_connected(struct q6apm *apm,
					   const struct snd_ar_control *m1,
					   const struct snd_ar_control *m2)
{
	const struct audioreach_graph_info *info;

	mutex_lock(&apm->lock);
	info = idr_find(&apm->graph_info_idr, m2->graph_id);
	mutex_unlock(&apm->lock);

	if (m1->route.num_links)
		return !list_empty(&m1->route.node);

	if (info->dst_mod_inst_id == m2->module_instance_id &&
	    info->src_mod_inst_id == m1->module_instance_id)
		return true;

	return false;
}

static int audioreach_get_audio_mixer(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc = (struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_to_dapm(kcontrol);
	const struct snd_soc_dapm_widget *dw = snd_soc_dapm_kcontrol_to_widget(kcontrol);
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	const struct snd_ar_control *dapm_scontrol = dw->dobj.private;
	const struct snd_ar_control *scontrol = mc->dobj.private;
	struct q6apm *data = dev_get_drvdata(c->dev);
	bool connected;

	connected = audioreach_is_vmixer_connected(data, scontrol, dapm_scontrol);
	if (connected)
		ucontrol->value.integer.value[0] = 1;
	else
		ucontrol->value.integer.value[0] = 0;

	return 0;
}

static void audioreach_prune_graphs(struct q6apm_graph *root,
				    struct snd_soc_dapm_widget_list *list)
{
	struct snd_soc_dapm_widget *w;
	int i, j;

	for (i = root->num_connected; i-- > 0;) {
		struct q6apm_graph *graph = root->connected[i];
		bool found = false;

		for_each_dapm_widgets(list, j, w) {
			struct audioreach_graph_info *info;

			info = audioreach_get_widget_graph(root->apm, w);

			if (info && info->id == graph->id) {
				found = true;
				break;
			}
		}

		if (found)
			continue;

		if (i < root->num_started) {
			int ret = q6apm_graph_stop(graph);

			if (ret)
				dev_err(graph->dev, "Failed to stop graph %u: %d\n",
					graph->id, ret);

			root->num_started--;
		}

		q6apm_graph_close(graph);
		root->num_connected--;
		memmove(&root->connected[i], &root->connected[i + 1],
			(root->num_connected - i) * sizeof(*root->connected));
	}
}

static int audioreach_update_paths(struct q6apm *apm, struct snd_soc_card *card, bool connect)
{
	struct snd_soc_pcm_runtime *rtd;
	int stream, ret = 0;

	snd_soc_dpcm_mutex_lock(card);

	for_each_card_rtds(card, rtd) {
		struct snd_soc_dai *dai = snd_soc_rtd_to_cpu(rtd, 0);

		if (!rtd->dai_link->dynamic || dai->dev != apm->dev)
			continue;

		for_each_pcm_streams(stream) {
			struct q6apm_graph *root = snd_soc_dai_dma_data_get(dai, stream);
			struct snd_soc_dapm_widget_list *list;

			if (!root || !root->connected)
				continue;

			ret = snd_soc_dapm_dai_get_connected_widgets(dai, stream, &list,
								     dpcm_end_walk_at_be);
			if (ret < 0)
				goto out;

			if (connect) {
				ret = audioreach_open_graphs(root, rtd, stream, list);

				if (!ret && root->path_prepared)
					ret = audioreach_prepare_graphs(root, list, stream);
			} else {
				audioreach_prune_graphs(root, list);
			}

			snd_soc_dapm_dai_free_widgets(&list);

			if (ret < 0)
				goto out;
		}
	}

	ret = 0;

out:
	snd_soc_dpcm_mutex_unlock(card);

	return ret;
}

static int audioreach_put_audio_mixer(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc = (struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_to_dapm(kcontrol);
	struct snd_soc_dapm_widget *dw = snd_soc_dapm_kcontrol_to_widget(kcontrol);
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	const struct snd_ar_control *dapm_scontrol = dw->dobj.private;
	struct snd_ar_control *scontrol = mc->dobj.private;
	struct q6apm *data = dev_get_drvdata(c->dev);
	bool connect = ucontrol->value.integer.value[0];
	int ret;

	ret = audioreach_connect_sub_graphs(data, scontrol, dapm_scontrol, connect);
	if (ret)
		return ret;

	snd_soc_dapm_mixer_update_power(dapm, kcontrol, connect, NULL);

	if (scontrol->route.num_links)
		return audioreach_update_paths(data, c->card, connect);

	return 0;
}

static int audioreach_get_vol_ctrl_audio_mixer(struct snd_kcontrol *kcontrol,
					       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget *dw = snd_soc_dapm_kcontrol_to_widget(kcontrol);
	struct audioreach_module *mod = dw->dobj.private;

	ucontrol->value.integer.value[0] = mod->gain;

	return 0;
}

static int audioreach_put_vol_ctrl_audio_mixer(struct snd_kcontrol *kcontrol,
					       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_to_dapm(kcontrol);
	struct snd_soc_dapm_widget *dw = snd_soc_dapm_kcontrol_to_widget(kcontrol);
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	struct audioreach_module *mod = dw->dobj.private;
	struct q6apm *apm = dev_get_drvdata(c->dev);
	int gain = ucontrol->value.integer.value[0];
	int ret = 0;

	snd_soc_dapm_mutex_lock(dapm);

	if (mod->gain == gain)
		goto out;

	if (dw->power) {
		ret = audioreach_gain_set_vol_ctrl(apm, mod, gain);
		if (ret)
			goto out;
	}

	mod->gain = gain;
	ret = 1;

out:
	snd_soc_dapm_mutex_unlock(dapm);

	return ret;
}

static int audioreach_control_load_links(struct audioreach_route *route,
					 const struct snd_soc_tplg_private *priv)
{
	const u8 *data = (const u8 *)priv->data;
	u32 remaining = le32_to_cpu(priv->size);
	struct audioreach_connection *links;
	int ret = -EINVAL;
	u32 i;

	while (remaining) {
		const struct snd_soc_tplg_vendor_array *array = (const void *)data;
		struct audioreach_connection link = {};
		u32 size;

		if (remaining < sizeof(*array))
			goto err;

		size = le32_to_cpu(array->size);
		if (size < struct_size(array, value, le32_to_cpu(array->num_elems)) ||
		    size > remaining)
			goto err;

		data += size;
		remaining -= size;

		if (le32_to_cpu(array->type) != SND_SOC_TPLG_TUPLE_TYPE_WORD)
			continue;

		for (i = 0; i < le32_to_cpu(array->num_elems); i++) {
			u32 value = le32_to_cpu(array->value[i].value);

			switch (le32_to_cpu(array->value[i].token)) {
			case AR_TKN_U32_MODULE_SRC_INSTANCE_ID:
				link.src_mod_inst_id = value;
				break;
			case AR_TKN_U32_MODULE_SRC_OP_PORT_ID:
				link.src_mod_op_port_id = value;
				break;
			case AR_TKN_U32_MODULE_DST_INSTANCE_ID:
				link.dst_mod_inst_id = value;
				break;
			case AR_TKN_U32_MODULE_DST_IN_PORT_ID:
				link.dst_mod_ip_port_id = value;
				break;
			}
		}

		if (!link.src_mod_inst_id)
			continue;

		if (!link.src_mod_op_port_id || !link.dst_mod_inst_id ||
		    !link.dst_mod_ip_port_id)
			goto err;

		links = krealloc_array(route->links, route->num_links + 1,
				       sizeof(*links), GFP_KERNEL);
		if (!links) {
			ret = -ENOMEM;
			goto err;
		}

		links[route->num_links++] = link;
		route->links = links;
	}

	return 0;

err:
	kfree(route->links);
	route->links = NULL;
	route->num_links = 0;

	return ret;
}

static int audioreach_control_load_mix(struct snd_soc_component *scomp,
				       struct snd_ar_control *scontrol,
				       struct snd_kcontrol_new *kc,
				       const struct snd_soc_tplg_ctl_hdr *hdr)
{
	const struct snd_soc_tplg_vendor_value_elem *c_elem;
	const struct snd_soc_tplg_vendor_array *c_array;
	const struct snd_soc_tplg_mixer_control *mc;
	int tkn_count = 0;

	mc = container_of_const(hdr, struct snd_soc_tplg_mixer_control, hdr);
	c_array = (struct snd_soc_tplg_vendor_array *)mc->priv.data;

	c_elem = c_array->value;

	while (tkn_count <= (le32_to_cpu(c_array->num_elems) - 1)) {
		switch (le32_to_cpu(c_elem->token)) {
		case AR_TKN_U32_SUB_GRAPH_INSTANCE_ID:
			scontrol->sgid = le32_to_cpu(c_elem->value);
			break;
		case AR_TKN_DAI_INDEX:
			scontrol->graph_id = le32_to_cpu(c_elem->value);
			break;
		default:
			/* Ignore other tokens */
			break;
		}
		c_elem++;
		tkn_count++;
	}

	return audioreach_control_load_links(&scontrol->route, &mc->priv);
}

static void *audioreach_copy_params(const unsigned int __user *bytes,
				    unsigned int size, u32 type, size_t *length)
{
	u32 tlv[2];

	if (size < sizeof(tlv))
		return ERR_PTR(-EINVAL);

	if (copy_from_user(tlv, bytes, sizeof(tlv)))
		return ERR_PTR(-EFAULT);

	if (tlv[0] != type || tlv[1] > size - sizeof(tlv))
		return ERR_PTR(-EINVAL);

	*length = tlv[1];

	return vmemdup_user(bytes + ARRAY_SIZE(tlv), tlv[1]);
}

static int audioreach_put_params(struct snd_kcontrol *kcontrol,
				 const unsigned int __user *bytes, unsigned int size)
{
	struct soc_bytes_ext *sbe = (void *)kcontrol->private_value;
	struct snd_ar_control *scontrol = sbe->dobj.private;
	struct q6apm *apm = dev_get_drvdata(scontrol->scomp->dev);
	size_t length;

	void *data __free(kvfree) =
		audioreach_copy_params(bytes, size, SND_SOC_AR_TLV_SET_PARAMS, &length);
	if (IS_ERR(data))
		return PTR_ERR(data);

	return q6apm_set_params(apm, scontrol->graph_id, data, length);
}

static int audioreach_put_query(struct snd_kcontrol *kcontrol,
				const unsigned int __user *bytes, unsigned int size)
{
	struct soc_bytes_ext *sbe = (void *)kcontrol->private_value;
	struct snd_ar_control *scontrol = sbe->dobj.private;
	size_t length;
	void *query;

	query = audioreach_copy_params(bytes, size, SND_SOC_AR_TLV_GET_PARAMS, &length);
	if (IS_ERR(query))
		return PTR_ERR(query);

	kvfree(scontrol->query);
	scontrol->query = query;
	scontrol->query_size = length;

	return 0;
}

static int audioreach_get_params(struct snd_kcontrol *kcontrol,
				 unsigned int __user *bytes, unsigned int size)
{
	struct soc_bytes_ext *sbe = (void *)kcontrol->private_value;
	struct snd_ar_control *scontrol = sbe->dobj.private;
	struct q6apm *apm = dev_get_drvdata(scontrol->scomp->dev);
	u32 tlv[2] = { SND_SOC_AR_TLV_GET_PARAMS };
	size_t length;
	int ret;

	if (!scontrol->query)
		return -ENODATA;

	if (size < sizeof(tlv))
		return -EINVAL;

	length = min(size - sizeof(tlv), Q6APM_MAX_INBAND_PARAMS);

	void *data __free(kfree) = kmalloc(length, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	ret = q6apm_get_params(apm, scontrol->graph_id, scontrol->query,
			       scontrol->query_size, data, &length);
	if (ret)
		return ret;

	tlv[1] = length;

	if (copy_to_user(bytes, tlv, sizeof(tlv)) ||
	    copy_to_user(bytes + ARRAY_SIZE(tlv), data, length))
		return -EFAULT;

	return 0;
}

static int audioreach_control_load_params(struct snd_soc_component *scomp,
					  struct snd_kcontrol_new *kc,
					  struct snd_soc_tplg_ctl_hdr *hdr)
{
	struct snd_soc_tplg_bytes_control *be =
		container_of(hdr, struct snd_soc_tplg_bytes_control, hdr);
	const struct snd_soc_tplg_vendor_array *array = (const void *)be->priv.data;
	struct soc_bytes_ext *sbe = (void *)kc->private_value;
	struct snd_ar_control *scontrol;

	if ((le32_to_cpu(be->ext_ops.put) != SND_SOC_AR_TPLG_SET_PARAMS &&
	     le32_to_cpu(be->ext_ops.put) != SND_SOC_AR_TPLG_GET_PARAMS) ||
	    le32_to_cpu(be->priv.size) < struct_size(array, value, 1) ||
	    le32_to_cpu(array->type) != SND_SOC_TPLG_TUPLE_TYPE_WORD ||
	    le32_to_cpu(array->num_elems) != 1 ||
	    le32_to_cpu(array->value[0].token) != AR_TKN_DAI_INDEX)
		return -EINVAL;

	scontrol = kzalloc_obj(*scontrol);
	if (!scontrol)
		return -ENOMEM;

	scontrol->scomp = scomp;
	scontrol->graph_id = le32_to_cpu(array->value[0].value);
	sbe->dobj.private = scontrol;

	return 0;
}

static int audioreach_control_load(struct snd_soc_component *scomp, int index,
				   struct snd_kcontrol_new *kc,
				   struct snd_soc_tplg_ctl_hdr *hdr)
{
	struct snd_ar_control *scontrol;
	struct soc_mixer_control *sm;
	struct snd_soc_dobj *dobj;
	int ret = 0;

	if (le32_to_cpu(hdr->type) == SND_SOC_TPLG_TYPE_BYTES)
		return audioreach_control_load_params(scomp, kc, hdr);

	scontrol = kzalloc_obj(*scontrol);
	if (!scontrol)
		return -ENOMEM;

	scontrol->scomp = scomp;
	INIT_LIST_HEAD(&scontrol->route.node);

	switch (le32_to_cpu(hdr->ops.get)) {
	case SND_SOC_AR_TPLG_FE_BE_GRAPH_CTL_MIX:
		sm = (struct soc_mixer_control *)kc->private_value;
		dobj = &sm->dobj;
		ret = audioreach_control_load_mix(scomp, scontrol, kc, hdr);
		break;
	case SND_SOC_AR_TPLG_VOL_CTL:
		sm = (struct soc_mixer_control *)kc->private_value;
		dobj = &sm->dobj;
		break;
	default:
		dev_warn(scomp->dev, "control type not supported %d:%d:%d\n",
			 hdr->ops.get, hdr->ops.put, hdr->ops.info);
		kfree(scontrol);
		return -EINVAL;
	}

	dobj->private = scontrol;
	return ret;
}

static int audioreach_control_unload(struct snd_soc_component *scomp,
				     struct snd_soc_dobj *dobj)
{
	struct snd_ar_control *scontrol = dobj->private;
	struct q6apm *apm = dev_get_drvdata(scomp->dev);

	if (scontrol->route.num_links) {
		scoped_guard(mutex, &apm->graph_lock)
			list_del(&scontrol->route.node);
		kfree(scontrol->route.links);
	}

	kvfree(scontrol->query);
	kfree(scontrol);

	return 0;
}

static const struct snd_soc_tplg_kcontrol_ops audioreach_io_ops[] = {
	{SND_SOC_AR_TPLG_FE_BE_GRAPH_CTL_MIX, audioreach_get_audio_mixer,
		audioreach_put_audio_mixer, snd_soc_info_volsw},
	{SND_SOC_AR_TPLG_VOL_CTL, audioreach_get_vol_ctrl_audio_mixer,
		audioreach_put_vol_ctrl_audio_mixer, snd_soc_info_volsw},
};

static const struct snd_soc_tplg_bytes_ext_ops audioreach_bytes_ext_ops[] = {
	{ SND_SOC_AR_TPLG_SET_PARAMS, NULL, audioreach_put_params },
	{ SND_SOC_AR_TPLG_GET_PARAMS, audioreach_get_params, audioreach_put_query },
};

static const struct snd_soc_tplg_ops audioreach_tplg_ops = {
	.io_ops = audioreach_io_ops,
	.io_ops_count = ARRAY_SIZE(audioreach_io_ops),
	.bytes_ext_ops = audioreach_bytes_ext_ops,
	.bytes_ext_ops_count = ARRAY_SIZE(audioreach_bytes_ext_ops),

	.control_load	= audioreach_control_load,
	.control_unload	= audioreach_control_unload,

	.widget_ready = audioreach_widget_ready,
	.widget_unload = audioreach_widget_unload,

	.complete = audioreach_tplg_complete,
	.link_load = audioreach_link_load,

	.dapm_route_load	= audioreach_route_load,
	.dapm_route_unload	= audioreach_route_unload,
};

int audioreach_tplg_init(struct snd_soc_component *component)
{
	struct snd_soc_card *card = component->card;
	struct device *dev = component->dev;
	int ret;

	/* Inline with Qualcomm UCM configs and linux-firmware path */
	char *tplg_fw_name __free(kfree) = kasprintf(GFP_KERNEL, "qcom/%s/%s-tplg.bin",
						     card->driver_name,
						     card->name);
	if (!tplg_fw_name)
		return -ENOMEM;

	const struct firmware *fw __free(firmware) = NULL;
	ret = request_firmware(&fw, tplg_fw_name, dev);
	if (ret < 0) {
		dev_err(dev, "tplg firmware loading %s failed %d\n", tplg_fw_name, ret);
		return ret;
	}

	ret = snd_soc_tplg_component_load(component, &audioreach_tplg_ops, fw);
	if (ret < 0) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "tplg component load failed: %d\n", ret);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(audioreach_tplg_init);
