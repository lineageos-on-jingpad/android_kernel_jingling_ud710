/*
 * Copyright (C) 2017-2018 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/of.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/sprd_ion.h>
#include <linux/kthread.h>

#include "sprd_isp_hw.h"
#include "sprd_img.h"
#include <video/sprd_mmsys_pw_domain.h>
#include <sprd_mm.h>

#include "cam_trusty.h"
#include "cam_queue.h"
#include "cam_debugger.h"
#include "isp_int.h"
#include "isp_reg.h"
#include "isp_core.h"
#include "isp_path.h"
#include "isp_slice.h"
#include "isp_cfg.h"

#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) "ISP_CORE: %d %d %s : "\
	fmt, current->pid, __LINE__, __func__

unsigned long *isp_cfg_poll_addr[ISP_CONTEXT_SW_NUM];
static int sprd_isp_put_context(
	void *isp_handle, int ctx_id);
static int sprd_isp_put_path(
	void *isp_handle, int ctx_id, int path_id);
static int isp_slice_ctx_init(struct isp_pipe_context *pctx, uint32_t *multi_slice);
static int isp_init_statis_q(void *isp_handle, int ctx_id);
static int isp_unmap_statis_buffer(void *isp_handle, int ctx_id);
static int sprd_isp_proc_frame(void *isp_handle, void *param, int ctx_id);

static DEFINE_MUTEX(isp_pipe_dev_mutex);

struct isp_pipe_dev *s_isp_dev;
uint32_t s_dbg_linebuf_len = ISP_LINE_BUFFER_W;
extern int s_dbg_work_mode;

struct offline_tmp_param {
	int valid_out_frame;
	int hw_ctx_id;
	uint32_t multi_slice;
	uint32_t target_fid;
	struct isp_stream_ctrl *stream;
};

static void free_offline_pararm(void *param)
{
	struct isp_offline_param *cur, *prev;

	cur = (struct isp_offline_param *)param;
	while (cur) {
		prev = (struct isp_offline_param *)cur->prev;
		pr_info("free %p\n", cur);
		kfree(cur);
		cur = prev;
	}
}

void isp_unmap_frame(void *param)
{
	struct camera_frame *frame;

	if (!param) {
		pr_err("fail to get input ptr.\n");
		return;
	}
	frame = (struct camera_frame *)param;
	if (frame->buf.mapping_state & CAM_BUF_MAPPING_DEV)
		cambuf_iommu_unmap(&frame->buf);
}

void isp_ret_out_frame(void *param)
{
	struct camera_frame *frame;
	struct isp_pipe_context *pctx;
	struct isp_path_desc *path;

	if (!param) {
		pr_err("fail to get input ptr.\n");
		return;
	}

	frame = (struct camera_frame *)param;
	path = (struct isp_path_desc *)frame->priv_data;
	if (!path) {
		pr_err("fail to get out_frame path.\n");
		return;
	}

	pr_debug("frame %p, ch_id %d, buf_fd %d\n",
		frame, frame->channel_id, frame->buf.mfd[0]);

	if (frame->is_reserved)
		camera_enqueue(
			&path->reserved_buf_queue, &frame->list);
	else {
		pctx = path->attach_ctx;
		if (frame->buf.mapping_state & CAM_BUF_MAPPING_DEV)
			cambuf_iommu_unmap(&frame->buf);
		pctx->isp_cb_func(
			ISP_CB_RET_DST_BUF,
			frame, pctx->cb_priv_data);
	}
}

void isp_ret_src_frame(void *param)
{
	struct camera_frame *frame;
	struct isp_pipe_context *pctx;

	if (!param) {
		pr_err("fail to get input ptr.\n");
		return;
	}

	frame = (struct camera_frame *)param;
	pctx = (struct isp_pipe_context *)frame->priv_data;
	if (!pctx) {
		pr_err("fail to get src_frame pctx.\n");
		return;
	}

	pr_debug("frame %p, ch_id %d, buf_fd %d\n",
		frame, frame->channel_id, frame->buf.mfd[0]);
	free_offline_pararm(frame->param_data);
	frame->param_data = NULL;
	cambuf_iommu_unmap(&frame->buf);
	pctx->isp_cb_func(
		ISP_CB_RET_SRC_BUF,
		frame, pctx->cb_priv_data);
}

void isp_destroy_reserved_buf(void *param)
{
	struct camera_frame *frame;

	if (!param) {
		pr_err("fail to get input ptr.\n");
		return;
	}

	frame = (struct camera_frame *)param;
	if (unlikely(frame->is_reserved == 0)) {
		pr_err("fail to get frame reserved buffer.\n");
		return;
	}
	/* is_reserved:
	 *  1:  basic mapping reserved buffer;
	 *  2:  copy of reserved buffer.
	 */
	if (frame->is_reserved == 1) {
		cambuf_iommu_unmap(&frame->buf);
		cambuf_put_ionbuf(&frame->buf);
	}
	put_empty_frame(frame);
}

void isp_destroy_statis_buf(void *param)
{
	struct camera_frame *frame;

	if (!param) {
		pr_err("fail to get input ptr.\n");
		return;
	}

	frame = (struct camera_frame *)param;
	put_empty_frame(frame);
}

int isp_adapt_blkparam(struct isp_pipe_context *pctx)
{
	uint32_t new_width, old_width;
	uint32_t new_height, old_height;
	uint32_t crop_start_x, crop_start_y;
	uint32_t crop_end_x, crop_end_y;
	struct img_trim *src_trim;
	struct img_size *dst = &pctx->original.dst_size;

	if (pctx->original.src_size.w > 0) {
		/* for input scaled image */
		src_trim = &pctx->original.src_trim;
		new_width = dst->w;
		new_height = dst->h;
		old_width = src_trim->size_x;
		old_height = src_trim->size_y;
	} else {
		src_trim = &pctx->input_trim;
		old_width = src_trim->size_x;
		old_height = src_trim->size_y;
		new_width = old_width;
		new_height = old_height;
	}
	crop_start_x = src_trim->start_x;
	crop_start_y = src_trim->start_y;
	crop_end_x = src_trim->start_x + src_trim->size_x - 1;
	crop_end_y = src_trim->start_y + src_trim->size_y - 1;

	pr_debug("crop %d %d %d %d, size(%d %d) => (%d %d)\n",
		crop_start_x, crop_start_y, crop_end_x, crop_end_y,
		old_width, old_height, new_width, new_height);

	isp_k_update_nlm(pctx->ctx_id, &pctx->isp_k_param,
		new_width, old_width, new_height, old_height);
	isp_k_update_ynr(pctx->ctx_id, &pctx->isp_k_param,
		new_width, old_width, new_height, old_height);
	isp_k_update_imbalance(pctx->ctx_id, &pctx->isp_k_param,
		new_width, old_width, new_height, old_height);

	if (pctx->mode_3dnr != MODE_3DNR_OFF)
		isp_k_update_3dnr(pctx->ctx_id, &pctx->isp_k_param,
			 new_width, old_width, new_height, old_height);

	return 0;
}

static int isp_update_hist_roi(struct isp_pipe_context *pctx)
{
	int ret = 0;
	uint32_t val;
	struct isp_dev_hist2_info hist2_info;
	struct isp_fetch_info *fetch = &pctx->fetch;

	pr_debug("sw %d, hist_roi w[%d] h[%d]\n",
		pctx->ctx_id, fetch->in_trim.size_x, fetch->in_trim.size_y);

	hist2_info.hist_roi.start_x = 0;
	hist2_info.hist_roi.start_y = 0;

	hist2_info.hist_roi.end_x = fetch->in_trim.size_x - 1;
	hist2_info.hist_roi.end_y = fetch->in_trim.size_y - 1;

	val = (hist2_info.hist_roi.start_y & 0xFFFF) | ((hist2_info.hist_roi.start_x & 0xFFFF) << 16);
	ISP_REG_WR(pctx->ctx_id, ISP_HIST2_ROI_S0, val);

	val = (hist2_info.hist_roi.end_y & 0xFFFF) | ((hist2_info.hist_roi.end_x & 0xFFFF) << 16);
	ISP_REG_WR(pctx->ctx_id, ISP_HIST2_ROI_E0, val);

	return ret;
}

static int isp_3dnr_process_frame(struct isp_pipe_context *pctx,
				  struct camera_frame *pframe)
{
	struct isp_3dnr_ctx_desc *nr3_ctx;

	struct dcam_frame_synchronizer *fsync = NULL;

	fsync = (struct dcam_frame_synchronizer *)pframe->sync_data;

	if (fsync) {
		pr_debug("id %u, valid %d, x %d, y %d, w %u, h %u\n",
			 fsync->index, fsync->nr3_me.valid,
			 fsync->nr3_me.mv_x, fsync->nr3_me.mv_y,
			 fsync->nr3_me.src_width, fsync->nr3_me.src_height);
	}
	nr3_ctx = &pctx->nr3_ctx;

	if (pctx->nr3_fbc_fbd) {
		nr3_ctx->nr3_store.st_bypass = 1;
		nr3_ctx->nr3_fbc_store.bypass = 0;
		nr3_ctx->mem_ctrl.nr3_ft_path_sel = 1;
	} else {
		nr3_ctx->nr3_store.st_bypass = 0;
		nr3_ctx->nr3_fbc_store.bypass = 1;
		nr3_ctx->mem_ctrl.nr3_ft_path_sel = 0;
	}

	/*  Check Zoom or not */
	if ((pctx->input_trim.size_x != pctx->nr3_ctx.width) ||
		(pctx->input_trim.size_y != pctx->nr3_ctx.height)) {
		pr_debug("isp %d frame size changed, reset 3dnr blending\n",
			pctx->ctx_id);
		pctx->nr3_ctx.blending_cnt = 0;
	}

	nr3_ctx->width  = pctx->input_trim.size_x;
	nr3_ctx->height = pctx->input_trim.size_y;

	pr_debug("isp %d nr3_type %d input.w[%d], input.h[%d], trim.w[%d], trim.h[%d]\n",
		pctx->ctx_id, pctx->mode_3dnr,
		pctx->input_size.w, pctx->input_size.h,
		pctx->input_trim.size_x, pctx->input_trim.size_y);

	switch (pctx->mode_3dnr) {
	case MODE_3DNR_PRE:
		nr3_ctx->type = NR3_FUNC_PRE;

		if (fsync && fsync->nr3_me.valid) {
			nr3_ctx->mv.mv_x = fsync->nr3_me.mv_x;
			nr3_ctx->mv.mv_y = fsync->nr3_me.mv_y;
			nr3_ctx->mvinfo = &fsync->nr3_me;

			isp_3dnr_conversion_mv(nr3_ctx);
		} else {
			pr_err("fail to get binning path mv, set default 0\n");
			nr3_ctx->mv.mv_x = 0;
			nr3_ctx->mv.mv_y = 0;
			nr3_ctx->mvinfo = NULL;
		}

		isp_3dnr_gen_config(nr3_ctx);
		isp_3dnr_config_param(nr3_ctx,
				      &pctx->isp_k_param,
				      pctx->ctx_id,
				      NR3_FUNC_PRE);

		pr_debug("MODE_3DNR_PRE frame type: %d from dcam binning path mv[%d, %d]!\n.",
			 pframe->channel_id,
			 nr3_ctx->mv.mv_x,
			 nr3_ctx->mv.mv_y);
		break;

	case MODE_3DNR_CAP:
		nr3_ctx->type = NR3_FUNC_CAP;

		if (fsync && fsync->nr3_me.valid) {
			nr3_ctx->mv.mv_x = fsync->nr3_me.mv_x;
			nr3_ctx->mv.mv_y = fsync->nr3_me.mv_y;
			if (pctx->input_size.w != pctx->input_trim.size_x ||
				pctx->input_size.h != pctx->input_trim.size_y) {
				nr3_ctx->mvinfo = &fsync->nr3_me;
				isp_3dnr_conversion_mv(nr3_ctx);
			}
		} else {
			pr_err("fail to get full path mv, set default 0\n");
			nr3_ctx->mv.mv_x = 0;
			nr3_ctx->mv.mv_y = 0;
		}
		if (nr3_ctx->mode == MODE_3DNR_OFF)
			return 0;

		isp_3dnr_gen_config(nr3_ctx);
		isp_3dnr_config_param(nr3_ctx,
				      &pctx->isp_k_param,
				      pctx->ctx_id,
				      NR3_FUNC_CAP);

		pr_debug("MODE_3DNR_CAP frame type: %d from dcam binning path mv[%d, %d]!\n.",
			 pframe->channel_id,
			 nr3_ctx->mv.mv_x,
			 nr3_ctx->mv.mv_y);
		break;

	case MODE_3DNR_OFF:
		/* default: bypass 3dnr */
		nr3_ctx->type = NR3_FUNC_OFF;
		isp_3dnr_bypass_config(pctx->ctx_id);
		pr_debug("isp_offline_start_frame MODE_3DNR_OFF\n");
		break;
	default:
		/* default: bypass 3dnr */
		nr3_ctx->type = NR3_FUNC_OFF;
		isp_3dnr_bypass_config(pctx->ctx_id);
		pr_debug("isp_offline_start_frame default\n");
		break;
	}

	if (fsync)
		dcam_if_release_sync(fsync, pframe);

	return 0;
}

static int isp_ltm_process_frame_previous(struct isp_pipe_context *pctx,
					  struct camera_frame *pframe)
{
	if (!pctx || !pframe) {
		pr_err("fail to get valid parameter pctx %p pframe %p\n",
			pctx, pframe);
		return -EINVAL;
	}

	/*
	 * Only preview path care of frame size changed
	 * Because capture path, USING hist from preview path
	 */
	if (pctx->mode_ltm != MODE_LTM_PRE)
		return 0;

	/*  Check Zoom or not */
	if ((pctx->input_trim.size_x != pctx->ltm_ctx.frame_width) ||
		(pctx->input_trim.size_y != pctx->ltm_ctx.frame_height) ||
		((pframe->fid - pctx->ltm_ctx.fid) != 1)||
		(pframe->fid == 0)){
		pr_debug("frame size changed or frame id not series , bypass ltm map\n");

		/* 1. hists from preview path always on
		 * 2. map will be off one time in preview case
		 */
		if (pctx->ltm_rgb)
			pctx->ltm_ctx.map[LTM_RGB].bypass = 1;
		if (pctx->ltm_yuv)
			pctx->ltm_ctx.map[LTM_YUV].bypass = 1;
	} else {
		if (pctx->ltm_rgb)
			pctx->ltm_ctx.map[LTM_RGB].bypass = 0;
		if (pctx->ltm_yuv)
			pctx->ltm_ctx.map[LTM_YUV].bypass = 0;
	}

	return 0;
}

static int isp_ltm_process_frame(struct isp_pipe_context *pctx,
				 struct camera_frame *pframe)
{
	int ret = 0;

	/* pre & cap */
	pctx->ltm_ctx.type = pctx->mode_ltm;
	pctx->ltm_ctx.fid = pframe->fid;
	pctx->ltm_ctx.frame_width  = pctx->input_trim.size_x;
	pctx->ltm_ctx.frame_height = pctx->input_trim.size_y;
	pctx->ltm_ctx.isp_pipe_ctx_id = pctx->ctx_id;

	pr_debug("LTM: type %d rgb %d yuv %d ctx id%d\n",
		pctx->ltm_ctx.type, pctx->ltm_rgb, pctx->ltm_yuv, pctx->ctx_id);

	/* pre & cap */
	if (pctx->ltm_rgb)
		ret = isp_ltm_gen_frame_config(&pctx->ltm_ctx, LTM_RGB,
			(struct isp_ltm_info *)&pctx->isp_k_param.rgb_ltm);
	if (ret == -1) {
		pctx->mode_ltm = MODE_LTM_OFF;
		pr_err("fail to rgb LTM cfg frame, DISABLE\n");
	}

	if (pctx->ltm_yuv)
		ret = isp_ltm_gen_frame_config(&pctx->ltm_ctx, LTM_YUV,
			(struct isp_ltm_info *)&pctx->isp_k_param.yuv_ltm);
	if (ret == -1) {
		pctx->mode_ltm = MODE_LTM_OFF;
		pr_err("fail to yuv LTM cfg frame, DISABLE\n");
	}

	pr_debug("type[%d], fid[%d], frame_width[%d], frame_height[%d], isp_pipe_ctx_id[%d]\n",
		pctx->ltm_ctx.type,
		pctx->ltm_ctx.fid,
		pctx->ltm_ctx.frame_width,
		pctx->ltm_ctx.frame_height,
		pctx->ltm_ctx.isp_pipe_ctx_id);

	pr_debug("frame_height_stat[%d], frame_width_stat[%d]",
		pctx->ltm_ctx.frame_height_stat,
		pctx->ltm_ctx.frame_width_stat);

	return ret;
}

static int isp_afbc_store(struct isp_path_desc *path)
{
	uint32_t w_tile_num = 0, h_tile_num = 0;
	uint32_t pad_width = 0, pad_height = 0;
	uint32_t header_size = 0, tile_data_size = 0;
	uint32_t header_addr = 0;
	struct isp_afbc_store_info *afbc_store_info = NULL;

	if (!path) {
		pr_err("fail to get valid input ptr\n");
		return -EFAULT;
	}

	afbc_store_info = &path->afbc_store;
	afbc_store_info->size.w = path->dst.w;
	afbc_store_info->size.h = path->dst.h;

	pad_width = (afbc_store_info->size.w + AFBC_PADDING_W_YUV420 - 1)
		/ AFBC_PADDING_W_YUV420 * AFBC_PADDING_W_YUV420;
	pad_height = (afbc_store_info->size.h + AFBC_PADDING_H_YUV420 - 1)
		/ AFBC_PADDING_H_YUV420 * AFBC_PADDING_H_YUV420;

	w_tile_num = pad_width / AFBC_PADDING_W_YUV420;
	h_tile_num = pad_height / AFBC_PADDING_H_YUV420;

	header_size = w_tile_num * h_tile_num * AFBC_HEADER_SIZE;
	tile_data_size = w_tile_num * h_tile_num * AFBC_PAYLOAD_SIZE;
	header_addr = afbc_store_info->yheader;

	afbc_store_info->header_offset = (header_size + 1024 - 1) / 1024 * 1024;
	afbc_store_info->yheader = header_addr;
	afbc_store_info->yaddr = afbc_store_info->yheader +
		afbc_store_info->header_offset;
	afbc_store_info->tile_number_pitch = w_tile_num;

	pr_debug("afbc w_tile_num = %d, h_tile_num = %d\n",
		w_tile_num,h_tile_num);
	pr_debug("afbc header_offset = %x, yheader = %x, yaddr = %x\n",
		afbc_store_info->header_offset,
		afbc_store_info->yheader,
		afbc_store_info->yaddr);

	return 0;
}

static int isp_ifbc_store(struct isp_path_desc *path)
{
	uint32_t pad_width = 0, pad_height = 0;
	uint32_t tile_hor = 0, tile_ver = 0;
	struct isp_ifbc_store_info *ifbc_store = NULL;

	if (!path) {
		pr_err("fail to get valid input ptr\n");
		return -EFAULT;
	}

	ifbc_store = &path->ifbc_store;
	ifbc_store->size_in_hor = path->dst.w;
	ifbc_store->size_in_ver = path->dst.h;

	pad_width = ifbc_store->size_in_hor;
	pad_height = ifbc_store->size_in_ver;

	if((pad_width % ISP_IFBC_Y_PAD_WIDTH) !=0 ||
		(pad_height % ISP_IFBC_Y_PAD_HEIGHT) !=0){
		pad_width = (pad_width + ISP_IFBC_Y_PAD_WIDTH - 1) /
			ISP_IFBC_Y_PAD_WIDTH * ISP_IFBC_Y_PAD_WIDTH;
		pad_height = (pad_height + ISP_IFBC_Y_PAD_HEIGHT - 1) /
			ISP_IFBC_Y_PAD_HEIGHT * ISP_IFBC_Y_PAD_HEIGHT;
	}

	tile_hor = pad_width / ISP_IFBC_Y_PAD_WIDTH;
	tile_ver = pad_height / ISP_IFBC_Y_PAD_HEIGHT;

	ifbc_store->tile_number = tile_hor * tile_ver + tile_hor * tile_ver / 2;
	ifbc_store->tile_number_pitch = pad_width / ISP_IFBC_Y_PAD_WIDTH;
	ifbc_store->fbc_constant_yuv = 0xff0000ff;
	ifbc_store->func_bits = 0xf;
	ifbc_store->slice_mode_en = 0;

	return 0;
}

static int isp_update_offline_size(
	struct isp_pipe_context *pctx,
	struct isp_offline_param *in_param)
{
	int ret = 0;
	int i;
	struct img_size *src_new = NULL;
	struct img_trim path_trim;
	struct isp_path_desc *path;
	struct isp_ctx_size_desc cfg;
	uint32_t update[ISP_SPATH_NUM] = {
			ISP_PATH0_TRIM, ISP_PATH1_TRIM, ISP_PATH2_TRIM};

	if(in_param->valid & ISP_SRC_SIZE) {
		memcpy(&pctx->original, &in_param->src_info, sizeof(pctx->original));
		cfg.src = in_param->src_info.dst_size;
		cfg.crop.start_x = 0;
		cfg.crop.start_y = 0;
		cfg.crop.size_x = cfg.src.w;
		cfg.crop.size_y = cfg.src.h;
		pctx->src_info.src = cfg.src;
		pctx->src_info.src_crop = cfg.crop;
		pr_debug("isp sw %d update size: %d %d\n",
			pctx->ctx_id, cfg.src.w, cfg.src.h);
		src_new = &cfg.src;
	}

	/* update all path scaler trim0  */
	for (i = 0; i < ISP_SPATH_NUM; i++) {
		path = &pctx->isp_path[i];
		if (atomic_read(&path->user_cnt) < 1)
			continue;

		if (in_param->valid & update[i]) {
			path_trim = in_param->trim_path[i];
		} else if (src_new) {
			path_trim.start_x = path_trim.start_y = 0;
			path_trim.size_x = src_new->w;
			path_trim.size_y = src_new->h;
		} else {
			continue;
		}

		path->stream_in_trim = path_trim;
		pr_debug("update isp path%d trim %d %d %d %d\n",
			i, path_trim.start_x, path_trim.start_y,
			path_trim.size_x, path_trim.size_y);
	}
	pctx->updated = 1;
	return ret;
}

static int set_fmcu_slw_queue(
	struct isp_fmcu_ctx_desc *fmcu,
	struct isp_pipe_context *pctx)
{
	int ret = 0, i;
	uint32_t frame_id;
	struct isp_path_desc *path;
	struct camera_frame *pframe = NULL;
	struct camera_frame *out_frame = NULL;

	if (!fmcu)
		return -EINVAL;

	pframe = camera_dequeue(&pctx->in_queue, struct camera_frame, list);
	if (pframe == NULL) {
		pr_err("fail to get frame from input queue. cxt:%d\n", pctx->ctx_id);
		return -EINVAL;
	}

	ret = cambuf_iommu_map(&pframe->buf, CAM_IOMMUDEV_ISP);
	if (ret) {
		pr_err("fail to map buf to ISP iommu. cxt %d\n", pctx->ctx_id);
		ret = -EINVAL;
	}

	ret = camera_enqueue(&pctx->proc_queue, &pframe->list);
	if (ret) {
		pr_err("fail to input frame queue, timeout.\n");
		ret = -EINVAL;
	}

	pframe->width = pctx->input_size.w;
	pframe->height = pctx->input_size.h;
	frame_id = pframe->fid;
	isp_path_set_fetch_frm(pctx, pframe);

	for (i = 0; i < ISP_SPATH_NUM; i++) {
		path = &pctx->isp_path[i];
		if (atomic_read(&path->user_cnt) < 1)
			continue;

		if (i == ISP_SPATH_VID)
			out_frame = camera_dequeue(&path->out_buf_queue,
				struct camera_frame, list);
		if (out_frame == NULL)
			out_frame = camera_dequeue(&path->reserved_buf_queue,
				struct camera_frame, list);

		if (out_frame == NULL) {
			pr_debug("fail to get available output buffer.\n");
			return -EINVAL;
		}

		if (out_frame->is_reserved == 0) {
			ret = cambuf_iommu_map(
					&out_frame->buf, CAM_IOMMUDEV_ISP);
			pr_debug("map output buffer %08x\n", (uint32_t)out_frame->buf.iova[0]);
			if (ret) {
				camera_enqueue(&path->out_buf_queue, &out_frame->list);
				out_frame = NULL;
				pr_err("fail to map isp iommu buf.\n");
				return -EINVAL;
			}
		}

		out_frame->fid = frame_id;
		out_frame->sensor_time = pframe->sensor_time;
		out_frame->boot_sensor_time = pframe->boot_sensor_time;

		pr_debug("isp output buf, iova 0x%x, phy: 0x%x\n",
				(uint32_t)out_frame->buf.iova[0],
				(uint32_t)out_frame->buf.addr_k[0]);
		isp_path_set_store_frm(path, out_frame);
		if ((i < FBC_PATH_NUM) && (path->afbc_store.bypass == 0)) {
			isp_path_set_afbc_store_frm(path, out_frame);
		}
		if ((i < FBC_PATH_NUM) && (path->ifbc_store.bypass == 0)) {
			isp_path_set_ifbc_store_frm(path, out_frame);
		}
		ret = camera_enqueue(&path->result_queue, &out_frame->list);
		if (ret) {
			if (out_frame->is_reserved)
				camera_enqueue(&path->reserved_buf_queue,
						&out_frame->list);
			else {
				cambuf_iommu_unmap(&out_frame->buf);
				camera_enqueue(&path->out_buf_queue,
						&out_frame->list);
			}
			return -EINVAL;
		}
		atomic_inc(&path->store_cnt);
	}

	ret = isp_set_slw_fmcu_cmds((void *)fmcu, pctx);

	pr_debug("fmcu slw queue done!");
	return ret;
}

static int proc_slices(struct isp_pipe_context *pctx)
{
	int ret = 0;
	int hw_ctx_id = -1;
	uint32_t slice_id, cnt = 0;
	struct isp_pipe_dev *dev = NULL;
	struct isp_cfg_ctx_desc *cfg_desc = NULL;
	struct cam_hw_info *hw = NULL;

	pr_debug("enter. need_slice %d\n", pctx->multi_slice);
	hw_ctx_id = isp_get_hw_context_id(pctx);
	if (hw_ctx_id < 0) {
		pr_err("fail to get valid hw for ctx %d\n", pctx->ctx_id);
		return -EINVAL;
	}

	dev = pctx->dev;
	cfg_desc = (struct isp_cfg_ctx_desc *)dev->cfg_handle;
	pctx->is_last_slice = 0;
	hw = pctx->hw;
	for (slice_id = 0; slice_id < SLICE_NUM_MAX; slice_id++) {
		if (pctx->is_last_slice == 1)
			break;

		ret = isp_update_slice(pctx, pctx->ctx_id, slice_id);
		if (ret < 0)
			continue;

		cnt++;
		if (cnt == pctx->valid_slc_num)
			pctx->is_last_slice = 1;
		pr_debug("slice %d, valid %d, last %d\n", slice_id,
			pctx->valid_slc_num, pctx->is_last_slice);

		pctx->started = 1;
		mutex_lock(&pctx->blkpm_lock);
		if (pctx->in_fmt == IMG_PIX_FMT_NV21) {
			hw->hw_ops.core_ops.isp_yuv_block_ctrl(pctx->ctx_id,
				&pctx->isp_k_param, ISP_YUV_BLOCK_DISABLE);
		} else {
			hw->hw_ops.core_ops.isp_yuv_block_ctrl(pctx->ctx_id,
				&pctx->isp_k_param, ISP_YUV_BLOCK_CFG);
		}
		ret = cfg_desc->ops->hw_cfg(
				cfg_desc, pctx->ctx_id, hw_ctx_id, 0);
		mutex_unlock(&pctx->blkpm_lock);
		ret = cfg_desc->ops->hw_start(
					cfg_desc, hw_ctx_id);
		ret = wait_for_completion_interruptible_timeout(
					&pctx->slice_done,
					ISP_CONTEXT_TIMEOUT);
		if (ret == ERESTARTSYS) {
			pr_err("fail to interrupt, when isp wait\n");
			ret = -EFAULT;
			goto exit;
		} else if (ret == 0) {
			pr_err("fail to wait isp context %d, timeout.\n", pctx->ctx_id);
			ret = -EFAULT;
			goto exit;
		}
		pr_debug("slice %d done\n", slice_id);
	}
exit:
	return ret;
}

static uint32_t isp_get_fid_across_context(struct isp_pipe_dev *dev, enum camera_id cam_id)
{
	struct isp_pipe_context *ctx;
	struct isp_path_desc *path;
	struct camera_frame *frame;
	uint32_t target_fid;
	int ctx_id, path_id;

	if (!dev)
		return CAMERA_RESERVE_FRAME_NUM;

	target_fid = CAMERA_RESERVE_FRAME_NUM;
	for (ctx_id = 0; ctx_id < ISP_CONTEXT_SW_NUM; ctx_id++) {
		ctx = &dev->ctx[ctx_id];
		if (!ctx || atomic_read(&ctx->user_cnt) < 1 || (ctx->attach_cam_id != cam_id))
			continue;

		for (path_id = 0; path_id < ISP_SPATH_NUM; path_id++) {
			path = &ctx->isp_path[path_id];
			if (!path || atomic_read(&path->user_cnt) < 1
			    || !path->uframe_sync)
				continue;

			frame = camera_dequeue_peek(&path->out_buf_queue,
				struct camera_frame, list);
			if (!frame)
				continue;

			target_fid = min(target_fid, frame->user_fid);
			pr_debug("ISP%d path%d user_fid %u\n",
				 ctx_id, path_id, frame->user_fid);
		}
	}

	pr_debug("target_fid %u, cam_id = %d\n", target_fid, cam_id);

	return target_fid;
}

static bool isp_check_fid(struct camera_frame *frame, void *data)
{
	uint32_t target_fid;

	if (!frame || !data)
		return false;

	target_fid = *(uint32_t *)data;

	pr_debug("target_fid = %d frame->user_fid = %d\n", target_fid, frame->user_fid);
	return frame->user_fid == CAMERA_RESERVE_FRAME_NUM
		|| frame->user_fid == target_fid;
}

int isp_get_hw_context_id(struct isp_pipe_context *pctx)
{
	int i;
	int hw_ctx_id = -1;
	struct isp_pipe_dev *dev = NULL;
	struct isp_pipe_hw_context *pctx_hw;

	dev = pctx->dev;

	for (i = 0; i < ISP_CONTEXT_HW_NUM; i++) {
		pctx_hw = &dev->hw_ctx[i];

		if ((pctx->ctx_id == pctx_hw->sw_ctx_id)
			&& (pctx == pctx_hw->pctx)) {
			hw_ctx_id = pctx_hw->hw_ctx_id;
			break;
		}
	}
	pr_debug("get hw %d\n", hw_ctx_id);

	return hw_ctx_id;
}

int isp_get_sw_context_id(enum isp_context_hw_id hw_ctx_id, struct isp_pipe_dev *dev)
{
	int sw_id = -1;
	struct isp_pipe_hw_context *pctx_hw;

	if (hw_ctx_id < ISP_CONTEXT_HW_NUM) {
		pctx_hw = &dev->hw_ctx[hw_ctx_id];
		sw_id = pctx_hw->sw_ctx_id;
		if (sw_id >= ISP_CONTEXT_P0 &&
			sw_id < ISP_CONTEXT_SW_NUM &&
			pctx_hw->pctx == &dev->ctx[sw_id]) {
				pr_debug("get sw %d\n", sw_id);
				return sw_id;
			}
	}

	return -1;
}

int isp_context_bind(struct isp_pipe_context *pctx, int fmcu_need)
{
	int i = 0,  m = 0, loop;
	int hw_ctx_id = -1;
	unsigned long flag = 0;
	struct isp_pipe_dev *dev = NULL;
	struct isp_pipe_hw_context *pctx_hw;

	dev = pctx->dev;
	spin_lock_irqsave(&dev->ctx_lock, flag);

	for (i = 0; i < ISP_CONTEXT_HW_NUM; i++) {
		pctx_hw = &dev->hw_ctx[i];
		if (pctx->ctx_id == pctx_hw->sw_ctx_id
			&& pctx_hw->pctx == pctx) {
			atomic_inc(&pctx_hw->user_cnt);
			pr_debug("sw %d & hw %d already binding, cnt=%d\n",
				pctx->ctx_id, i, atomic_read(&pctx_hw->user_cnt));
			spin_unlock_irqrestore(&dev->ctx_lock, flag);
			return 0;
		}
	}

	loop = (fmcu_need & FMCU_IS_MUST) ? 1 : 2;
	for (m = 0; m < loop; m++) {
		for (i = 0; i < ISP_CONTEXT_HW_NUM; i++) {
			pctx_hw = &dev->hw_ctx[i];
			if (m == 0) {
				/* first pass we just select fmcu/no-fmcu context */
				if ((!fmcu_need && pctx_hw->fmcu_handle) ||
					(fmcu_need && !pctx_hw->fmcu_handle))
					continue;
			}

			if (atomic_inc_return(&pctx_hw->user_cnt) == 1) {
				hw_ctx_id = pctx_hw->hw_ctx_id;
				goto exit;
			}
			atomic_dec(&pctx_hw->user_cnt);
		}
	}

	/* force fmcu used, we will retry */
	if (fmcu_need & FMCU_IS_MUST)
		goto exit;

exit:
	spin_unlock_irqrestore(&dev->ctx_lock, flag);

	if (hw_ctx_id == -1) {
		return -1;
	}

	pctx_hw->pctx = pctx;
	pctx_hw->sw_ctx_id = pctx->ctx_id;
	pr_debug("sw %d, hw %d %d, fmcu_need %d ptr 0x%lx\n",
		pctx->ctx_id, hw_ctx_id, pctx_hw->hw_ctx_id,
		fmcu_need, (unsigned long)pctx_hw->fmcu_handle);

	return 0;
}

int isp_context_unbind(struct isp_pipe_context *pctx)
{
	int i, cnt;
	struct isp_pipe_dev *dev = NULL;
	struct isp_pipe_hw_context *pctx_hw;
	unsigned long flag = 0;

	if (isp_get_hw_context_id(pctx) < 0) {
		pr_err("fail to binding sw ctx %d to any hw ctx\n", pctx->ctx_id);
		return -EINVAL;
	}

	dev = pctx->dev;
	spin_lock_irqsave(&dev->ctx_lock, flag);

	for (i = 0; i < ISP_CONTEXT_HW_NUM; i++) {
		pctx_hw = &dev->hw_ctx[i];
		if ((pctx->ctx_id != pctx_hw->sw_ctx_id) ||
			(pctx != pctx_hw->pctx))
			continue;

		if (atomic_dec_return(&pctx_hw->user_cnt) == 0) {
			pr_debug("sw_id=%d, hw_id=%d unbind success\n",
				pctx->ctx_id, pctx_hw->hw_ctx_id);
			pctx_hw->pctx = NULL;
			pctx_hw->sw_ctx_id = -1;
			goto exit;
		}

		cnt = atomic_read(&pctx_hw->user_cnt);
		if (cnt >= 1) {
			pr_debug("sw id=%d, hw_id=%d, cnt=%d\n",
				pctx->ctx_id, pctx_hw->hw_ctx_id, cnt);
		} else {
			pr_debug("should not be here: sw id=%d, hw id=%d, cnt=%d\n",
				pctx->ctx_id, pctx_hw->hw_ctx_id, cnt);
			spin_unlock_irqrestore(&dev->ctx_lock, flag);
			return -EINVAL;
		}
	}

exit:
	spin_unlock_irqrestore(&dev->ctx_lock, flag);
	return 0;
}

static int isp_slice_needed(struct isp_pipe_context *pctx)
{
	int i;
	struct isp_path_desc *path;

	if (pctx->input_trim.size_x > g_camctrl.isp_linebuf_len)
		return 1;

	for (i = 0; i < ISP_SPATH_NUM; i++) {
		path = &pctx->isp_path[i];
		if (atomic_read(&path->user_cnt) < 1)
			continue;
		if (path->dst.w > g_camctrl.isp_linebuf_len)
			return 1;
	}
	return 0;
}

static void isp_sw_slice_prepare(struct isp_pipe_context *pctx,
					   struct camera_frame *pframe)
{
	struct isp_fetch_info *fetch = &pctx->fetch;
	struct isp_store_info *store = NULL;
	uint32_t mipi_byte_info = 0;
	uint32_t mipi_word_info = 0;
	uint32_t start_col = 0;
	uint32_t end_col = pframe->slice_trim.size_x - 1;
	uint32_t mipi_word_num_start[16] = {
		0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5};
	uint32_t mipi_word_num_end[16] = {
		0, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5};
	struct img_size src;
	uint32_t slice_num = 0;
	uint32_t slice_no = 0;
	uint32_t first_slice = 0;
	struct isp_path_desc *path;

	pctx->sw_slice_num = pframe->sw_slice_num;
	pctx->sw_slice_no = pframe->sw_slice_no;
	slice_num = pctx->sw_slice_num;
	slice_no = pctx->sw_slice_no;
	first_slice = (slice_no != 0) ? 0 : 1;

	mipi_byte_info = start_col & 0xF;
	mipi_word_info =
		((end_col + 1) >> 4) * 5
		+ mipi_word_num_end[(end_col + 1) & 0xF]
		- ((start_col + 1) >> 4) * 5
		- mipi_word_num_start[(start_col + 1) & 0xF] + 1;

	fetch->mipi_byte_rel_pos = 0;
	fetch->mipi_word_num = mipi_word_info;
	fetch->pitch.pitch_ch0 = cal_sprd_raw_pitch(pframe->slice_trim.size_x, 0);
	fetch->pitch.pitch_ch1 = 0;
	fetch->pitch.pitch_ch2 = 0;

	path = &pctx->isp_path[ISP_SPATH_CP];
	store = &path->store;

	if (first_slice) {
		fetch->in_trim.size_x = fetch->src.w = pframe->slice_trim.size_x;
		fetch->in_trim.size_y = fetch->src.h = pframe->slice_trim.size_y;

		pctx->input_trim.start_x = 0;
		pctx->input_trim.start_y = 0;
		src.w = pctx->input_trim.size_x = fetch->in_trim.size_x;
		src.h = pctx->input_trim.size_y = fetch->in_trim.size_y;
		pctx->input_size = src;
		path->in_trim = pctx->input_trim;
		path->src = src;

		store->pitch.pitch_ch0 = store->size.w;
		store->pitch.pitch_ch1 = store->size.w;
		if (slice_num > SLICE_NUM_MAX) {
			store->size.w = ALIGN(store->size.w / (slice_num / 2), 2);
			store->size.h = ALIGN(store->size.h / 2, 2);
		} else
			store->size.w = ALIGN(store->size.w/ slice_num, 2);
		path->out_trim.start_x = 0;
		path->out_trim.start_y = 0;
		path->out_trim.size_x = path->dst.w = store->size.w;
		path->out_trim.size_y = path->dst.h = store->size.h;
		isp_cfg_path_scaler(path);
	}

	if (slice_num > SLICE_NUM_MAX) {
		fetch->trim_off.addr_ch0 =
			(slice_no / (slice_num / 2)) * fetch->pitch.pitch_ch0 * fetch->src.h;
		store->slice_offset.addr_ch0 = path->dst.w * (slice_no % (slice_num / 2))
			+ (slice_no / (slice_num / 2)) * store->pitch.pitch_ch0 * store->size.h;
		store->slice_offset.addr_ch1 = path->dst.w * (slice_no % (slice_num / 2))
			+ (slice_no / (slice_num / 2)) * store->pitch.pitch_ch1 * store->size.h / 2;
		store->slice_offset.addr_ch2 = 0;
	} else {
		fetch->trim_off.addr_ch0 = 0;
		store->slice_offset.addr_ch0 = path->dst.w * slice_no;
		store->slice_offset.addr_ch1 = path->dst.w * slice_no;
		store->slice_offset.addr_ch2 = 0;
	}
}

static struct camera_frame * isp_get_path_out_frame(
	struct isp_pipe_context *pctx, struct isp_path_desc *path,
	struct offline_tmp_param *tmp)
{
	int ret = 0;
	int j = 0;
	uint32_t buf_type = 0;
	struct camera_frame *out_frame = NULL;

	if (!pctx || !path || !tmp) {
		pr_err("fail to get valid input pctx %p, path %p\n", pctx, path);
		return NULL;
	}

	if (tmp->stream) {
		buf_type = tmp->stream->buf_type[path->spath_id];
		switch (buf_type) {
		case ISP_STREAM_BUF_OUT:
			goto normal_out_put;
		case ISP_STREAM_BUF_RESERVED:
			out_frame = camera_dequeue(&path->reserved_buf_queue,
				struct camera_frame, list);
			tmp->valid_out_frame = 1;
			pr_debug("reserved buffer %d %lx\n",
				out_frame->is_reserved, out_frame->buf.iova[0]);
			break;
		case ISP_STREAM_BUF_POSTPROC:
			out_frame = pctx->postproc_buf;
			cambuf_iommu_map(&out_frame->buf, CAM_IOMMUDEV_ISP);
			tmp->valid_out_frame = 1;
			break;
		case ISP_STREAM_BUF_RESULT:
			out_frame = camera_dequeue(&path->result_queue,
					struct camera_frame, list);
			tmp->valid_out_frame = 1;
			break;
		default:
			pr_err("fail to support buf_type %d\n", tmp->stream->buf_type);
			break;
		}
		goto exit;
	}

normal_out_put:
	if (pctx->sw_slice_num && pctx->sw_slice_no != 0) {
		out_frame = camera_dequeue(&path->result_queue,
					struct camera_frame, list);
	} else {
		if (path->uframe_sync
			&& tmp->target_fid != CAMERA_RESERVE_FRAME_NUM)
			out_frame = camera_dequeue_if(&path->out_buf_queue,
				isp_check_fid, (void *)&tmp->target_fid);
		else
			out_frame = camera_dequeue(&path->out_buf_queue,
				struct camera_frame, list);
	}

	if (out_frame)
		tmp->valid_out_frame = 1;
	else
		out_frame = camera_dequeue(&path->reserved_buf_queue,
			struct camera_frame, list);

	if (out_frame != NULL) {
		if (out_frame->is_reserved == 0 &&
			(out_frame->buf.mapping_state & CAM_BUF_MAPPING_DEV) == 0) {
			if (out_frame->buf.mfd[0] == path->reserved_buf_fd) {
				for (j = 0; j < 3; j++) {
					out_frame->buf.size[j] = path->reserve_buf_size[j];
					pr_debug("out_frame->buf.size[j] = %d, path->reserve_buf_size[j] = %d",
						(int)out_frame->buf.size[j], (int)path->reserve_buf_size[j]);
				}
			}
			ret = cambuf_iommu_map(
					&out_frame->buf, CAM_IOMMUDEV_ISP);
			pr_debug("map output buffer %08x\n", (uint32_t)out_frame->buf.iova[0]);
			if (ret) {
				camera_enqueue(&path->out_buf_queue, &out_frame->list);
				out_frame = NULL;
				pr_err("fail to map isp iommu buf.\n");
			}
		}
	}

exit:
	return out_frame;
}

static int isp_cfg_offline_param(struct isp_pipe_context *pctx,
	struct camera_frame *pframe, struct offline_tmp_param *tmp)
{
	int ret = 0;
	int i = 0;
	struct isp_stream_ctrl *stream = NULL;
	struct isp_path_desc *path = NULL;
	struct isp_ctx_size_desc cfg;
	struct img_trim path_trim;

	if (!pctx || !pframe || !tmp) {
		pr_err("fail to get input ptr, pctx %p, pframe %p tmp %p\n",
			pctx, pframe, tmp);
		return -EFAULT;
	}

	stream = camera_dequeue(&pctx->stream_ctrl_in_q,
		struct isp_stream_ctrl, list);
	tmp->stream = stream;
	if (stream) {
		pctx->in_fmt = stream->in_fmt;
		cfg.src = stream->in;
		cfg.crop = stream->in_crop;
		isp_cfg_ctx_size(pctx, &cfg);
		pr_debug("isp %d in_size %d %d crop_szie %d %d %d %d\n",
			pctx->ctx_id, stream->in.w, stream->in.h,
			stream->in_crop.start_x, stream->in_crop.start_y,
			stream->in_crop.size_x, stream->in_crop.size_y);
		for (i = 0; i < ISP_SPATH_NUM; i++) {
			path = &pctx->isp_path[i];
			if (atomic_read(&path->user_cnt) < 1)
				continue;
			path->dst = stream->out[i];
			path_trim = stream->out_crop[i];
			isp_cfg_path_size(path, &path_trim);
			pr_debug("isp %d out_size %d %d crop_szie %d %d %d %d\n",
				pctx->ctx_id, stream->out[i].w, stream->out[i].h,
				stream->out_crop[i].start_x, stream->out_crop[i].start_y,
				stream->out_crop[i].size_x, stream->out_crop[i].size_y);
		}
		if (pctx->mode_3dnr == MODE_3DNR_CAP) {
			pctx->nr3_ctx.blending_cnt =
				stream->cur_cnt % NR3_BLEND_CNT;
			if (stream->data_src == ISP_STREAM_SRC_ISP)
				pctx->nr3_ctx.mode = MODE_3DNR_OFF;
			else
				pctx->nr3_ctx.mode = MODE_3DNR_CAP;
		}
		pctx->updated = 1;
	}

	pr_debug("isp %d 3dnr blend cnt %d\n", pctx->ch_id, pctx->nr3_ctx.blending_cnt);
	if (pframe->sw_slice_num) {
		isp_sw_slice_prepare(pctx, pframe);
	}
	isp_update_hist_roi(pctx);
	/*update NR param for crop/scaling image */
	isp_adapt_blkparam(pctx);
	/* the context/path maybe init/updated after dev start. */
	if (pctx->updated || pctx->sw_slice_num)
		ret = isp_slice_ctx_init(pctx, &tmp->multi_slice);
	if (pctx->uframe_sync)
		tmp->target_fid = isp_get_fid_across_context(pctx->dev,
			pctx->attach_cam_id);

	return ret;
}

static int isp_set_offline_param(struct isp_pipe_context *pctx,
	struct camera_frame *pframe, struct offline_tmp_param *tmp)
{
	int ret = 0;
	int i = 0, loop = 0;
	struct isp_path_desc *path = NULL;
	struct isp_path_desc *slave_path = NULL;
	struct isp_pipe_dev *dev = NULL;
	struct camera_frame *out_frame = NULL;
	struct cam_hw_info *hw = NULL;

	if (!pctx || !pframe || !tmp) {
		pr_err("fail to get input ptr, pctx %p, pframe %p tmp %p\n",
			pctx, pframe, tmp);
		return -EFAULT;
	}

	dev = pctx->dev;
	hw = dev->isp_hw;

	/* config fetch address */
	isp_path_set_fetch_frm(pctx, pframe);
	if (pctx->updated || pctx->sw_slice_num) {
		hw->hw_ops.core_ops.isp_fetch_set(pctx);
		if (pctx->in_fmt == IMG_PIX_FMT_NV21)
			pctx->hw->hw_ops.core_ops.isp_cfg_subblock(pctx);
	}

	/* config all paths output */
	for (i = 0; i < ISP_SPATH_NUM; i++) {
		path = &pctx->isp_path[i];
		if (atomic_read(&path->user_cnt) < 1)
			continue;

		if ((i < FBC_PATH_NUM) && (path->afbc_store.bypass == 0))
			ret = isp_afbc_store(path);
		if ((i < FBC_PATH_NUM) && (path->ifbc_store.bypass == 0))
			ret = isp_ifbc_store(path);

		if (pctx->updated || pctx->sw_slice_num)
			ret = isp_set_path(path);

		/* slave path output buffer binding to master buffer*/
		if (path->bind_type == ISP_PATH_SLAVE)
			continue;
		out_frame = isp_get_path_out_frame(pctx, path, tmp);
		if (out_frame) {
			out_frame->fid = pframe->fid;
			out_frame->sensor_time = pframe->sensor_time;
			out_frame->boot_sensor_time = pframe->boot_sensor_time;
		} else {
			ret = -EINVAL;
			goto exit;
		}

		/* config store buffer */
		pr_debug("isp %d is_reserved %d iova 0x%x, user_fid: %x mfd %x\n",
			pctx->ctx_id, out_frame->is_reserved,
			(uint32_t)out_frame->buf.iova[0], out_frame->user_fid,
			out_frame->buf.mfd[0]);
		ret = isp_path_set_store_frm(path, out_frame);
		/* If some error comes then do not start ISP */
		if (ret) {
			cambuf_iommu_unmap(&out_frame->buf);
			cambuf_put_ionbuf(&out_frame->buf);
			put_empty_frame(out_frame);
			ret = -EINVAL;
			goto exit;
		}
		if ((i < FBC_PATH_NUM) && (path->afbc_store.bypass == 0))
			isp_path_set_afbc_store_frm(path, out_frame);
		if ((i < FBC_PATH_NUM) && (path->ifbc_store.bypass == 0))
			isp_path_set_ifbc_store_frm(path, out_frame);

		if (path->bind_type == ISP_PATH_MASTER) {
			struct camera_frame temp;
			/* fixed buffer offset here. HAL should use same offset calculation method */
			temp.buf.iova[0] = out_frame->buf.iova[0] + path->store.total_size;
			temp.buf.iova[1] = temp.buf.iova[2] = 0;
			slave_path = &pctx->isp_path[path->slave_path_id];
			isp_path_set_store_frm(slave_path, &temp);
		}
		/*
		 * context proc_queue frame number
		 * should be equal to path result queue.
		 * if ctx->proc_queue enqueue OK,
		 * path result_queue enqueue should be OK.
		 */
		loop = 0;
		do {
			ret = camera_enqueue(&path->result_queue, &out_frame->list);
			if (ret == 0)
				break;
			printk_ratelimited(KERN_INFO "wait for output queue. loop %d\n", loop);
			/* wait for previous frame output queue done */
			mdelay(1);
		} while (loop++ < 500);

		if (ret) {
			trace_isp_irq_cnt(tmp->hw_ctx_id);
			pr_err("fail to enqueue, hw %d, path %d, store %d\n",
					tmp->hw_ctx_id, path->spath_id,
					atomic_read(&path->store_cnt));
			/* ret frame to original queue */
			if (out_frame->is_reserved) {
				camera_enqueue(
					&path->reserved_buf_queue, &out_frame->list);
				dev->ltm_handle->ops->clear_status(pctx->ltm_ctx.ltm_index);
			} else {
				cambuf_iommu_unmap(&out_frame->buf);
				camera_enqueue(
					&path->out_buf_queue, &out_frame->list);
			}
			ret = -EINVAL;
			goto exit;
		}
		atomic_inc(&path->store_cnt);
	}

exit:
	return ret;
}

static int isp_offline_start_frame(void *ctx)
{
	int ret = 0;
	int i = 0, loop = 0, kick_fmcu = 0, slc_by_ap = 0;
	int hw_ctx_id = -1;
	uint32_t use_fmcu = 0;
	uint32_t frame_id;
	struct isp_pipe_dev *dev = NULL;
	struct camera_frame *pframe = NULL;
	struct isp_pipe_context *pctx = NULL;
	struct isp_pipe_hw_context *pctx_hw = NULL;
	struct isp_path_desc *path;
	struct isp_cfg_ctx_desc *cfg_desc = NULL;
	struct isp_fmcu_ctx_desc *fmcu = NULL;
	struct cam_hw_info *hw = NULL;
	struct offline_tmp_param tmp = {0};

	pctx = (struct isp_pipe_context *)ctx;
	pr_debug("enter sw id %d, user_cnt=%d, ch_id=%d, cam_id=%d\n",
		pctx->ctx_id, atomic_read(&pctx->user_cnt),
		pctx->ch_id, pctx->attach_cam_id);

	if (atomic_read(&pctx->user_cnt) < 1) {
		pr_err("fail to init isp cxt %d.\n", pctx->ctx_id);
		return -EINVAL;
	}

	dev = pctx->dev;
	hw = dev->isp_hw;
	cfg_desc = (struct isp_cfg_ctx_desc *)dev->cfg_handle;

	if (pctx->multi_slice | isp_slice_needed(pctx))
		use_fmcu = FMCU_IS_NEED;
	if (use_fmcu && (pctx->mode_3dnr != MODE_3DNR_OFF))
		use_fmcu |= FMCU_IS_MUST; /* force fmcu used*/
	if (pctx->enable_slowmotion)
		use_fmcu |= FMCU_IS_MUST; // force fmcu used
	if (pctx->sw_slice_num)
		use_fmcu = FMCU_IS_NEED;

	loop = 0;
	do {
		ret = isp_context_bind(pctx, use_fmcu);
		if (!ret) {
			hw_ctx_id = isp_get_hw_context_id(pctx);
			if (hw_ctx_id >= 0 && hw_ctx_id < ISP_CONTEXT_HW_NUM)
				pctx_hw = &dev->hw_ctx[hw_ctx_id];
			else
				pr_err("fail to get hw_ctx_id\n");
			break;
		}
		pr_info_ratelimited("ctx %d wait for hw. loop %d\n", pctx->ctx_id, loop);
		usleep_range(600, 2000);
	} while (loop++ < 5000);

	pframe = camera_dequeue(&pctx->in_queue, struct camera_frame, list);

	if (!pctx_hw || pframe == NULL) {
		pr_err("fail to get hw(%p) or input frame (%p) for ctx %d\n",
			pctx_hw, pframe, pctx->ctx_id);
		ret = 0;
		goto input_err;
	}

	if ((pframe->fid & 0x1f) == 0)
		pr_info(" sw %d, cam%d , fid %d, ch_id %d, buf_fd %d\n",
			pctx->ctx_id, pctx->attach_cam_id,
			pframe->fid, pframe->channel_id, pframe->buf.mfd[0]);

	ret = cambuf_iommu_map(&pframe->buf, CAM_IOMMUDEV_ISP);
	if (ret) {
		pr_err("fail to map buf to ISP iommu. cxt %d\n",
			pctx->ctx_id);
		ret = -EINVAL;
		goto map_err;
	}

	frame_id = pframe->fid;
	loop = 0;
	do {
		ret = camera_enqueue(&pctx->proc_queue, &pframe->list);
		if (ret == 0)
			break;
		pr_info_ratelimited("wait for proc queue. loop %d\n", loop);
		/* wait for previous frame proccessed done.*/
		usleep_range(600, 2000);
	} while (loop++ < 500);

	if (ret) {
		pr_err("fail to input frame queue, timeout.\n");
		ret = -EINVAL;
		goto inq_overflow;
	}

	/*
	 * param_mutex to avoid ctx/all paths param
	 * updated when set to register.
	 */
	mutex_lock(&pctx->param_mutex);
	tmp.multi_slice = pctx->multi_slice;
	tmp.valid_out_frame = -1;
	tmp.target_fid = CAMERA_RESERVE_FRAME_NUM;
	tmp.hw_ctx_id = hw_ctx_id;
	tmp.stream = NULL;
	ret = isp_cfg_offline_param(pctx, pframe, &tmp);
	if (ret) {
		pr_err("fail to cfg offline param.\n");
		ret = 0;
		goto unlock;
	}

	ret = isp_set_offline_param(pctx, pframe, &tmp);
	if (ret) {
		pr_err("fail to set offline param.\n");
		ret = 0;
		goto unlock;
	}

	if (tmp.valid_out_frame == -1) {
		pr_debug(" No available output buffer sw %d, hw %d,discard\n",
			pctx_hw->sw_ctx_id, pctx_hw->hw_ctx_id);
		dev->ltm_handle->ops->clear_status(pctx->ltm_ctx.ltm_index);
		goto unlock;
	}

	isp_ltm_process_frame_previous(pctx, pframe);
	isp_3dnr_process_frame(pctx, pframe);
	isp_ltm_process_frame(pctx, pframe);

	use_fmcu = 0;
	fmcu = (struct isp_fmcu_ctx_desc *)pctx_hw->fmcu_handle;
	if (fmcu) {
		use_fmcu = (tmp.multi_slice | pctx->enable_slowmotion);
		if (use_fmcu)
			fmcu->ops->ctx_reset(fmcu);
	}

	if (tmp.multi_slice  || pctx->enable_slowmotion) {
		struct slice_cfg_input slc_cfg;

		memset(&slc_cfg, 0, sizeof(slc_cfg));
		for (i = 0; i < ISP_SPATH_NUM; i++) {
			path = &pctx->isp_path[i];
			if (atomic_read(&path->user_cnt) < 1)
				continue;
			slc_cfg.frame_store[i] = &path->store;
			if ((i < FBC_PATH_NUM) && (path->afbc_store.bypass == 0))
				slc_cfg.frame_afbc_store[i] = &path->afbc_store;
			if ((i < FBC_PATH_NUM) && (path->ifbc_store.bypass == 0))
				slc_cfg.frame_ifbc_store[i] = &path->ifbc_store;
		}
		slc_cfg.frame_fetch = &pctx->fetch;
		slc_cfg.frame_fbd_raw = &pctx->fbd_raw;
		isp_cfg_slice_fetch_info(&slc_cfg, pctx->slice_ctx);
		isp_cfg_slice_store_info(&slc_cfg, pctx->slice_ctx);
		if (path->afbc_store.bypass == 0)
			isp_cfg_slice_afbc_store_info(&slc_cfg, pctx->slice_ctx);
		if (path->ifbc_store.bypass == 0)
			isp_cfg_slice_ifbc_store_info(&slc_cfg, pctx->slice_ctx);

		/* 3DNR Capture case: Using CP Config */
		slc_cfg.frame_in_size.w = pctx->input_trim.size_x;
		slc_cfg.frame_in_size.h = pctx->input_trim.size_y;
		slc_cfg.nr3_ctx = &pctx->nr3_ctx;
		isp_cfg_slice_3dnr_info(&slc_cfg, pctx->slice_ctx);

		slc_cfg.ltm_ctx = &pctx->ltm_ctx;
		if (pctx->ltm_rgb)
			isp_cfg_slice_ltm_info(&slc_cfg, pctx->slice_ctx, LTM_RGB);
		if (pctx->ltm_yuv)
			isp_cfg_slice_ltm_info(&slc_cfg, pctx->slice_ctx, LTM_YUV);

		slc_cfg.nofilter_ctx = &pctx->isp_k_param;
		isp_cfg_slice_noisefilter_info(&slc_cfg, pctx->slice_ctx);

		if (!use_fmcu) {
			pr_debug("use ap support slices for ctx %d hw %d\n",
				pctx->ctx_id, hw_ctx_id);
			slc_by_ap = 1;
		} else {
			pr_debug("use fmcu support slices for ctx %d hw %d\n",
				pctx->ctx_id, hw_ctx_id);
			ret = isp_set_slices_fmcu_cmds((void *)fmcu, pctx);
			if (ret == 0)
				kick_fmcu = 1;
		}
	}

	pctx->updated = 0;
	mutex_unlock(&pctx->param_mutex);

	if (pctx->enable_slowmotion) {
		for (i = 0; i < pctx->slowmotion_count - 1; i++) {
			ret = set_fmcu_slw_queue(fmcu, pctx);
			if (ret)
				pr_err("fail to set fmcu slw queue\n");
		}
	}

	ret = wait_for_completion_interruptible_timeout(
					&pctx->frm_done,
					ISP_CONTEXT_TIMEOUT);
	if (ret == ERESTARTSYS) {
		pr_err("fail to interrupt, when isp wait\n");
		ret = -EFAULT;
		goto dequeue;
	} else if (ret == 0) {
		pr_err("fail to wait isp context %d, timeout.\n", pctx->ctx_id);
		ret = -EFAULT;
		goto dequeue;
	}

	if (tmp.stream) {
		ret = camera_enqueue(&pctx->stream_ctrl_proc_q, &tmp.stream->list);
		if (ret) {
			pr_err("fail to stream state overflow\n");
			put_empty_state(tmp.stream);
		}
	}
	pctx->iommu_status = (uint32_t)(-1);
	pctx->started = 1;
	pctx->multi_slice = tmp.multi_slice ;
	pctx_hw->fmcu_used = use_fmcu;

	if (slc_by_ap) {
		ret = proc_slices(pctx);
		goto done;
	}

	/* start to prepare/kickoff cfg buffer. */
	if (likely(dev->wmode == ISP_CFG_MODE)) {
		pr_debug("cfg enter.");

		/* blkpm_lock to avoid user config block param across frame */
		mutex_lock(&pctx->blkpm_lock);
		if (pctx->in_fmt == IMG_PIX_FMT_NV21) {
			hw->hw_ops.core_ops.isp_yuv_block_ctrl(pctx->ctx_id,
				&pctx->isp_k_param, ISP_YUV_BLOCK_DISABLE);
		} else {
			hw->hw_ops.core_ops.isp_yuv_block_ctrl(pctx->ctx_id,
				&pctx->isp_k_param, ISP_YUV_BLOCK_CFG);
		}
		ret = cfg_desc->ops->hw_cfg(cfg_desc,
					pctx->ctx_id, hw_ctx_id, kick_fmcu);
		mutex_unlock(&pctx->blkpm_lock);

		if (kick_fmcu) {
			pr_info("fmcu start.");
			if (pctx->slw_state == CAM_SLOWMOTION_ON) {
				ret = fmcu->ops->cmd_ready(fmcu);
			} else {
				ret = fmcu->ops->hw_start(fmcu);
				if (!ret && pctx->enable_slowmotion)
					pctx->slw_state = CAM_SLOWMOTION_ON;
			}
		} else {
			pr_debug("cfg start. fid %d\n", frame_id);
			ret = cfg_desc->ops->hw_start(
					cfg_desc, hw_ctx_id);
		}
	} else {
		if (kick_fmcu) {
			pr_info("fmcu start.");
			ret = fmcu->ops->hw_start(fmcu);
		} else {
			pr_debug("fetch start.");
			ISP_HREG_WR(ISP_FETCH_START, 1);
		}
	}

done:
	pr_debug("done.\n");
	return 0;

unlock:
	mutex_unlock(&pctx->param_mutex);
dequeue:
	for (i = 0; i < ISP_SPATH_NUM; i++) {
		path = &pctx->isp_path[i];
		if (atomic_read(&path->user_cnt) < 1)
			continue;
		pframe = camera_dequeue_tail(&path->result_queue);
		if (pframe) {
			/* ret frame to original queue */
			if (pframe->is_reserved)
				camera_enqueue(
					&path->reserved_buf_queue, &pframe->list);
			else
				camera_enqueue(
					&path->out_buf_queue, &pframe->list);
		}
		atomic_dec(&path->store_cnt);
	}

	pframe = camera_dequeue_tail(&pctx->proc_queue);
inq_overflow:
	if (pframe)
		cambuf_iommu_unmap(&pframe->buf);
map_err:
input_err:
	if (pframe) {
		free_offline_pararm(pframe->param_data);
		pframe->param_data = NULL;
		/* release sync data as if ISP has consumed */
		if (pframe->sync_data)
			dcam_if_release_sync(pframe->sync_data, pframe);
		/* return buffer to cam channel shared buffer queue. */
		if (tmp.stream && tmp.stream->data_src == ISP_STREAM_SRC_ISP)
			pr_debug("isp post proc no need return\n");
		else
			pctx->isp_cb_func(ISP_CB_RET_SRC_BUF, pframe, pctx->cb_priv_data);
	}

	if (tmp.stream)
		put_empty_state(tmp.stream);
	if (pctx->enable_slowmotion) {
		for (i = 0; i < pctx->slowmotion_count - 1; i++) {
			pframe = camera_dequeue(&pctx->in_queue,
					struct camera_frame, list);
			if (pframe) {
				free_offline_pararm(pframe->param_data);
				pframe->param_data = NULL;
				/* release sync data as if ISP has consumed */
				if (pframe->sync_data)
					dcam_if_release_sync(pframe->sync_data, pframe);
				/* return buffer to cam channel shared buffer queue. */
				pctx->isp_cb_func(ISP_CB_RET_SRC_BUF, pframe, pctx->cb_priv_data);
			}
		}
	}
	if (pctx_hw)
		isp_context_unbind(pctx);
	return ret;
}

static int isp_offline_thread_loop(void *arg)
{
	struct isp_pipe_dev *dev = NULL;
	struct isp_pipe_context *pctx;
	struct cam_thread_info *thrd;

	if (!arg) {
		pr_err("fail to get valid input ptr\n");
		return -1;
	}

	thrd = (struct cam_thread_info *)arg;
	pctx = (struct isp_pipe_context *)thrd->ctx_handle;
	dev = pctx->dev;

	while (1) {
		if (wait_for_completion_interruptible(
			&thrd->thread_com) == 0) {
			if (atomic_cmpxchg(
					&thrd->thread_stop, 1, 0) == 1) {
				pr_info("isp context %d thread stop.\n",
						pctx->ctx_id);
				break;
			}
			pr_debug("thread com done.\n");

			if (thrd->proc_func(pctx)) {
				pr_err("fail to start isp pipe to proc. exit thread\n");
				pctx->isp_cb_func(
					ISP_CB_DEV_ERR, dev,
					pctx->cb_priv_data);
				break;
			}
		} else {
			pr_debug("offline thread exit!");
			break;
		}
	}

	return 0;
}

static int isp_stop_offline_thread(void *param)
{
	int cnt = 0;
	int ret = 0;
	struct cam_thread_info *thrd;
	struct isp_pipe_context *pctx;

	thrd = (struct cam_thread_info *)param;
	pctx = (struct isp_pipe_context *)thrd->ctx_handle;

	if (thrd->thread_task) {
		atomic_set(&thrd->thread_stop, 1);
		complete(&thrd->thread_com);
		while (cnt < 2500) {
			cnt++;
			if (atomic_read(&thrd->thread_stop) == 0)
				break;
			udelay(1000);
		}
		thrd->thread_task = NULL;
		pr_info("offline thread stopped. wait %d ms\n", cnt);
	}

	/* wait for last frame done */
	ret = wait_for_completion_interruptible_timeout(
					&pctx->frm_done,
					ISP_CONTEXT_TIMEOUT);
	if (ret == -ERESTARTSYS)
		pr_err("fail to interrupt, when isp wait\n");
	else if (ret == 0)
		pr_err("fail to wait ctx %d, timeout.\n", pctx->ctx_id);
	else
		pr_info("wait time %d\n", ret);
	return 0;
}

static int isp_create_offline_thread(void *param)
{
	struct isp_pipe_context *pctx;
	struct cam_thread_info *thrd;
	char thread_name[32] = { 0 };

	pctx = (struct isp_pipe_context *)param;
	thrd = &pctx->thread;
	thrd->ctx_handle = pctx;

	if (thrd->thread_task) {
		pr_info("isp ctx sw %d offline thread created is exist.\n",
			pctx->ctx_id);
		return 0;
	}

	thrd->proc_func = isp_offline_start_frame;
	sprintf(thread_name, "isp_ctx%d_offline", pctx->ctx_id);
	atomic_set(&thrd->thread_stop, 0);
	init_completion(&thrd->thread_com);
	thrd->thread_task = kthread_run(isp_offline_thread_loop,
		thrd, "%s", thread_name);
	if (IS_ERR_OR_NULL(thrd->thread_task)) {
		pr_err("fail to start offline thread for isp sw %d err %ld\n",
				pctx->ctx_id, PTR_ERR(thrd->thread_task));
		return -EFAULT;
	}

	pr_info("isp ctx sw %d offline thread created.\n", pctx->ctx_id);
	return 0;
}

static int isp_context_init(struct isp_pipe_dev *dev)
{
	int ret = 0;
	int i, bind_fmcu;
	struct isp_fmcu_ctx_desc *fmcu = NULL;
	struct isp_cfg_ctx_desc *cfg_desc = NULL;
	struct isp_pipe_context *pctx;
	struct isp_pipe_hw_context *pctx_hw;
	struct cam_hw_info *hw = NULL;
	enum isp_context_id cid[ISP_CONTEXT_SW_NUM] = {
		ISP_CONTEXT_P0,
		ISP_CONTEXT_C0,
		ISP_CONTEXT_P1,
		ISP_CONTEXT_C1,
		ISP_CONTEXT_P2,
		ISP_CONTEXT_C2,
		ISP_CONTEXT_SUPERZOOM
	};

	pr_info("isp contexts init start!\n");
	memset(&dev->ctx[0], 0, sizeof(dev->ctx));

	for (i = 0; i < ISP_CONTEXT_SW_NUM; i++) {
		pctx = &dev->ctx[i];
		pctx->ctx_id = cid[i];
		pctx->dev = dev;
		pctx->attach_cam_id = CAM_ID_MAX;
		pctx->hw = dev->isp_hw;
		atomic_set(&pctx->user_cnt, 0);
		pr_debug("isp context %d init done!\n", cid[i]);
	}

	/* CFG module init */
	if (dev->wmode == ISP_AP_MODE) {

		pr_info("isp ap mode.\n");
		for (i = 0; i < ISP_CONTEXT_SW_NUM; i++)
			isp_cfg_poll_addr[i] = &s_isp_regbase[0];

	} else {
		cfg_desc = get_isp_cfg_ctx_desc();
		if (!cfg_desc || !cfg_desc->ops) {
			pr_err("fail to get isp cfg ctx %p.\n", cfg_desc);
			ret = -EINVAL;
			goto cfg_null;
		}
		cfg_desc->hw_ops = &dev->isp_hw->hw_ops;
		pr_debug("cfg_init start.\n");

		ret = cfg_desc->ops->ctx_init(cfg_desc);
		if (ret) {
			pr_err("fail to cfg ctx init.\n");
			goto ctx_fail;
		}
		pr_debug("cfg_init done.\n");

		ret = cfg_desc->ops->hw_init(cfg_desc);
		if (ret)
			goto hw_fail;
	}
	dev->cfg_handle = cfg_desc;

	dev->ltm_handle = isp_get_ltm_share_ctx_desc();

	pr_info("isp hw contexts init start!\n");
	for (i = 0; i < ISP_CONTEXT_HW_NUM; i++) {
		pctx_hw = &dev->hw_ctx[i];
		pctx_hw->hw_ctx_id = i;
		pctx_hw->sw_ctx_id = 0xffff;
		atomic_set(&pctx_hw->user_cnt, 0);

		hw = dev->isp_hw;
		hw->hw_ops.isp_irq_ops.irq_enable(hw->ip_isp, &i);
		hw->hw_ops.isp_irq_ops.irq_clear(hw->ip_isp, &i);

		bind_fmcu = 0;
		if (unlikely(dev->wmode == ISP_AP_MODE)) {
			/* for AP mode, multi-context is not supported */
			if (i != ISP_CONTEXT_HW_P0) {
				atomic_set(&pctx_hw->user_cnt, 1);
				continue;
			}
			bind_fmcu = 1;
		} else if (*(hw->ip_isp->ctx_fmcu_support + i)) {
			bind_fmcu = 1;
		}

		if (bind_fmcu) {
			fmcu = get_isp_fmcu_ctx_desc(hw);
			pr_debug("get fmcu %p\n", fmcu);

			if (fmcu && fmcu->ops) {
				ret = fmcu->ops->ctx_init(fmcu);
				if (ret) {
					pr_err("fail to init fmcu ctx\n");
					put_isp_fmcu_ctx_desc(fmcu);
				} else {
					pctx_hw->fmcu_handle = fmcu;
				}
			} else {
				pr_info("no more fmcu or ops\n");
			}
		}
		pr_info("isp hw context %d init done. fmcu %p\n",
				i, pctx_hw->fmcu_handle);

		fmcu = (struct isp_fmcu_ctx_desc *)pctx_hw->fmcu_handle;
		if (fmcu) {
			uint32_t val[2];
			unsigned long reg_offset = 0;
			uint32_t reg_bits[4] = { 0x00, 0x02, 0x01, 0x03};

			reg_offset = (fmcu->fid == 0) ?
						ISP_COMMON_FMCU0_PATH_SEL :
						ISP_COMMON_FMCU1_PATH_SEL;
			if (reg_offset == 0) {
				pr_info("no fmcu sel register\n");
				continue;
			}

			ISP_HREG_MWR(reg_offset, BIT_1 | BIT_0, reg_bits[i]);
			pr_debug("FMCU%d reg_bits %d for hw_ctx %d\n", fmcu->fid, reg_bits[i], i);

			val[0] = ISP_HREG_RD(ISP_COMMON_FMCU0_PATH_SEL);
			val[1] = ISP_HREG_RD(ISP_COMMON_FMCU1_PATH_SEL);

			if ((val[0] & 3) == (val[1] & 3)) {
				pr_warn("isp fmcus select same context %d\n", val[0] & 3);
				if (fmcu->fid == 0) {
					/* force to set different value */
					reg_offset = ISP_COMMON_FMCU1_PATH_SEL;
					ISP_HREG_MWR(reg_offset, BIT_1 | BIT_0, reg_bits[(i + 1) & 3]);
					pr_debug("force FMCU1 regbits %d\n", reg_bits[(i + 1) & 3]);
				}
			}
		}
	}

	pr_debug("done!\n");
	return 0;

hw_fail:
	cfg_desc->ops->ctx_deinit(cfg_desc);
ctx_fail:
	put_isp_cfg_ctx_desc(cfg_desc);
cfg_null:
	return ret;
}

static int isp_context_deinit(struct isp_pipe_dev *dev)
{
	int ret = 0;
	int i, j;
	uint32_t path_id;
	struct isp_fmcu_ctx_desc *fmcu = NULL;
	struct isp_cfg_ctx_desc *cfg_desc = NULL;
	struct isp_pipe_context *pctx;
	struct isp_pipe_hw_context *pctx_hw;
	struct isp_path_desc *path;

	pr_debug("enter.\n");

	for (i = 0; i < ISP_CONTEXT_SW_NUM; i++) {
		pctx  = &dev->ctx[i];

		/* free all used path here if user did not call put_path  */
		for (j = 0; j < ISP_SPATH_NUM; j++) {
			path = &pctx->isp_path[j];
			path_id = path->spath_id;
			if (atomic_read(&path->user_cnt) > 0)
				sprd_isp_put_path(dev, pctx->ctx_id, path_id);
		}
		sprd_isp_put_context(dev, pctx->ctx_id);

		atomic_set(&pctx->user_cnt, 0);
		mutex_destroy(&pctx->param_mutex);
		mutex_destroy(&pctx->blkpm_lock);
	}

	for (i = 0; i < ISP_CONTEXT_HW_NUM; i++) {
		pctx_hw  = &dev->hw_ctx[i];
		fmcu = (struct isp_fmcu_ctx_desc *)pctx_hw->fmcu_handle;
		if (fmcu) {
			fmcu->ops->ctx_deinit(fmcu);
			put_isp_fmcu_ctx_desc(fmcu);
		}
		pctx_hw->fmcu_handle = NULL;
		pctx_hw->fmcu_used = 0;
		atomic_set(&pctx_hw->user_cnt, 0);
		trace_isp_irq_cnt(i);
	}
	pr_info("isp contexts deinit done!\n");

	cfg_desc = (struct isp_cfg_ctx_desc *)dev->cfg_handle;
	if (cfg_desc) {
		cfg_desc->ops->ctx_deinit(cfg_desc);
		put_isp_cfg_ctx_desc(cfg_desc);
	}
	dev->cfg_handle = NULL;

	isp_put_ltm_share_ctx_desc(dev->ltm_handle);
	dev->ltm_handle = NULL;

	pr_debug("done.\n");
	return ret;
}

static int isp_slice_ctx_init(struct isp_pipe_context *pctx, uint32_t * multi_slice)
{
	int ret = 0;
	int j;
	uint32_t val;
	struct isp_path_desc *path;
	struct slice_cfg_input slc_cfg_in;

	*multi_slice = 0;

	if  ((isp_slice_needed(pctx) == 0) && (pctx->enable_slowmotion == 0)) {
		pr_debug("sw %d don't need to slice , slowmotion %d\n",
			pctx->ctx_id, pctx->enable_slowmotion);
		return 0;
	}

	if (pctx->slice_ctx == NULL) {
		pctx->slice_ctx = get_isp_slice_ctx();
		if (IS_ERR_OR_NULL(pctx->slice_ctx)) {
			pr_err("fail to get memory for slice_ctx.\n");
			pctx->slice_ctx = NULL;
			ret = -ENOMEM;
			goto exit;
		}
	}

	memset(&slc_cfg_in, 0, sizeof(struct slice_cfg_input));
	slc_cfg_in.frame_in_size.w = pctx->input_trim.size_x;
	slc_cfg_in.frame_in_size.h = pctx->input_trim.size_y;
	slc_cfg_in.frame_fetch = &pctx->fetch;
	slc_cfg_in.frame_fbd_raw = &pctx->fbd_raw;
	for (j = 0; j < ISP_SPATH_NUM; j++) {
		path = &pctx->isp_path[j];
		if (atomic_read(&path->user_cnt) <= 0)
			continue;
		slc_cfg_in.frame_out_size[j] = &path->dst;
		slc_cfg_in.frame_store[j] = &path->store;
		slc_cfg_in.frame_scaler[j] = &path->scaler;
		slc_cfg_in.frame_deci[j] = &path->deci;
		slc_cfg_in.frame_trim0[j] = &path->in_trim;
		slc_cfg_in.frame_trim1[j] = &path->out_trim;
		if ((j < FBC_PATH_NUM) && (path->afbc_store.bypass == 0))
			slc_cfg_in.frame_afbc_store[j] = &path->afbc_store;
		if ((j < FBC_PATH_NUM) && (path->ifbc_store.bypass == 0))
			slc_cfg_in.frame_ifbc_store[j] = &path->ifbc_store;
	}

	val = ISP_REG_RD(pctx->ctx_id, ISP_NLM_RADIAL_1D_DIST);
	slc_cfg_in.nlm_center_x = val & 0x3FFF;
	slc_cfg_in.nlm_center_y = (val >> 16) & 0x3FFF;

	val = ISP_REG_RD(pctx->ctx_id, ISP_YNR_CFG31);
	slc_cfg_in.ynr_center_x = val & 0xFFFF;
	slc_cfg_in.ynr_center_y = (val >> 16) & 0xfFFF;
	pr_debug("ctx %d,  nlm center %d %d, ynr center %d, %d\n",
		pctx->ctx_id, slc_cfg_in.nlm_center_x, slc_cfg_in.nlm_center_y,
		slc_cfg_in.ynr_center_x, slc_cfg_in.ynr_center_y);

	isp_cfg_slices(&slc_cfg_in, pctx->slice_ctx, &pctx->valid_slc_num);

	pr_debug("sw %d valid_slc_num %d\n", pctx->ctx_id, pctx->valid_slc_num);
	if (pctx->valid_slc_num > 1)
		*multi_slice = 1;
exit:
	return ret;
}

static int isp_get_stream_state(struct isp_pipe_context *pctx)
{
	int ret = 0;
	int i = 0, j = 0;
	uint32_t normal_cnt = 0, postproc_cnt = 0;
	uint32_t maxw = 0, maxh = 0;
	uint32_t scl_x = 0, scl_w = 0, scl_h = 0;
	enum isp_stream_frame_type frame_type = ISP_STREAM_SIGNLE;
	struct isp_stream_ctrl *stream = NULL;
	struct isp_path_desc *path = NULL;
	struct isp_stream_ctrl tmp_stream[5];

	if (!pctx) {
		pr_err("fail to get valid input ptr\n");
		return -EFAULT;
	}

	if (pctx->stream_ctrl_in_q.cnt != 0) {
		pr_debug("cxt %d stream state not complete\n", pctx->ctx_id);
		return ret;
	}

	normal_cnt = 1;
	for (i = 0; i < ISP_SPATH_NUM; i++) {
		path = &pctx->isp_path[i];
		if (atomic_read(&path->user_cnt) < 1)
			continue;
		maxw = MAX(maxw, path->stream_dst.w);
		maxh = MAX(maxh, path->stream_dst.h);
	}

	if (!maxw || !maxh) {
		pr_err("fail to get valid max dst size\n");
		return -EFAULT;
	}

	scl_w = maxw / pctx->src_info.src_crop.size_x;
	if ((maxw % pctx->src_info.src_crop.size_x) != 0)
		scl_w ++;
	scl_h = maxh / pctx->src_info.src_crop.size_y;
	if ((maxh % pctx->src_info.src_crop.size_y) != 0)
		scl_h ++;
	scl_x = MAX(scl_w, scl_h);
	pr_debug("scl_x %d scl_w %d scl_h %d max_w %d max_h %d\n", scl_x,
		scl_w, scl_h, maxw, maxh);
	do {
		if (scl_x == ISP_SCALER_UP_MAX)
			break;
		scl_x = scl_x / ISP_SCALER_UP_MAX;
		if (scl_x)
			postproc_cnt ++;
	} while(scl_x);

	/* If need postproc, first normal proc need to mark as postproc too*/
	if (postproc_cnt) {
		postproc_cnt ++;
		normal_cnt --;
	}

	for (i = postproc_cnt - 1; i >= 0; i--) {
		maxw = maxw / ISP_SCALER_UP_MAX;
		maxw = MAX(maxw, pctx->src_info.src_crop.size_x);
		maxw = ISP_ALIGN_W(maxw);
		maxh = maxh / ISP_SCALER_UP_MAX;
		maxh = MAX(maxh, pctx->src_info.src_crop.size_y);
		maxh = ISP_ALIGN_H(maxh);

		if (i == 0) {
			tmp_stream[i].in_fmt = pctx->src_info.src_fmt;
			tmp_stream[i].in = pctx->src_info.src;
			tmp_stream[i].in_crop = pctx->src_info.src_crop;
		} else {
			tmp_stream[i].in_fmt = IMG_PIX_FMT_NV21;
			tmp_stream[i].in.w = maxw;
			tmp_stream[i].in.h = maxh;
			tmp_stream[i].in_crop.start_x = 0;
			tmp_stream[i].in_crop.start_y = 0;
			tmp_stream[i].in_crop.size_x = maxw;
			tmp_stream[i].in_crop.size_y = maxh;
		}
		for (j = 0; j < ISP_SPATH_NUM; j++) {
			path = &pctx->isp_path[j];
			if (atomic_read(&path->user_cnt) < 1)
				continue;
			/* This is for ensure the last postproc frame buf is OUT for user
			.* Then the frame before should be POST. Thus, only one post
			.* buffer is enough for all the isp postproc process */
			if ((postproc_cnt + i) % 2 == 1)
				tmp_stream[i].buf_type[j] = ISP_STREAM_BUF_OUT;
			else
				tmp_stream[i].buf_type[j] = ISP_STREAM_BUF_POSTPROC;
			if (j != ISP_SPATH_CP && (i != postproc_cnt - 1))
				tmp_stream[i].buf_type[j] = ISP_STREAM_BUF_RESERVED;
			if (i == (postproc_cnt - 1)) {
				tmp_stream[i].out[j] = path->stream_dst;
				tmp_stream[i].out_crop[j].start_x = 0;
				tmp_stream[i].out_crop[j].start_y = 0;
				tmp_stream[i].out_crop[j].size_x = maxw;
				tmp_stream[i].out_crop[j].size_y = maxh;
			} else if (i == 0) {
				tmp_stream[i].out[j].w = tmp_stream[i + 1].in.w;
				tmp_stream[i].out[j].h = tmp_stream[i + 1].in.h;
				tmp_stream[i].out_crop[j] = path->stream_in_trim;
			} else {
				tmp_stream[i].out[j].w = tmp_stream[i + 1].in.w;
				tmp_stream[i].out[j].h = tmp_stream[i + 1].in.h;
				tmp_stream[i].out_crop[j].start_x = 0;
				tmp_stream[i].out_crop[j].start_y = 0;
				tmp_stream[i].out_crop[j].size_x = maxw;
				tmp_stream[i].out_crop[j].size_y = maxh;
			}
			pr_debug("isp %d i %d j %d out_size %d %d crop_szie %d %d %d %d\n",
				pctx->ctx_id, i, j, tmp_stream[i].out[j].w, tmp_stream[i].out[j].h,
				tmp_stream[i].out_crop[j].start_x, tmp_stream[i].out_crop[j].start_y,
				tmp_stream[i].out_crop[j].size_x, tmp_stream[i].out_crop[j].size_y);
		}
		pr_debug("isp %d index %d in_size %d %d crop_szie %d %d %d %d\n",
			pctx->ctx_id, i, tmp_stream[i].in.w, tmp_stream[i].in.h,
			tmp_stream[i].in_crop.start_x, tmp_stream[i].in_crop.start_y,
			tmp_stream[i].in_crop.size_x, tmp_stream[i].in_crop.size_y);
	}

	if (pctx->mode_3dnr == MODE_3DNR_CAP) {
		normal_cnt = normal_cnt + NR3_BLEND_CNT - 1;
		frame_type = ISP_STREAM_MULTI;
	}
	for (i = 0; i < normal_cnt; i++) {
		stream = get_empty_state();
		stream->state = ISP_STREAM_NORMAL_PROC;
		stream->data_src = ISP_STREAM_SRC_DCAM;
		stream->frame_type = frame_type;
		stream->in = pctx->src_info.src;
		stream->in_crop = pctx->src_info.src_crop;
		stream->in_fmt = pctx->src_info.src_fmt;
		for (j = 0; j < ISP_SPATH_NUM; j++) {
			path = &pctx->isp_path[j];
			if (atomic_read(&path->user_cnt) < 1)
				continue;
			/* Use reserved buffer when not 3dnr last frame */
			if (normal_cnt == 1 || i == (NR3_BLEND_CNT - 1))
				stream->buf_type[j] = ISP_STREAM_BUF_OUT;
			else
				stream->buf_type[j] = ISP_STREAM_BUF_RESERVED;
			stream->out[j] = path->stream_dst;
			stream->out_crop[j] = path->stream_in_trim;
			if (postproc_cnt) {
				stream->out[j] = tmp_stream[0].out[j];
				stream->out_crop[j] = tmp_stream[0].out_crop[j];
			}
			pr_debug("isp %d out_size %d %d crop_szie %d %d %d %d\n",
				pctx->ctx_id, stream->out[j].w, stream->out[j].h,
				stream->out_crop[j].start_x, stream->out_crop[j].start_y,
				stream->out_crop[j].size_x, stream->out_crop[j].size_y);
		}
		stream->cur_cnt = i;
		stream->max_cnt = normal_cnt + postproc_cnt - 1;
		pr_debug("stream type %d cur_cnt %d max_cnt %d\n",
			stream->buf_type, stream->cur_cnt, stream->max_cnt);
		ret = camera_enqueue(&pctx->stream_ctrl_in_q, &stream->list);
		if (ret) {
			pr_info("stream state overflow\n");
			put_empty_state(stream);
		}
		pr_debug("isp %d in_size %d %d crop_szie %d %d %d %d\n",
			pctx->ctx_id, stream->in.w, stream->in.h,
			stream->in_crop.start_x, stream->in_crop.start_y,
			stream->in_crop.size_x, stream->in_crop.size_y);
	}

	for (i = 0; i < postproc_cnt; i++) {
		stream = get_empty_state();
		stream->state = ISP_STREAM_POST_PROC;
		stream->data_src = ISP_STREAM_SRC_ISP;
		stream->frame_type = frame_type;
		stream->in_fmt = tmp_stream[i].in_fmt;
		stream->in = tmp_stream[i].in;
		stream->in_crop = tmp_stream[i].in_crop;
		pr_debug("isp %d in_size %d %d crop_szie %d %d %d %d\n",
			pctx->ctx_id, stream->in.w, stream->in.h,
			stream->in_crop.start_x, stream->in_crop.start_y,
			stream->in_crop.size_x, stream->in_crop.size_y);
		for (j = 0; j < ISP_SPATH_NUM; j++) {
			path = &pctx->isp_path[j];
			if (atomic_read(&path->user_cnt) < 1)
				continue;
			stream->buf_type[j] = tmp_stream[i].buf_type[j];
			stream->out[j] = tmp_stream[i].out[j];
			stream->out_crop[j] = tmp_stream[i].out_crop[j];
			pr_debug("isp %d out_size %d %d crop_szie %d %d %d %d\n",
				pctx->ctx_id, stream->out[j].w, stream->out[j].h,
				stream->out_crop[j].start_x, stream->out_crop[j].start_y,
				stream->out_crop[j].size_x, stream->out_crop[j].size_y);
		}
		if (i == 0)
			stream->data_src = ISP_STREAM_SRC_DCAM;
		stream->cur_cnt = i + normal_cnt;
		stream->max_cnt = normal_cnt + postproc_cnt - 1;
		ret = camera_enqueue(&pctx->stream_ctrl_in_q, &stream->list);
		if (ret) {
			pr_err("fail to stream state overflow\n");
			put_empty_state(stream);
		}
	}

	return ret;
}

static int isp_put_stream_state(void *isp_handle, int ctx_id)
{
	int ret = 0;
	struct isp_pipe_dev *dev = NULL;
	struct isp_pipe_context *pctx = NULL;
	struct isp_stream_ctrl *stream = NULL;

	if (!isp_handle) {
		pr_err("fail to input ptr NULL\n");
		ret = -EFAULT;
		goto exit;
	}

	if (ctx_id < 0 || ctx_id >= ISP_CONTEXT_SW_NUM) {
		pr_debug("fail to ctx_id is err  %d\n", ctx_id);
		ret = -EFAULT;
		goto exit;
	}

	dev = (struct isp_pipe_dev *)isp_handle;
	pctx = &dev->ctx[ctx_id];

	do {
		stream = camera_dequeue(&pctx->stream_ctrl_in_q,
			struct isp_stream_ctrl, list);
		if (stream)
			put_empty_state(stream);
	} while(stream);

exit:
	return ret;
}

static int isp_postproc_irq(void *handle,uint32_t idx,
	enum isp_postproc_type type)
{
	int ret = 0;
	int i, j;
	struct isp_pipe_dev *dev = NULL;
	struct isp_stream_ctrl *stream = NULL;
	struct isp_pipe_context *pctx = NULL;
	struct camera_frame *pframe;
	struct isp_path_desc *path;
	struct timespec cur_ts;
	ktime_t boot_time;

	if (!handle || type >= POSTPROC_MAX) {
		pr_err("fail to get valid input handle %p, type %d\n",
			handle, type);
		return -EFAULT;
	}

	dev = (struct isp_pipe_dev *)handle;
	pctx = &dev->ctx[idx];
	if (pctx->enable_slowmotion == 0) {
		isp_context_unbind(pctx);
		complete(&pctx->frm_done);
	}

	boot_time = ktime_get_boottime();
	ktime_get_ts(&cur_ts);

	stream = camera_dequeue(&pctx->stream_ctrl_proc_q,
		struct isp_stream_ctrl, list);
	pframe = camera_dequeue(&pctx->proc_queue, struct camera_frame, list);
	if (pframe) {
		if (stream && stream->data_src == ISP_STREAM_SRC_ISP) {
			pr_debug("isp %d post proc, do not need to return frame\n",
				pctx->ctx_id);
			cambuf_iommu_unmap(&pframe->buf);
		} else {
			/* return buffer to cam channel shared buffer queue. */
			cambuf_iommu_unmap(&pframe->buf);
			pctx->isp_cb_func(ISP_CB_RET_SRC_BUF, pframe,
				pctx->cb_priv_data);
			pr_debug("sw %d, ch_id %d, fid:%d, shard buffer cnt:%d\n",
				pctx->ctx_id, pframe->channel_id, pframe->fid,
				pctx->proc_queue.cnt);
		}
	} else {
		pr_err("fail to get src frame  sw_idx=%d  proc_queue.cnt:%d\n",
			pctx->ctx_id, pctx->proc_queue.cnt);
	}

	if (pctx->sw_slice_num) {
		if (pctx->sw_slice_no != (pctx->sw_slice_num - 1)) {
			pr_debug("done cxt_id:%d ch_id[%d] slice %d\n",
				pctx->ctx_id, pctx->ch_id, pctx->sw_slice_no);
			return 0;
		} else {
			pr_debug("done cxt_id:%d ch_id[%d] lastslice %d\n",
				pctx->ctx_id, pctx->ch_id, pctx->sw_slice_no);
			pctx->sw_slice_no = 0;
			pctx->sw_slice_num = 0;
		}
	}

	/* get output buffers for all path */
	for (i = 0; i < ISP_SPATH_NUM; i++) {
		path = &pctx->isp_path[i];
		if (atomic_read(&path->user_cnt) <= 0) {
			pr_debug("path %p not enable\n", path);
			continue;
		}
		if (path->bind_type == ISP_PATH_SLAVE) {
			pr_debug("slave path %d\n", path->spath_id);
			continue;
		}
		pframe = camera_dequeue(&path->result_queue,
				struct camera_frame, list);
		if (!pframe) {
			pr_err("fail to get frame from queue. cxt:%d, path:%d\n",
						pctx->ctx_id, path->spath_id);
			continue;
		}
		atomic_dec(&path->store_cnt);
		pframe->boot_time = boot_time;
		pframe->time.tv_sec = cur_ts.tv_sec;
		pframe->time.tv_usec = cur_ts.tv_nsec / NSEC_PER_USEC;

		pr_debug("ctx %d path %d, ch_id %d, fid %d, storen %d, queue cnt:%d\n",
			pctx->ctx_id, path->spath_id, pframe->channel_id, pframe->fid,
			atomic_read(&path->store_cnt), path->result_queue.cnt);
		pr_debug("time_sensor %03d.%6d, time_isp %03d.%06d\n",
			(uint32_t)pframe->sensor_time.tv_sec,
			(uint32_t)pframe->sensor_time.tv_usec,
			(uint32_t)pframe->time.tv_sec,
			(uint32_t)pframe->time.tv_usec);

		if (unlikely(pframe->is_reserved)) {
			camera_enqueue(&path->reserved_buf_queue, &pframe->list);
		} else if (stream && stream->cur_cnt != stream->max_cnt &&
			stream->state == ISP_STREAM_POST_PROC) {
			cambuf_iommu_unmap(&pframe->buf);
			camera_enqueue(&pctx->in_queue, &pframe->list);
			complete(&pctx->thread.thread_com);
		} else {
			if (pframe->buf.mfd[0] == path->reserved_buf_fd) {
				for (j = 0; j < 3; j++) {
					pframe->buf.size[j] = path->reserve_buf_size[j];
					pr_debug("pframe->buf.size[j] = %d, path->reserve_buf_size[j] = %d",
						(int)pframe->buf.size[j], (int)path->reserve_buf_size[j]);
				}
			}
			cambuf_iommu_unmap(&pframe->buf);
			pctx->isp_cb_func(ISP_CB_RET_DST_BUF,
				pframe, pctx->cb_priv_data);
		}
	}

	if (stream)
		put_empty_state(stream);

	return ret;
}

static int isp_blk_pm_init(struct isp_k_block *isp_k_param)
{
	isp_k_param->nlm_info.bypass = 1;
	isp_k_param->ynr_param.ynr_info.bypass = 1;
	isp_k_param->rgb_ltm.ltm_stat.bypass = 1;
	isp_k_param->rgb_ltm.ltm_map.bypass = 1;
	isp_k_param->yuv_ltm.ltm_stat.bypass = 1;
	isp_k_param->yuv_ltm.ltm_map.bypass = 1;

	isp_k_param->cce_bypass = 1;
	isp_k_param->bchs_bypass = 1;
	isp_k_param->cdn_bypass = 1;
	isp_k_param->edge_bypass = 1;
	isp_k_param->hist_bypass = 1;
	isp_k_param->iircnr_bypass = 1;
	isp_k_param->nf_bypass = 1;
	isp_k_param->nr3_bypass = 1;
	isp_k_param->postcdn_bypass = 1;
	isp_k_param->precdn_bypass = 1;
	isp_k_param->pstz_bypass = 1;
	isp_k_param->uvd_bypass = 1;
	isp_k_param->ygamma_bypass = 1;
	isp_k_param->ynr_bypass = 1;
	isp_k_param->yrandom_bypass = 1;

	return 0;
}

/* offline process frame */
static int sprd_isp_proc_frame(void *isp_handle, void *param, int ctx_id)
{
	int ret = 0;
	struct camera_frame *pframe;
	static int slw_frm_cnt;
	struct isp_pipe_context *pctx;
	struct isp_pipe_dev *dev;
	struct isp_stream_ctrl *stream = NULL;
	struct isp_offline_param *in_param = NULL;

	if (!isp_handle || !param) {
		pr_err("fail to get valid input ptr, isp_handle %p, param %p\n",
			isp_handle, param);
		return -EFAULT;
	}
	if (ctx_id < 0 || ctx_id >= ISP_CONTEXT_SW_NUM) {
		pr_err("fail to get legal ctx_id %d\n", ctx_id);
		return -EFAULT;
	}

	dev = (struct isp_pipe_dev *)isp_handle;
	pctx = &dev->ctx[ctx_id];
	pframe = (struct camera_frame *)param;
	pframe->priv_data = pctx;

	stream = camera_dequeue_peek(&pctx->stream_ctrl_in_q,
		struct isp_stream_ctrl, list);
	if (stream && stream->state == ISP_STREAM_POST_PROC) {
		if (stream->frame_type == ISP_STREAM_SIGNLE ||
			stream->data_src == ISP_STREAM_SRC_ISP) {
			pr_debug("isp postproc running: frame need write back\n");
			return -EFAULT;
		}
	}

	in_param = (struct isp_offline_param *)pframe->param_data;
	if (in_param) {
		/*preview*/
		isp_update_offline_size(pctx, in_param);
		free_offline_pararm(in_param);
		pframe->param_data = NULL;
	}

	pr_debug("cam%d ctx %d, fid %d, ch_id %d, buf  %d, 3dnr %d , w %d, h %d\n",
		pctx->attach_cam_id, ctx_id, pframe->fid,
		pframe->channel_id, pframe->buf.mfd[0], pctx->mode_3dnr,
		pframe->width, pframe->height);

	ret = camera_enqueue(&pctx->in_queue, &pframe->list);
	if (ret == 0) {
		isp_get_stream_state(pctx);
		if (pctx->enable_slowmotion && ++slw_frm_cnt < pctx->slowmotion_count)
			return ret;
		complete(&pctx->thread.thread_com);
		slw_frm_cnt = 0;
	}

	return ret;
}

/*
 * Get a free context and initialize it.
 * Input param is possible max_size of image.
 * Return valid context id, or -1 for failure.
 */
static int sprd_isp_get_context(void *isp_handle, void *param)
{
	int ret = 0;
	int i;
	int sel_ctx_id = -1;
	enum  isp_context_id ctx_id = 0;
	struct isp_pipe_context *pctx;
	struct isp_pipe_dev *dev = NULL;
	struct isp_path_desc *path = NULL;
	struct cam_hw_info *hw = NULL;
	struct isp_cfg_ctx_desc *cfg_desc;
	struct isp_init_param *init_param;

	if (!isp_handle || !param) {
		pr_err("fail to get valid input ptr, isp_handle %p, param %p\n",
			isp_handle, param);
		return -EFAULT;
	}
	pr_debug("start.\n");

	dev = (struct isp_pipe_dev *)isp_handle;
	hw = dev->isp_hw;
	init_param = (struct isp_init_param *)param;

	mutex_lock(&dev->path_mutex);

	/* ISP cfg mode, get free context */
	for (i = 0; i < ISP_CONTEXT_SW_NUM; i++) {
		ctx_id = i;
		pctx = &dev->ctx[ctx_id];
		if (atomic_inc_return(&pctx->user_cnt) == 1) {
			sel_ctx_id = ctx_id;
			pr_info("sw %d, get context\n", pctx->ctx_id);
			break;
		}
		atomic_dec(&pctx->user_cnt);
	}

	if (sel_ctx_id == -1)
		goto exit;

	if (dev->wmode == ISP_CFG_MODE) {
		cfg_desc = (struct isp_cfg_ctx_desc *)dev->cfg_handle;
		cfg_desc->ops->ctx_reset(cfg_desc, ctx_id);
	}

	for (i = 0; i < ISP_SPATH_NUM; i++) {
		path = &pctx->isp_path[i];
		path->spath_id = i;
		path->attach_ctx = pctx;
		path->q_init = 0;
		path->hw = hw;
		atomic_set(&path->user_cnt, 0);
	}

	mutex_init(&pctx->param_mutex);
	mutex_init(&pctx->blkpm_lock);
	init_completion(&pctx->frm_done);
	init_completion(&pctx->slice_done);
	/* complete for first frame config */
	complete(&pctx->frm_done);
	isp_blk_pm_init(&pctx->isp_k_param);

	/* bypass fbd_raw by default */
	pctx->fbd_raw.fetch_fbd_bypass = 1;
	pctx->multi_slice = 0;
	pctx->started = 0;
	pctx->uframe_sync = 0;
	pctx->attach_cam_id = init_param->cam_id;
	pctx->ltm_ctx.ltm_index = pctx->attach_cam_id;
	pctx->enable_slowmotion = 0;
	pctx->postproc_func = isp_postproc_irq;
	if (init_param->is_high_fps)
		pctx->enable_slowmotion = hw->ip_isp->slm_cfg_support;;
	pr_info("cam%d isp slowmotion eb %d\n",
		pctx->attach_cam_id, pctx->enable_slowmotion);

	pctx->isp_k_param.nlm_buf = vzalloc(sizeof(uint32_t) * ISP_VST_IVST_NUM2);
	if (pctx->isp_k_param.nlm_buf == NULL) {
		pr_err("fail to alloc nlm buf\n");
		mutex_unlock(&dev->path_mutex);
		return -ENOMEM;
	}
	ret = isp_create_offline_thread(pctx);
	if (unlikely(ret != 0)) {
		pr_err("fail to create offline thread for isp cxt:%d\n",
				pctx->ctx_id);
		goto thrd_err;
	}

	if (init_param->is_high_fps == 0) {
		camera_queue_init(&pctx->in_queue,
			ISP_IN_Q_LEN, isp_ret_src_frame);
		camera_queue_init(&pctx->proc_queue,
			ISP_PROC_Q_LEN, isp_ret_src_frame);
	} else {
		camera_queue_init(&pctx->in_queue,
			ISP_SLW_IN_Q_LEN, isp_ret_src_frame);
		camera_queue_init(&pctx->proc_queue,
			ISP_SLW_PROC_Q_LEN, isp_ret_src_frame);
	}

	camera_queue_init(&pctx->stream_ctrl_in_q,
		ISP_STREAM_STATE_Q_LEN, put_empty_state);
	camera_queue_init(&pctx->stream_ctrl_proc_q,
		ISP_STREAM_STATE_Q_LEN, put_empty_state);
	camera_queue_init(&pctx->hist2_result_queue, 16,
		isp_destroy_statis_buf);

	hw->hw_ops.core_ops.default_para_set(hw, &pctx->ctx_id, ISP_CFG_PARA);
	reset_isp_irq_sw_cnt(pctx->ctx_id);

	goto exit;

thrd_err:
	atomic_dec(&pctx->user_cnt); /* free context */
	sel_ctx_id = -1;
exit:
	mutex_unlock(&dev->path_mutex);
	pr_info("success to get context id %d\n", sel_ctx_id);
	return sel_ctx_id;
}

/*
 * Free a context and deinitialize it.
 * All paths of this context should be put before this.
 *
 * TODO:
 * we do not stop or reset ISP here because four contexts share it.
 * How to make sure current context process in ISP is done
 * before we clear buffer Q?
 * If ISP hw doesn't done buffer reading/writting,
 * we free buffer here may cause memory over-writting and perhaps system crash.
 * Delay buffer Q clear is a just solution to reduce this risk
 */
static int sprd_isp_put_context(void *isp_handle, int ctx_id)
{
	int ret = 0;
	int i;
	uint32_t loop = 0;
	struct isp_pipe_dev *dev;
	struct isp_pipe_context *pctx;
	struct isp_path_desc *path;

	if (!isp_handle) {
		pr_err("fail to get valid input ptr\n");
		return -EFAULT;
	}
	if (ctx_id < 0 || ctx_id >= ISP_CONTEXT_SW_NUM) {
		pr_err("fail to get legal ctx_id %d\n", ctx_id);
		return -EFAULT;
	}

	dev = (struct isp_pipe_dev *)isp_handle;
	pctx = &dev->ctx[ctx_id];

	mutex_lock(&dev->path_mutex);
	if (pctx->isp_k_param.nlm_buf) {
		vfree(pctx->isp_k_param.nlm_buf);
		pctx->isp_k_param.nlm_buf = NULL;
	}

	if (atomic_read(&pctx->user_cnt) == 1)
		isp_stop_offline_thread(&pctx->thread);

	if (atomic_dec_return(&pctx->user_cnt) == 0) {
		pctx->started = 0;
		pr_info("free context %d without users.\n", pctx->ctx_id);

		/* make sure irq handler exit to avoid crash */
		while (pctx->in_irq_handler && (loop < 1000)) {
			pr_info("cxt % in irq. wait %d", pctx->ctx_id, loop);
			loop++;
			udelay(500);
		};

		if (pctx->slice_ctx)
			put_isp_slice_ctx(&pctx->slice_ctx);

		camera_queue_clear(&pctx->in_queue, struct camera_frame, list);
		camera_queue_clear(&pctx->proc_queue, struct camera_frame, list);
		camera_queue_clear(&pctx->hist2_result_queue,
			struct camera_frame, list);
		camera_queue_clear(&pctx->stream_ctrl_in_q,
			struct isp_stream_ctrl, list);
		camera_queue_clear(&pctx->stream_ctrl_proc_q,
			struct isp_stream_ctrl, list);

		dev->ltm_handle->ops->set_status(0, ctx_id, pctx->mode_ltm,
						pctx->attach_cam_id);
		if (pctx->mode_ltm == MODE_LTM_PRE) {
			if (pctx->ltm_rgb) {
				dev->ltm_handle->ops->clear_status(pctx->ltm_ctx.ltm_index);
				dev->ltm_handle->ops->complete_completion(LTM_RGB,
					pctx->ltm_ctx.ltm_index);
				for (i = 0; i < ISP_LTM_BUF_NUM; i++) {
					if (pctx->ltm_buf[LTM_RGB][i]) {
						isp_unmap_frame(pctx->ltm_buf[LTM_RGB][i]);
						pctx->ltm_buf[LTM_RGB][i] = NULL;
					}
				}
			}

			if (pctx->ltm_yuv) {
				dev->ltm_handle->ops->clear_status(pctx->ltm_ctx.ltm_index);
				dev->ltm_handle->ops->complete_completion(LTM_YUV,
					pctx->ltm_ctx.ltm_index);
				for (i = 0; i < ISP_LTM_BUF_NUM; i++) {
					if (pctx->ltm_buf[LTM_YUV][i]) {
						isp_unmap_frame(pctx->ltm_buf[LTM_YUV][i]);
						pctx->ltm_buf[LTM_YUV][i] = NULL;
					}
				}
			}
		}

		if (pctx->mode_ltm == MODE_LTM_CAP) {
			if (pctx->ltm_rgb) {
				dev->ltm_handle->ops->clear_status(pctx->ltm_ctx.ltm_index);
				for (i = 0; i < ISP_LTM_BUF_NUM; i++) {
					if (pctx->ltm_buf[LTM_RGB][i])
						pctx->ltm_buf[LTM_RGB][i] = NULL;
				}
			}

			if (pctx->ltm_yuv) {
				dev->ltm_handle->ops->clear_status(pctx->ltm_ctx.ltm_index);
				for (i = 0; i < ISP_LTM_BUF_NUM; i++) {
					if (pctx->ltm_buf[LTM_YUV][i])
						pctx->ltm_buf[LTM_YUV][i] = NULL;
				}
			}
		}

		for (i = 0; i < ISP_NR3_BUF_NUM; i++) {
			if (pctx->nr3_buf[i]) {
				isp_unmap_frame(pctx->nr3_buf[i]);
				pctx->nr3_buf[i] = NULL;
			}
		}

		/* clear path queue. */
		for (i = 0; i < ISP_SPATH_NUM; i++) {
			path = &pctx->isp_path[i];
			if (path->q_init == 0)
				continue;

			/* reserved buffer queue should be cleared at last. */
			camera_queue_clear(&path->result_queue,
				struct camera_frame, list);
			camera_queue_clear(&path->out_buf_queue,
				struct camera_frame, list);
			camera_queue_clear(&path->reserved_buf_queue,
				struct camera_frame, list);
		}

		if (pctx->postproc_buf) {
			isp_unmap_frame(pctx->postproc_buf);
			pctx->postproc_buf = NULL;
			pr_info("sw %d, superzoom out buffer unmap\n", pctx->ctx_id);
		}

		pctx->postproc_func = NULL;
		pctx->isp_cb_func = NULL;
		pctx->cb_priv_data = NULL;
		trace_isp_irq_sw_cnt(pctx->ctx_id);
		isp_unmap_statis_buffer(isp_handle, ctx_id);
	} else {
		pr_debug("ctx%d.already release. \n", ctx_id);
		atomic_set(&pctx->user_cnt, 0);
	}
	memset(pctx, 0, sizeof(struct isp_pipe_context));
	pctx->ctx_id = ctx_id;
	pctx->dev = dev;
	pctx->attach_cam_id = CAM_ID_MAX;
	pctx->hw = dev->isp_hw;

	mutex_unlock(&dev->path_mutex);
	pr_info("done, put ctx_id: %d\n", ctx_id);
	return ret;
}

/*
 * Enable path of sepicific path_id for specific context.
 * The context must be enable before get_path.
 */
static int sprd_isp_get_path(void *isp_handle, int ctx_id, int path_id)
{
	struct isp_pipe_context *pctx;
	struct isp_pipe_dev *dev;
	struct isp_path_desc *path;

	if (!isp_handle) {
		pr_err("fail to get valid input ptr\n");
		return -EFAULT;
	}

	pr_debug("start.\n");
	if (ctx_id < 0 || ctx_id >= ISP_CONTEXT_SW_NUM ||
		path_id < 0 || path_id >= ISP_SPATH_NUM) {
		pr_err("fail to get legal ctx_id %d, path_id %d\n", ctx_id, path_id);
		return -EFAULT;
	}

	dev = (struct isp_pipe_dev *)isp_handle;
	pctx = &dev->ctx[ctx_id];

	mutex_lock(&dev->path_mutex);

	if (atomic_read(&pctx->user_cnt) < 1) {
		mutex_unlock(&dev->path_mutex);
		pr_err("fail to get path from free context.\n");
		return -EFAULT;
	}

	path = &pctx->isp_path[path_id];
	if (atomic_inc_return(&path->user_cnt) > 1) {
		mutex_unlock(&dev->path_mutex);
		atomic_dec(&path->user_cnt);
		pr_err("fail to get used path %d of cxt %d.\n",
			path_id, ctx_id);
		return -EFAULT;
	}

	mutex_unlock(&dev->path_mutex);

	path->frm_cnt = 0;
	atomic_set(&path->store_cnt, 0);

	if (path->q_init == 0) {
		if (pctx->enable_slowmotion)
			camera_queue_init(&path->result_queue,
				ISP_SLW_RESULT_Q_LEN, isp_ret_out_frame);
		else
			camera_queue_init(&path->result_queue,
				ISP_RESULT_Q_LEN, isp_ret_out_frame);
		camera_queue_init(&path->out_buf_queue, ISP_OUT_BUF_Q_LEN,
			isp_ret_out_frame);
		camera_queue_init(&path->reserved_buf_queue,
			ISP_RESERVE_BUF_Q_LEN, isp_destroy_reserved_buf);
		path->q_init = 1;
	}

	pr_info("get path %d done.", path_id);
	return 0;
}

/*
 * Disable path of sepicific path_id for specific context.
 * The path and context should be enable before this.
 */
static int sprd_isp_put_path(void *isp_handle, int ctx_id, int path_id)
{
	int ret = 0;
	struct isp_pipe_context *pctx;
	struct isp_pipe_dev *dev;
	struct isp_path_desc *path;

	if (!isp_handle) {
		pr_err("fail to get valid input ptr\n");
		return -EFAULT;
	}

	pr_debug("start.\n");
	if (ctx_id < 0 || ctx_id >= ISP_CONTEXT_SW_NUM ||
		path_id < 0 || path_id >= ISP_SPATH_NUM) {
		pr_err("fail to get legal ctx_id %d, path_id %d\n",
			ctx_id, path_id);
		return -EFAULT;
	}

	dev = (struct isp_pipe_dev *)isp_handle;
	pctx = &dev->ctx[ctx_id];

	mutex_lock(&dev->path_mutex);

	if (atomic_read(&pctx->user_cnt) < 1) {
		mutex_unlock(&dev->path_mutex);
		pr_err("fail to free path for free context.\n");
		return -EFAULT;
	}

	path = &pctx->isp_path[path_id];

	if (atomic_read(&path->user_cnt) == 0) {
		mutex_unlock(&dev->path_mutex);
		pr_err("fail to use free isp cxt %d, path %d.\n",
					ctx_id, path_id);
		return -EFAULT;
	}

	if (atomic_dec_return(&path->user_cnt) != 0) {
		pr_warn("isp cxt %d, path %d has multi users.\n",
					ctx_id, path_id);
		atomic_set(&path->user_cnt, 0);
	}

	mutex_unlock(&dev->path_mutex);
	pr_info("done, put path_id: %d for ctx %d\n", path_id, ctx_id);
	return ret;
}

static int sprd_isp_cfg_path(void *isp_handle,
			enum isp_path_cfg_cmd cfg_cmd,
			int ctx_id, int path_id,
			void *param)
{
	int ret = 0;
	int i, j;
	struct isp_pipe_context *pctx;
	struct isp_pipe_dev *dev;
	struct isp_path_desc *path = NULL;
	struct isp_path_desc *slave_path;
	struct camera_frame *pframe = NULL;

	if (!isp_handle || !param) {
		pr_err("fail to get valid input ptr, isp_handle %p, param %p\n",
			isp_handle, param);
		return -EFAULT;
	}

	if (ctx_id >= ISP_CONTEXT_SW_NUM  || ctx_id < ISP_CONTEXT_P0) {
		pr_err("fail to get legal id ctx %d\n", ctx_id);
		return -EFAULT;
	}
	if (path_id >= ISP_SPATH_NUM) {
		pr_err("fail to use legal id path %d\n", path_id);
		return -EFAULT;
	}
	dev = (struct isp_pipe_dev *)isp_handle;
	pctx = &dev->ctx[ctx_id];
	path = &pctx->isp_path[path_id];

	if ((cfg_cmd != ISP_PATH_CFG_CTX_BASE) &&
		(cfg_cmd != ISP_PATH_CFG_CTX_SIZE) &&
		(cfg_cmd != ISP_PATH_CFG_CTX_COMPRESSION)) {
		if (atomic_read(&path->user_cnt) == 0) {
			pr_err("fail to use free isp cxt %d, path %d.\n",
					ctx_id, path_id);
			return -EFAULT;
		}
	}

	switch (cfg_cmd) {
	case ISP_PATH_CFG_OUTPUT_RESERVED_BUF:
		pframe = (struct camera_frame *)param;
		ret = cambuf_iommu_map_single_page(
				&pframe->buf, CAM_IOMMUDEV_ISP);
		if (ret) {
			pr_err("fail to map isp iommu buf.\n");
			ret = -EINVAL;
			goto exit;
		}
		pr_debug("cfg buf path %d, %p\n",
			path->spath_id, pframe);

		/* is_reserved:
		 *  1:  basic mapping reserved buffer;
		 *  2:  copy of reserved buffer.
		 */
		if (unlikely(cfg_cmd == ISP_PATH_CFG_OUTPUT_RESERVED_BUF)) {
			struct camera_frame *newfrm;

			pframe->is_reserved = 1;
			pframe->priv_data = path;
			path->reserved_buf_fd = pframe->buf.mfd[0];
			for (j = 0; j < 3; j++)
				path->reserve_buf_size[j] = pframe->buf.size[j];
			pr_info("reserved buf\n");
			ret = camera_enqueue(&path->reserved_buf_queue,
				&pframe->list);
			i = 1;
			while (i < ISP_RESERVE_BUF_Q_LEN) {
				newfrm = get_empty_frame();
				if (newfrm) {
					newfrm->is_reserved = 2;
					newfrm->priv_data = path;
					newfrm->user_fid = pframe->user_fid;
					newfrm->channel_id = pframe->channel_id;
					memcpy(&newfrm->buf,
						&pframe->buf,
						sizeof(pframe->buf));
					camera_enqueue(
						&path->reserved_buf_queue,
						&newfrm->list);
					i++;
				}
			}
		}
		break;
	case ISP_PATH_CFG_OUTPUT_BUF:
		pframe = (struct camera_frame *)param;
		pr_debug("output buf\n");
		pframe->is_reserved = 0;
		pframe->priv_data = path;
		ret = camera_enqueue(&path->out_buf_queue, &pframe->list);
		if (ret) {
			pr_err("fail to enqueue output buffer, path %d.\n",
					path_id);
			goto exit;
		}
		break;
	case ISP_PATH_CFG_3DNR_BUF:
		pframe = (struct camera_frame *)param;
		ret = cambuf_iommu_map(
				&pframe->buf, CAM_IOMMUDEV_ISP);
		if (ret) {
			pr_err("fail to map isp iommu buf.\n");
			ret = -EINVAL;
			goto exit;
		}
		for (i = 0; i < ISP_NR3_BUF_NUM; i++) {
			if (pctx->nr3_buf[i] == NULL) {
				pctx->nr3_buf[i] = pframe;
				pctx->nr3_ctx.buf_info[i] =
					&pctx->nr3_buf[i]->buf;
				pr_debug("3DNR CFGB[%d][0x%p][0x%p] = 0x%lx, 0x%lx\n",
					 i, pframe, pctx->nr3_buf[i],
					 pctx->nr3_ctx.buf_info[i]->iova[0],
					 pctx->nr3_buf[i]->buf.iova[0]);
				break;
			} else {
				pr_debug("3DNR CFGB[%d][0x%p][0x%p] failed\n",
				       i, pframe, pctx->nr3_buf[i]);
			}
		}
		if (i == 2) {
			pr_err("fail to set isp ctx %d nr3 buffers.\n", ctx_id);
			cambuf_iommu_unmap(&pframe->buf);
			goto exit;
		}
		break;
	case ISP_PATH_CFG_RGB_LTM_BUF:
		pframe = (struct camera_frame *)param;
		if (pctx->mode_ltm == MODE_LTM_PRE) {
			ret = cambuf_iommu_map(
				 &pframe->buf, CAM_IOMMUDEV_ISP);
			if (ret) {
				pr_err("fail to isp map iommu buf.\n");
				ret = -EINVAL;
				goto exit;
			}
		}
		for (i = 0; i < ISP_LTM_BUF_NUM; i++) {
			if (pctx->ltm_buf[LTM_RGB][i] == NULL) {
				pctx->ltm_buf[LTM_RGB][i] = pframe;
				pctx->ltm_ctx.pbuf[LTM_RGB][i] = &pframe->buf;
				pr_debug("LTM CFGB[%d][0x%p][0x%p] = 0x%lx, 0x%lx\n",
					 i, pframe, pctx->ltm_buf[LTM_RGB][i],
					 pctx->ltm_ctx.pbuf[LTM_RGB][i]->iova[0],
					 pctx->ltm_buf[LTM_RGB][i]->buf.iova[0]);
				break;
			}
		}
		pr_debug("isp ctx [%d], ltm buf idx [%d], buf addr [0x%p]\n",
			pctx->ctx_id, i, pframe);
		break;
	case ISP_PATH_CFG_YUV_LTM_BUF:
		pframe = (struct camera_frame *)param;
		if (pctx->mode_ltm == MODE_LTM_PRE) {
			ret = cambuf_iommu_map(
				 &pframe->buf, CAM_IOMMUDEV_ISP);
			if (ret) {
				pr_err("fail to isp map iommu buf.\n");
				ret = -EINVAL;
				goto exit;
			}
		}
		for (i = 0; i < ISP_LTM_BUF_NUM; i++) {
			if (pctx->ltm_buf[LTM_YUV][i] == NULL) {
				pctx->ltm_buf[LTM_YUV][i] = pframe;
				pctx->ltm_ctx.pbuf[LTM_YUV][i] = &pframe->buf;
				pr_debug("LTM CFGB[%d][0x%p][0x%p] = 0x%lx, 0x%lx\n",
					 i, pframe, pctx->ltm_buf[LTM_YUV][i],
					 pctx->ltm_ctx.pbuf[LTM_YUV][i]->iova[0],
					 pctx->ltm_buf[LTM_YUV][i]->buf.iova[0]);
				break;
			}
		}
		pr_debug("isp ctx [%d], ltm buf idx [%d], buf addr [0x%p]\n",
			pctx->ctx_id, i, pframe);
		break;
	case ISP_PATH_CFG_POSTPROC_BUF:
		pframe = (struct camera_frame *)param;
		pctx->postproc_buf = pframe;
		pr_debug("isp %d postproc buffer %d.\n", pctx->ctx_id,
			pframe->buf.mfd[0]);
		break;
	case ISP_PATH_CFG_CTX_BASE:
		ret = isp_cfg_ctx_base(pctx, param);
		pctx->updated = 1;
		break;
	case ISP_PATH_CFG_CTX_SIZE:
		mutex_lock(&pctx->param_mutex);
		ret = isp_cfg_ctx_size(pctx, param);
		pctx->src_info.src = pctx->input_size;
		pctx->src_info.src_crop = pctx->input_trim;
		pctx->updated = 1;
		mutex_unlock(&pctx->param_mutex);
		break;
	case ISP_PATH_CFG_CTX_COMPRESSION:
		ret = isp_cfg_ctx_compression(pctx, param);
		break;
	case ISP_PATH_CFG_CTX_UFRAME_SYNC:
		ret = isp_cfg_ctx_uframe_sync(pctx, param);
		break;
	case ISP_PATH_CFG_PATH_BASE:
		ret = isp_cfg_path_base(path, param);
		pctx->updated = 1;
		break;
	case ISP_PATH_CFG_PATH_SIZE:
		mutex_lock(&pctx->param_mutex);
		ret = isp_cfg_path_size(path, param);
		if (path->bind_type == ISP_PATH_MASTER) {
			slave_path = &pctx->isp_path[path->slave_path_id];
			ret = isp_cfg_path_size(slave_path, param);
			slave_path->stream_in_trim = slave_path->in_trim;
		}
		path->stream_in_trim = path->in_trim;
		pctx->updated = 1;
		mutex_unlock(&pctx->param_mutex);
		break;
	case ISP_PATH_CFG_PATH_COMPRESSION:
		ret = isp_cfg_path_compression(path, param);
		break;

	case ISP_PATH_CFG_PATH_UFRAME_SYNC:
		ret = isp_cfg_path_uframe_sync(path, param);
		break;
	case ISP_PATH_CFG_3DNR_MODE:
		mutex_lock(&pctx->param_mutex);
		pctx->mode_3dnr = *(uint32_t *)param;
		mutex_unlock(&pctx->param_mutex);
		break;
	default:
		pr_warn("unsupported cmd: %d\n", cfg_cmd);
		break;
	}
exit:
	pr_debug("cfg path %d done. ret %d\n", path->spath_id, ret);
	return ret;
}

static int isp_cfg_statis_buffer(
		void *isp_handle, int ctx_id,
		struct isp_statis_buf_input *input)
{
	int ret = 0;
	int j;
	int32_t mfd;
	uint32_t offset;
	enum isp_statis_buf_type stats_type = STATIS_HIST2;
	struct isp_pipe_dev *dev = NULL;
	struct isp_pipe_context *pctx;
	struct camera_buf *ion_buf = NULL;
	struct camera_frame *pframe;

	if (!isp_handle || ctx_id < 0 || ctx_id >= ISP_CONTEXT_SW_NUM) {
		pr_info("isp_handle=%p, cxt_id=%d\n", isp_handle, ctx_id);
		return 0;
	}

	dev = (struct isp_pipe_dev *)isp_handle;
	pctx = &dev->ctx[ctx_id];

	if (atomic_read(&pctx->user_cnt) == 0) {
		pr_info("isp ctx %d is not enable\n", ctx_id);
		return 0;
	}

	if (input->type == STATIS_HIST2)
		goto cfg_single;

	memset(&pctx->statis_buf_array[0], 0, sizeof(pctx->statis_buf_array));
	stats_type = STATIS_HIST2;

	for (j = 0; j < STATIS_BUF_NUM_MAX; j++) {
		mfd = input->mfd_array[stats_type][j];
		offset = input->offset_array[stats_type][j];
		if (mfd <= 0)
			continue;
		ion_buf = &pctx->statis_buf_array[j];
		ion_buf->mfd[0] = mfd;
		ion_buf->offset[0] = offset;
		ion_buf->type = CAM_BUF_USER;
		ret = cambuf_get_ionbuf(ion_buf);
		if (ret) {
			memset(ion_buf, 0, sizeof(struct camera_buf));
			continue;
		}

		ret = cambuf_kmap(ion_buf);
		if (ret) {
			cambuf_put_ionbuf(ion_buf);
			memset(ion_buf, 0, sizeof(struct camera_buf));
			continue;
		}

		pr_debug("stats %d, j %d, buf %d, offset %d,  kaddr 0x%lx\n",
				stats_type, j, mfd, offset, ion_buf->addr_k[0]);
	}

	isp_init_statis_q(isp_handle, ctx_id);

	pr_info("init done\n");
	return 0;

cfg_single:
	for (j = 0; j < STATIS_BUF_NUM_MAX; j++) {
		mfd = pctx->statis_buf_array[j].mfd[0];
		offset = pctx->statis_buf_array[j].offset[0];
		if ((mfd > 0) && (mfd == input->mfd)
			&& (offset == input->offset)) {
			ion_buf = &pctx->statis_buf_array[j];
			break;
		}
	}

	if (ion_buf == NULL) {
		pr_err("fail to get statis buf %d, type %d\n",
				input->type, input->mfd);
		ret = -EINVAL;
		return ret;
	}

	pframe = get_empty_frame();
	pframe->channel_id = pctx->ch_id;
	pframe->irq_property = STATIS_HIST2;
	pframe->buf = *ion_buf;
	ret = camera_enqueue(&pctx->hist2_result_queue, &pframe->list);
	if (ret) {
		pr_info("statis %d overflow\n", stats_type);
		put_empty_frame(pframe);
	}
	pr_debug("buf %d, off %d, kaddr 0x%lx iova 0x%08x\n",
		ion_buf->mfd[0], ion_buf->offset[0],
		ion_buf->addr_k[0], (uint32_t)ion_buf->iova[0]);
	return 0;
}

static int isp_init_statis_q(void *isp_handle, int ctx_id)
{
	int ret = 0;
	int j;
	enum isp_statis_buf_type stats_type;
	struct isp_pipe_dev *dev = NULL;
	struct isp_pipe_context *pctx;
	struct camera_buf *ion_buf;
	struct camera_frame *pframe;

	dev = (struct isp_pipe_dev *)isp_handle;
	pctx = &dev->ctx[ctx_id];

	stats_type = STATIS_HIST2;
	for (j = 0; j < STATIS_BUF_NUM_MAX; j++) {
		ion_buf = &pctx->statis_buf_array[j];
		if (ion_buf->mfd[0] <= 0) {
			memset(ion_buf, 0, sizeof(struct camera_buf));
			continue;
		}

		pframe = get_empty_frame();
		pframe->channel_id = pctx->ch_id;
		pframe->irq_property = stats_type;
		pframe->buf = *ion_buf;

		ret = camera_enqueue(&pctx->hist2_result_queue, &pframe->list);
		if (ret) {
			pr_info("statis %d overflow\n", stats_type);
			put_empty_frame(pframe);
		}
		pr_debug("buf %d, off %d, kaddr 0x%lx iova 0x%08x\n",
			ion_buf->mfd[0], ion_buf->offset[0],
			ion_buf->addr_k[0], (uint32_t)ion_buf->iova[0]);
	}

	return ret;
}

static int isp_unmap_statis_buffer(void *isp_handle, int ctx_id)
{
	int ret = 0;
	int j;
	struct isp_pipe_dev *dev = NULL;
	struct isp_pipe_context *pctx;
	struct camera_buf *ion_buf;

	dev = (struct isp_pipe_dev *)isp_handle;
	pctx = &dev->ctx[ctx_id];

	pr_debug("enter\n");

	for (j = 0; j < STATIS_BUF_NUM_MAX; j++) {
		ion_buf = &pctx->statis_buf_array[j];
		if (ion_buf->mfd[0] <= 0) {
			memset(ion_buf, 0, sizeof(struct camera_buf));
			continue;
		}

		pr_info("ctx %d free buf %d, off %d, kaddr 0x%lx iova 0x%08x\n",
			ctx_id, ion_buf->mfd[0], ion_buf->offset[0],
			ion_buf->addr_k[0], (uint32_t)ion_buf->iova[0]);

		cambuf_kunmap(ion_buf);
		cambuf_put_ionbuf(ion_buf);
		memset(ion_buf, 0, sizeof(struct camera_buf));
	}

	pr_debug("done.\n");
	return ret;
}

static int sprd_isp_cfg_sec(struct isp_pipe_dev *dev, void *param)
{

	enum sprd_cam_sec_mode  *sec_mode = (enum sprd_cam_sec_mode *)param;

	dev->sec_mode =  *sec_mode;

	pr_debug("camca: isp sec_mode=%d\n", dev->sec_mode);

	return 0;
}

static int sprd_isp_ioctl(void *isp_handle, int ctx_id,
	enum isp_ioctrl_cmd cmd, void *param)
{
	int ret = 0;
	struct isp_pipe_dev *dev = NULL;

	if (!isp_handle) {
		pr_err("fail to get valid input ptr\n");
		return -EFAULT;
	}
	dev = (struct isp_pipe_dev *)isp_handle;

	switch (cmd) {
	case ISP_IOCTL_CFG_STATIS_BUF:
		ret = isp_cfg_statis_buffer(isp_handle, ctx_id, param);
		break;
	case ISP_IOCTL_CFG_SEC:
		ret = sprd_isp_cfg_sec(dev, param);
		break;
	default:
		pr_err("fail to get known cmd: %d\n", cmd);
		ret = -EFAULT;
		break;
	}

	return ret;
}

static int sprd_isp_cfg_blkparam(
	void *isp_handle, int ctx_id, void *param)
{
	int ret = 0;
	int i;
	struct isp_pipe_context *pctx = NULL;
	struct isp_pipe_dev *dev = NULL;
	struct isp_cfg_entry *cfg_entry = NULL;
	struct isp_io_param *io_param = NULL;
	func_isp_cfg_param cfg_fun_ptr = NULL;
	struct cam_hw_core_ops *ops = NULL;

	if (!isp_handle || !param) {
		pr_err("fail to get valid input ptr, isp_handle %p, param %p\n",
			isp_handle, param);
		return -EFAULT;
	}
	if (ctx_id < 0 || ctx_id >= ISP_CONTEXT_SW_NUM) {
		pr_err("fail to get legal ctx_id %d\n", ctx_id);
		return -EINVAL;
	}

	dev = (struct isp_pipe_dev *)isp_handle;
	ops = &dev->isp_hw->hw_ops.core_ops;
	pctx = &dev->ctx[ctx_id];
	io_param = (struct isp_io_param *)param;
	mutex_lock(&dev->path_mutex);

	if (atomic_read(&pctx->user_cnt) < 1) {
		pr_err("fail to use unable isp ctx %d.\n", ctx_id);
		mutex_unlock(&dev->path_mutex);
		return -EINVAL;
	}

	/* lock to avoid block param across frame */
	mutex_lock(&pctx->blkpm_lock);
	if (io_param->sub_block == ISP_BLOCK_NLM) {
		ret = isp_k_cfg_nlm(param, &pctx->isp_k_param, ctx_id);
	} else if (io_param->sub_block == ISP_BLOCK_3DNR) {
		if (pctx->mode_3dnr != MODE_3DNR_OFF)
			ret = isp_k_cfg_3dnr(param, &pctx->isp_k_param, ctx_id);
	} else if (io_param->sub_block == ISP_BLOCK_YNR) {
		ret = isp_k_cfg_ynr(param, &pctx->isp_k_param, ctx_id);
	} else if (io_param->sub_block == ISP_BLOCK_NOISEFILTER
			&& io_param->scene_id == PM_SCENE_CAP) {
		ret = isp_k_cfg_yuv_noisefilter(param, &pctx->isp_k_param, ctx_id);
	} else if (io_param->sub_block == ISP_BLOCK_RGB_LTM) {
		ret = isp_k_cfg_rgb_ltm(param, &pctx->isp_k_param, ctx_id);
	} else if (io_param->sub_block == ISP_BLOCK_YUV_LTM) {
		ret = isp_k_cfg_yuv_ltm(param, &pctx->isp_k_param, ctx_id);
	} else {
		i = io_param->sub_block - ISP_BLOCK_BASE;
		cfg_entry = ops->block_func_get(i, ISP_BLOCK_TYPE);
		if (cfg_entry != NULL &&
			cfg_entry->sub_block == io_param->sub_block)
			cfg_fun_ptr = cfg_entry->cfg_func;

		if (cfg_fun_ptr == NULL) {
			pr_debug("isp block 0x%x is not supported.\n",
				io_param->sub_block);
			mutex_unlock(&pctx->blkpm_lock);
			mutex_unlock(&dev->path_mutex);
			return 0;
		}

		ret = cfg_entry->cfg_func(io_param, &pctx->isp_k_param, ctx_id);
	}

	mutex_unlock(&pctx->blkpm_lock);
	mutex_unlock(&dev->path_mutex);

	return ret;
}

static int sprd_isp_set_sb(void *isp_handle, int ctx_id,
		isp_dev_callback cb, void *priv_data)
{
	int ret = 0;
	struct isp_pipe_dev *dev = NULL;
	struct isp_pipe_context *pctx;

	if (!isp_handle || !cb || !priv_data) {
		pr_err("fail to get valid input ptr, isp_handle %p, callback %p, priv_data %p\n",
			isp_handle, cb, priv_data);
		return -EFAULT;
	}
	if (ctx_id < 0 || ctx_id >= ISP_CONTEXT_SW_NUM) {
		pr_err("fail to get legal ctx_id %d\n", ctx_id);
		return -EFAULT;
	}

	dev = (struct isp_pipe_dev *)isp_handle;
	pctx = &dev->ctx[ctx_id];
	if (pctx->isp_cb_func == NULL) {
		pctx->isp_cb_func = cb;
		pctx->cb_priv_data = priv_data;
		pr_info("ctx: %d, cb %p, %p\n", ctx_id, cb, priv_data);
	}

	return ret;
}

static int sprd_isp_dev_open(void *isp_handle, void *param)
{
	int ret = 0;
	struct isp_pipe_dev *dev = NULL;
	struct cam_hw_info *hw = NULL;

	pr_debug("enter.\n");
	if (!isp_handle || !param) {
		pr_err("fail to get valid input ptr, isp_handle %p, param %p\n",
			isp_handle, param);
		return -EFAULT;
	}
	dev = (struct isp_pipe_dev *)isp_handle;
	hw = (struct cam_hw_info *)param;
	if (hw == NULL) {
		pr_err("fail to get valid hw\n");
		return -EFAULT;
	}

	if (atomic_inc_return(&dev->enable) == 1) {
		pr_info("open_start: sec_mode: %d, work mode: %d,  line_buf_len: %d\n",
			dev->sec_mode, dev->wmode, g_camctrl.isp_linebuf_len);

		/* line_buffer_len for debug */
		if (s_dbg_linebuf_len > 0 &&
			s_dbg_linebuf_len <= ISP_LINE_BUFFER_W)
			g_camctrl.isp_linebuf_len = s_dbg_linebuf_len;
		else
			g_camctrl.isp_linebuf_len = ISP_LINE_BUFFER_W;

		if (dev->sec_mode == SEC_SPACE_PRIORITY)
			dev->wmode = ISP_AP_MODE;
		else
			dev->wmode = s_dbg_work_mode;
		g_camctrl.isp_wmode = dev->wmode;

		dev->isp_hw = param;
		mutex_init(&dev->path_mutex);
		spin_lock_init(&dev->ctx_lock);

		ret = isp_hw_init(dev);
		ret = isp_hw_start(hw, dev);
		ret = isp_context_init(dev);
		if (ret) {
			pr_err("fail to init isp context.\n");
			ret = -EFAULT;
			goto err_init;
		}
	}

	pr_info("open isp pipe dev done!\n");
	return 0;

err_init:
	isp_hw_stop(hw, dev);
	isp_hw_deinit(dev);
	atomic_dec(&dev->enable);
	pr_err("fail to open isp dev!\n");
	return ret;
}

int sprd_isp_dev_close(void *isp_handle)
{
	int ret = 0;
	struct isp_pipe_dev *dev = NULL;
	struct cam_hw_info *hw;

	if (!isp_handle) {
		pr_err("fail to get valid input ptr\n");
		return -EINVAL;
	}

	dev = (struct isp_pipe_dev *)isp_handle;
	hw = dev->isp_hw;
	if (atomic_dec_return(&dev->enable) == 0) {

		ret = isp_hw_stop(hw, dev);
		ret = isp_context_deinit(dev);
		mutex_destroy(&dev->path_mutex);
		ret = isp_hw_deinit(dev);
	}

	pr_info("isp dev disable done\n");
	return ret;

}

static int sprd_isp_dev_reset(void *isp_handle, void *param)
{
	int ret = 0;

	if (!isp_handle || !param) {
		pr_err("fail to get valid input ptr, isp_handle %p, param %p\n",
			isp_handle, param);
		return -EFAULT;
	}
	return ret;
}

static struct isp_pipe_ops isp_ops = {
	.open = sprd_isp_dev_open,
	.close = sprd_isp_dev_close,
	.reset = sprd_isp_dev_reset,
	.get_context = sprd_isp_get_context,
	.put_context = sprd_isp_put_context,
	.get_path = sprd_isp_get_path,
	.put_path = sprd_isp_put_path,
	.cfg_path = sprd_isp_cfg_path,
	.ioctl = sprd_isp_ioctl,
	.cfg_blk_param = sprd_isp_cfg_blkparam,
	.proc_frame = sprd_isp_proc_frame,
	.set_callback = sprd_isp_set_sb,
	.clear_stream_ctrl = isp_put_stream_state,
};

struct isp_pipe_ops *get_isp_ops(void)
{
	return &isp_ops;
}

void *get_isp_pipe_dev(void)
{
	struct isp_pipe_dev *dev = NULL;

	mutex_lock(&isp_pipe_dev_mutex);

	if (s_isp_dev) {
		atomic_inc(&s_isp_dev->user_cnt);
		dev = s_isp_dev;
		pr_info("s_isp_dev is already exist=%p, user_cnt=%d",
			s_isp_dev, atomic_read(&s_isp_dev->user_cnt));
		goto exit;
	}

	dev = vzalloc(sizeof(struct isp_pipe_dev));
	if (!dev)
		goto exit;

	atomic_set(&dev->user_cnt, 1);
	atomic_set(&dev->enable, 0);
	s_isp_dev = dev;
	if (dev)
		pr_info("get isp pipe dev: %p\n", dev);
exit:
	mutex_unlock(&isp_pipe_dev_mutex);

	return dev;
}


int put_isp_pipe_dev(void *isp_handle)
{
	int ret = 0;
	int user_cnt, en_cnt;
	struct isp_pipe_dev *dev = NULL;

	if (!isp_handle) {
		pr_err("fail to get valid input ptr\n");
		return -EFAULT;
	}

	dev = (struct isp_pipe_dev *)isp_handle;
	pr_info("put isp pipe dev:%p, s_isp_dev:%p,  users: %d\n",
		dev, s_isp_dev, atomic_read(&dev->user_cnt));

	mutex_lock(&isp_pipe_dev_mutex);

	if (dev != s_isp_dev) {
		mutex_unlock(&isp_pipe_dev_mutex);
		pr_err("fail to match dev: %p, %p\n",
					dev, s_isp_dev);
		return -EINVAL;
	}

	user_cnt = atomic_read(&dev->user_cnt);
	en_cnt = atomic_read(&dev->enable);
	if (user_cnt != (en_cnt + 1))
		pr_err("fail to match %d %d\n",
				user_cnt, en_cnt);

	if (atomic_dec_return(&dev->user_cnt) == 0) {
		pr_info("free isp pipe dev %p\n", dev);
		vfree(dev);
		dev = NULL;
		s_isp_dev = NULL;
	}
	mutex_unlock(&isp_pipe_dev_mutex);

	if (dev)
		pr_info("put isp pipe dev: %p\n", dev);

	return ret;
}

