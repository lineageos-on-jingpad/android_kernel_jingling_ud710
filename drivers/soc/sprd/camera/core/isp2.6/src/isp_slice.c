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

#include <linux/kernel.h>
#include <linux/vmalloc.h>
#include <sprd_mm.h>

#include "isp_reg.h"
#include "isp_core.h"
#include "isp_slice.h"

#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) "ISP_SLICE: %d %d %s : "\
	fmt, current->pid, __LINE__, __func__

#define ISP_SLICE_ALIGN_SIZE           2
#define ISP_ALIGNED(size)              ((size) & ~(ISP_SLICE_ALIGN_SIZE - 1))

struct isp_scaler_slice_tmp {
	uint32_t slice_row_num;
	uint32_t slice_col_num;
	uint32_t start_col;
	uint32_t start_row;
	uint32_t end_col;
	uint32_t end_row;
	uint32_t start_col_orig;
	uint32_t start_row_orig;
	uint32_t end_col_orig;
	uint32_t end_row_orig;
	uint32_t x;
	uint32_t y;
	uint32_t overlap_bad_up;
	uint32_t overlap_bad_down;
	uint32_t overlap_bad_left;
	uint32_t overlap_bad_right;
	uint32_t trim0_end_x;
	uint32_t trim0_end_y;
	uint32_t trim0_start_adjust_x;
	uint32_t trim0_start_adjust_y;
	uint32_t trim1_sum_x;
	uint32_t trim1_sum_y;
	uint32_t deci_x;
	uint32_t deci_y;
	uint32_t deci_x_align;
	uint32_t deci_y_align;
	uint32_t scaler_out_height_temp;
	uint32_t scaler_out_width_temp;
};

static int sprd_ispslice_noisefliter_info_set(struct isp_slice_desc *slc_ctx,
	struct isp_slice_context *ctx)
{
	int rtn = 0,slice_id = 0;
	uint32_t slice_num = 0;
	uint32_t slice_width = 0 ;
	uint32_t seed0 = 0;
	struct isp_slice_desc *cur_slc=slc_ctx;
	struct slice_noisefilter_info *noisefilter_info = NULL;
	struct slice_scaler_info *scaler_info = NULL;

	if (!ctx) {
		pr_err("fail to get valid input ptr\n");
		rtn = -EINVAL;
		goto exit;
	}

	slice_num = ctx->slice_num;
	seed0 = cur_slc->slice_noisefilter_mode.seed_for_mode1;
	pr_debug("shape_mode=%d,slice_num=%d\n",
		cur_slc->slice_noisefilter_mode.yrandom_mode, slice_num);
	if (cur_slc->slice_noisefilter_mode.yrandom_mode == 1) {
		for (slice_id = 0; slice_id < slice_num; slice_id++,cur_slc++) {
			scaler_info = &cur_slc->slice_scaler[0];
			noisefilter_info = &cur_slc->noisefilter_info;
			noisefilter_info->seed0 = seed0;
			slice_width = scaler_info->trim1_size_x;
			sprd_noisefilter_seeds(slice_width,
				noisefilter_info->seed0,&noisefilter_info->seed1,
					&noisefilter_info->seed2, &noisefilter_info->seed3);
			pr_debug("seed0=%d,seed1=%d,seed2=%d,seed3=%d,slice_width=%d\n",
				noisefilter_info->seed0, noisefilter_info->seed1,
					noisefilter_info->seed2, noisefilter_info->seed3, slice_width);
		}
	} else
		return rtn;

exit:
	return rtn;
}

static int get_slice_size_info(
			struct slice_cfg_input *in_ptr,
			uint32_t *w, uint32_t *h)
{
	int rtn = 0;
	uint32_t j;
	uint32_t slice_num, slice_w, slice_w_out;
	uint32_t slice_max_w, max_w;
	uint32_t linebuf_len;
	struct img_size *input = &in_ptr->frame_in_size;
	struct img_size *output;

	/* based input */
	max_w = input->w;
	slice_num = 1;
	linebuf_len = g_camctrl.isp_linebuf_len;
	slice_max_w = linebuf_len - SLICE_OVERLAP_W_MAX;
	if (max_w <= linebuf_len) {
		slice_w = input->w;
	} else {
		do {
			slice_num++;
			slice_w = (max_w + slice_num - 1) / slice_num;
		} while (slice_w >= slice_max_w);
	}
	pr_info("input_w %d, slice_num %d, slice_w %d\n",
		max_w, slice_num, slice_w);

	/* based output */
	max_w = 0;
	slice_num = 1;
	slice_max_w = linebuf_len;
	for (j = 0; j < ISP_SPATH_NUM; j++) {
		output = in_ptr->frame_out_size[j];
		if (output && (output->w > max_w))
			max_w = output->w;
	}
	if (max_w > 0) {
		if (max_w > linebuf_len) {
			do {
				slice_num++;
				slice_w_out = (max_w + slice_num - 1) / slice_num;
			} while (slice_w_out >= slice_max_w);
		}
		/* set to equivalent input size, because slice size based on input. */
		slice_w_out = (input->w + slice_num - 1) / slice_num;
	} else
		slice_w_out = slice_w;
	pr_debug("max output w %d, slice_num %d, out limited slice_w %d\n",
		max_w, slice_num, slice_w_out);

	*w = (slice_w < slice_w_out) ?  slice_w : slice_w_out;
	*h = input->h / SLICE_H_NUM_MAX;

	*w = ISP_ALIGNED(*w);
	*h = ISP_ALIGNED(*h);

	return rtn;
}

static int get_slice_overlap_info(
			struct slice_cfg_input *in_ptr,
			struct isp_slice_context *slc_ctx)
{
	switch (in_ptr->frame_fetch->fetch_fmt) {
	case ISP_FETCH_RAW10:
	case ISP_FETCH_CSI2_RAW10:
		slc_ctx->overlap_up = RAW_OVERLAP_UP;
		slc_ctx->overlap_down = RAW_OVERLAP_DOWN;
		slc_ctx->overlap_left = RAW_OVERLAP_LEFT;
		slc_ctx->overlap_right = RAW_OVERLAP_RIGHT;
		break;
	default:
		slc_ctx->overlap_up = YUV_OVERLAP_UP;
		slc_ctx->overlap_down = YUV_OVERLAP_DOWN;
		slc_ctx->overlap_left = YUV_OVERLAP_LEFT;
		slc_ctx->overlap_right = YUV_OVERLAP_RIGHT;
		break;
	}

	return 0;
}

static int cfg_slice_base_info(
			struct slice_cfg_input *in_ptr,
			struct isp_slice_context *slc_ctx,
			uint32_t *valid_slc_num)
{
	int rtn = 0;
	uint32_t i = 0, j = 0;
	uint32_t img_height, img_width;
	uint32_t slice_height = 0, slice_width = 0;
	uint32_t slice_total_row, slice_total_col, slice_num;
	uint32_t fetch_start_x, fetch_start_y;
	uint32_t fetch_end_x, fetch_end_y;
	struct isp_fetch_info *frame_fetch = in_ptr->frame_fetch;
	struct isp_fbd_raw_info *frame_fbd_raw = in_ptr->frame_fbd_raw;

	struct isp_slice_desc *cur_slc;

	rtn = get_slice_size_info(in_ptr, &slice_width, &slice_height);

	rtn = get_slice_overlap_info(in_ptr, slc_ctx);

	img_height = in_ptr->frame_in_size.h;
	img_width = in_ptr->frame_in_size.w;
	fetch_start_x = frame_fetch->in_trim.start_x;
	fetch_start_y = frame_fetch->in_trim.start_y;
	fetch_end_x = frame_fetch->in_trim.start_x + frame_fetch->in_trim.size_x - 1;
	fetch_end_y = frame_fetch->in_trim.start_y + frame_fetch->in_trim.size_y - 1;
	if (!frame_fbd_raw->fetch_fbd_bypass) {
		fetch_start_x = frame_fbd_raw->trim.start_x;
		fetch_start_y = frame_fbd_raw->trim.start_y;
		fetch_end_x = frame_fbd_raw->trim.start_x
			+ frame_fbd_raw->trim.size_x - 1;
		fetch_end_y = frame_fbd_raw->trim.start_y
			+ frame_fbd_raw->trim.size_y - 1;
	}
	slice_total_row = (img_height + slice_height - 1) / slice_height;
	slice_total_col = (img_width + slice_width - 1) / slice_width;
	slice_num = slice_total_col * slice_total_row;
	slc_ctx->slice_num = slice_num;
	slc_ctx->slice_col_num = slice_total_col;
	slc_ctx->slice_row_num = slice_total_row;
	slc_ctx->slice_height = slice_height;
	slc_ctx->slice_width = slice_width;
	slc_ctx->img_height = img_height;
	slc_ctx->img_width = img_width;
	pr_info("img w %d, h %d, slice w %d, h %d, slice num %d\n",
		img_width, img_height,
		slice_width, slice_height, slice_num);
	if (!frame_fbd_raw->fetch_fbd_bypass)
		pr_debug("src %d %d, fbd_raw crop %d %d %d %d\n",
			frame_fetch->src.w, frame_fetch->src.h,
			fetch_start_x, fetch_start_y, fetch_end_x, fetch_end_y);
	else
		pr_debug("src %d %d, fetch crop %d %d %d %d\n",
			frame_fbd_raw->width, frame_fbd_raw->height,
			fetch_start_x, fetch_start_y, fetch_end_x, fetch_end_y);

	for (i = 0; i < SLICE_NUM_MAX; i++)
		pr_debug("slice %d valid %d\n", i, slc_ctx->slices[i].valid);

	cur_slc = &slc_ctx->slices[0];
	for (i = 0; i < slice_total_row; i++) {
		for (j = 0; j < slice_total_col; j++) {
			uint32_t start_col;
			uint32_t start_row;
			uint32_t end_col;
			uint32_t end_row;

			cur_slc->valid = 1;
			cur_slc->y = i;
			cur_slc->x = j;
			start_col = j * slice_width;
			start_row = i * slice_height;
			end_col = start_col + slice_width - 1;
			end_row = start_row + slice_height - 1;
			if (i != 0)
				cur_slc->slice_overlap.overlap_up =
							slc_ctx->overlap_up;
			if (j != 0)
				cur_slc->slice_overlap.overlap_left =
							slc_ctx->overlap_left;
			if (i != (slice_total_row - 1))
				cur_slc->slice_overlap.overlap_down =
							slc_ctx->overlap_down;
			else
				end_row = img_height - 1;

			if (j != (slice_total_col - 1))
				cur_slc->slice_overlap.overlap_right =
							slc_ctx->overlap_right;
			else
				end_col = img_width - 1;

			cur_slc->slice_pos_orig.start_col = start_col;
			cur_slc->slice_pos_orig.start_row = start_row;
			cur_slc->slice_pos_orig.end_col = end_col;
			cur_slc->slice_pos_orig.end_row = end_row;

			cur_slc->slice_pos.start_col =
				start_col - cur_slc->slice_overlap.overlap_left;
			cur_slc->slice_pos.start_row =
				start_row - cur_slc->slice_overlap.overlap_up;
			cur_slc->slice_pos.end_col =
				end_col + cur_slc->slice_overlap.overlap_right;
			cur_slc->slice_pos.end_row =
				end_row + cur_slc->slice_overlap.overlap_down;

			cur_slc->slice_pos_fetch.start_col = cur_slc->slice_pos.start_col + fetch_start_x;
			cur_slc->slice_pos_fetch.start_row = cur_slc->slice_pos.start_row + fetch_start_y;
			cur_slc->slice_pos_fetch.end_col = cur_slc->slice_pos.end_col + fetch_start_x;
			cur_slc->slice_pos_fetch.end_row = cur_slc->slice_pos.end_row + fetch_start_y;

			pr_debug("slice %d %d pos_orig [%d %d %d %d]\n", i, j,
					cur_slc->slice_pos_orig.start_col,
					cur_slc->slice_pos_orig.end_col,
					cur_slc->slice_pos_orig.start_row,
					cur_slc->slice_pos_orig.end_row);
			pr_debug("slice %d %d pos [%d %d %d %d]\n", i, j,
					cur_slc->slice_pos.start_col,
					cur_slc->slice_pos.end_col,
					cur_slc->slice_pos.start_row,
					cur_slc->slice_pos.end_row);
			pr_debug("slice %d %d pos_fetch [%d %d %d %d]\n", i, j,
					cur_slc->slice_pos_fetch.start_col,
					cur_slc->slice_pos_fetch.end_col,
					cur_slc->slice_pos_fetch.start_row,
					cur_slc->slice_pos_fetch.end_row);
			pr_debug("slice %d %d ovl [%d %d %d %d]\n", i, j,
					cur_slc->slice_overlap.overlap_up,
					cur_slc->slice_overlap.overlap_down,
					cur_slc->slice_overlap.overlap_left,
					cur_slc->slice_overlap.overlap_right);

			cur_slc->slice_fbd_raw.fetch_fbd_bypass =
				frame_fbd_raw->fetch_fbd_bypass;

			cur_slc++;
		}
	}

	if (valid_slc_num)
		*valid_slc_num = 0;

	for (i = 0; i < SLICE_NUM_MAX; i++) {
		pr_debug("slice %d valid %d. xy (%d %d)  %p\n",
			i, slc_ctx->slices[i].valid,
			slc_ctx->slices[i].x, slc_ctx->slices[i].y,
			&slc_ctx->slices[i]);
		if (slc_ctx->slices[i].valid && valid_slc_num)
			*valid_slc_num = (*valid_slc_num) + 1;
	}

	return rtn;
}

static int cfg_slice_nr_info(
		struct slice_cfg_input *in_ptr,
		struct isp_slice_context *slc_ctx)
{
	int i;
	uint32_t start_col, start_row;
	uint32_t end_col, end_row;
	struct isp_slice_desc *cur_slc;

	cur_slc = &slc_ctx->slices[0];
	for (i = 0; i < SLICE_NUM_MAX; i++, cur_slc++) {
		if (cur_slc->valid == 0)
			continue;

		start_col = cur_slc->slice_pos.start_col;
		start_row = cur_slc->slice_pos.start_row;
		end_col = cur_slc->slice_pos.end_col;
		end_row = cur_slc->slice_pos.end_row;

		/* NLM */
		cur_slc->slice_nlm.center_x_relative =
				in_ptr->nlm_center_x - start_col;
		cur_slc->slice_nlm.center_y_relative =
				in_ptr->nlm_center_y - start_row;

		/* Post CDN */
		cur_slc->slice_postcdn.start_row_mod4 =
				cur_slc->slice_pos.start_row & 0x3;

		/* YNR */
		cur_slc->slice_ynr.center_offset_x =
			(int32_t)in_ptr->ynr_center_x - (int32_t)start_col;
		cur_slc->slice_ynr.center_offset_y =
			(int32_t)in_ptr->ynr_center_y - (int32_t)start_row;
		cur_slc->slice_ynr.slice_width = end_col - start_col + 1;
		cur_slc->slice_ynr.slice_height = end_row - start_row + 1;
		pr_debug("slice %d,  (%d %d %d %d),  ynr_off %d %d, size %d %d\n",
			i, start_row, start_col, end_row, end_col,
			cur_slc->slice_ynr.center_offset_x,
			cur_slc->slice_ynr.center_offset_y,
			cur_slc->slice_ynr.slice_width,
			cur_slc->slice_ynr.slice_height);
	}

	return 0;
}

static void cfg_spath_trim0_info(
		struct isp_scaler_slice_tmp *sinfo,
		struct img_trim *frm_trim0,
		struct slice_scaler_info *slc_scaler)
{
	uint32_t start, end;

	/* trim0 x */
	start = sinfo->start_col + sinfo->overlap_bad_left;
	end = sinfo->end_col + 1 - sinfo->overlap_bad_right;

	pr_debug("start end %d, %d\n", start, end);
	pr_debug("frame %d %d %d %d\n",
		frm_trim0->start_x,
		frm_trim0->start_y,
		frm_trim0->size_x,
		frm_trim0->size_y);

	if (sinfo->slice_col_num == 1) {
		slc_scaler->trim0_start_x = frm_trim0->start_x;
		slc_scaler->trim0_size_x = frm_trim0->size_x;
		pr_debug("%d, %d\n",
			slc_scaler->trim0_start_x,
			slc_scaler->trim0_size_x);
	} else {
		if ((sinfo->end_col_orig < frm_trim0->start_x) ||
			(sinfo->start_col_orig >
			(frm_trim0->start_x + frm_trim0->size_x - 1))) {
			slc_scaler->out_of_range = 1;
			return;
		}

		if (sinfo->x == 0) {
			/* first slice */
			slc_scaler->trim0_start_x = frm_trim0->start_x;
			if (sinfo->trim0_end_x < end)
				slc_scaler->trim0_size_x = frm_trim0->size_x;
			else
				slc_scaler->trim0_size_x = end -
					frm_trim0->start_x;
		} else if ((sinfo->slice_col_num - 1) == sinfo->x) {
			/* last slice */
			if (frm_trim0->start_x > start) {
				slc_scaler->trim0_start_x =
					frm_trim0->start_x - sinfo->start_col;
				slc_scaler->trim0_size_x = frm_trim0->size_x;
			} else {
				slc_scaler->trim0_start_x =
					sinfo->overlap_bad_left;
				slc_scaler->trim0_size_x = sinfo->trim0_end_x -
					start;
			}
		} else {
			if (frm_trim0->start_x < start) {
				slc_scaler->trim0_start_x =
					sinfo->overlap_bad_left;
				if (sinfo->trim0_end_x < end)
					slc_scaler->trim0_size_x =
						sinfo->trim0_end_x-start;
				else
					slc_scaler->trim0_size_x = end - start;
			} else {
				slc_scaler->trim0_start_x =
					frm_trim0->start_x - sinfo->start_col;
				if (sinfo->trim0_end_x < end)
					slc_scaler->trim0_size_x =
						frm_trim0->size_x;
				else
					slc_scaler->trim0_size_x = end -
						frm_trim0->start_x;
			}
		}
	}

	/* trim0 y */
	start = sinfo->start_row + sinfo->overlap_bad_up;
	end = sinfo->end_row + 1 - sinfo->overlap_bad_down;
	if (sinfo->slice_row_num == 1) {
		slc_scaler->trim0_start_y = frm_trim0->start_y;
		slc_scaler->trim0_size_y = frm_trim0->size_y;
	} else {
		pr_err("fail to get support vertical slices.\n");
	}
}

static void cfg_spath_deci_info(
		struct isp_scaler_slice_tmp *sinfo,
		struct img_deci_info *frm_deci,
		struct img_trim *frm_trim0,
		struct slice_scaler_info *slc_scaler)
{
	uint32_t start;

	if (frm_deci->deci_x_eb)
		sinfo->deci_x = 1 << (frm_deci->deci_x + 1);
	else
		sinfo->deci_x = 1;

	if (frm_deci->deci_y_eb)
		sinfo->deci_y = 1 << (frm_deci->deci_y + 1);
	else
		sinfo->deci_y = 1;

	sinfo->deci_x_align = sinfo->deci_x * 2;

	start = sinfo->start_col + sinfo->overlap_bad_left;

	if (frm_trim0->start_x >= sinfo->start_col &&
		(frm_trim0->start_x <= (sinfo->end_col + 1))) {
		slc_scaler->trim0_size_x = slc_scaler->trim0_size_x /
			sinfo->deci_x_align * sinfo->deci_x_align;
	} else {
		sinfo->trim0_start_adjust_x = (start + sinfo->deci_x_align - 1)/
			sinfo->deci_x_align * sinfo->deci_x_align - start;
		slc_scaler->trim0_start_x += sinfo->trim0_start_adjust_x;
		slc_scaler->trim0_size_x -= sinfo->trim0_start_adjust_x;
		slc_scaler->trim0_size_x = slc_scaler->trim0_size_x/
			sinfo->deci_x_align * sinfo->deci_x_align;
	}

	if (slc_scaler->odata_mode == ODATA_YUV422)
		sinfo->deci_y_align = sinfo->deci_y;
	else
		sinfo->deci_y_align = sinfo->deci_y * 2;

	start = sinfo->start_row + sinfo->overlap_bad_up;

	if (frm_trim0->start_y >= sinfo->start_row &&
		(frm_trim0->start_y  <= (sinfo->end_row + 1))) {
		slc_scaler->trim0_size_y = slc_scaler->trim0_size_y/
			sinfo->deci_y_align * sinfo->deci_y_align;
	} else {
		sinfo->trim0_start_adjust_y = (start + sinfo->deci_y_align - 1)/
			sinfo->deci_y_align * sinfo->deci_y_align - start;
		slc_scaler->trim0_start_y += sinfo->trim0_start_adjust_y;
		slc_scaler->trim0_size_y -= sinfo->trim0_start_adjust_y;
		slc_scaler->trim0_size_y = slc_scaler->trim0_size_y/
			sinfo->deci_y_align * sinfo->deci_y_align;
	}

	slc_scaler->scaler_in_width =
		slc_scaler->trim0_size_x / sinfo->deci_x;
	slc_scaler->scaler_in_height =
		slc_scaler->trim0_size_y / sinfo->deci_y;

}

static void calc_scaler_phase(uint32_t phase, uint32_t factor,
	uint32_t *phase_int, uint32_t *phase_rmd)
{
	phase_int[0] = (uint32_t)(phase / factor);
	phase_rmd[0] = (uint32_t)(phase - factor * phase_int[0]);
}

static void cfg_spath_scaler_info(
		struct isp_scaler_slice_tmp *slice,
		struct img_trim *frm_trim0,
		struct isp_scaler_info *in,
		struct slice_scaler_info *out)
{
	uint32_t scl_factor_in, scl_factor_out;
	uint32_t  initial_phase, last_phase, phase_in;
	uint32_t phase_tmp, scl_temp, out_tmp;
	uint32_t start, end;
	uint32_t tap_hor, tap_ver, tap_hor_uv, tap_ver_uv;
	uint32_t tmp, n;

	if (in->scaler_bypass == 0) {
		scl_factor_in = in->scaler_factor_in / 2;
		scl_factor_out = in->scaler_factor_out / 2;
		initial_phase = 0;
		last_phase = initial_phase+
			scl_factor_in * (in->scaler_out_width / 2 - 1);
		tap_hor = 8;
		tap_hor_uv = tap_hor / 2;

		start = slice->start_col + slice->overlap_bad_left +
			slice->deci_x_align - 1;
		end = slice->end_col + 1 - slice->overlap_bad_right +
			slice->deci_x_align - 1;
		if (frm_trim0->start_x >= slice->start_col &&
			(frm_trim0->start_x <= slice->end_col + 1)) {
			phase_in = 0;
			if (out->scaler_in_width ==
				frm_trim0->size_x / slice->deci_x)
				phase_tmp = last_phase;
			else
				phase_tmp = (out->scaler_in_width / 2 -
					tap_hor_uv / 2) * scl_factor_out -
					scl_factor_in / 2 - 1;
			out_tmp = (phase_tmp - phase_in) / scl_factor_in + 1;
			out->scaler_out_width = out_tmp * 2;
		} else {
			phase_in = (tap_hor_uv / 2) * scl_factor_out;
			if (slice->x == slice->slice_col_num - 1) {
				phase_tmp = last_phase -
					((frm_trim0->size_x / 2) /
					slice->deci_x -	out->scaler_in_width /
					2) * scl_factor_out;
				out_tmp = (phase_tmp - phase_in) /
					scl_factor_in + 1;
				out->scaler_out_width = out_tmp * 2;
				phase_in = phase_tmp - (out_tmp - 1) *
					scl_factor_in;
			} else {
				if (slice->trim0_end_x >= slice->start_col
					&& (slice->trim0_end_x <= slice->end_col
					+1 - slice->overlap_bad_right)) {
					phase_tmp = last_phase -
						((frm_trim0->size_x / 2) /
						slice->deci_x -
						out->scaler_in_width / 2) *
						scl_factor_out;
					out_tmp = (phase_tmp - phase_in)/
						scl_factor_in + 1;
					out->scaler_out_width = out_tmp * 2;
					phase_in = phase_tmp - (out_tmp - 1)
						*scl_factor_in;
				} else {
					initial_phase = ((((start/
					slice->deci_x_align *
						slice->deci_x_align
					-frm_trim0->start_x) / slice->deci_x) /
						2+
					(tap_hor_uv / 2)) * (scl_factor_out)+
					(scl_factor_in - 1)) / scl_factor_in*
					scl_factor_in;
					slice->scaler_out_width_temp =
					((last_phase - initial_phase)/
					scl_factor_in + 1) * 2;

					scl_temp = ((end / slice->deci_x_align*
					slice->deci_x_align -
						frm_trim0->start_x)/
					slice->deci_x) / 2;
					last_phase = ((
						scl_temp - tap_hor_uv / 2)*
					(scl_factor_out) - scl_factor_in / 2 -
						1)/
					scl_factor_in * scl_factor_in;

					out_tmp = (last_phase - initial_phase)/
						scl_factor_in + 1;
					out->scaler_out_width = out_tmp * 2;
					phase_in = initial_phase - (((start/
					slice->deci_x_align *
						slice->deci_x_align-
					frm_trim0->start_x) / slice->deci_x) /
						2)*
					scl_factor_out;
				}
			}
		}

		calc_scaler_phase(phase_in * 4, scl_factor_out * 2,
			&out->scaler_ip_int, &out->scaler_ip_rmd);
		calc_scaler_phase(phase_in, scl_factor_out,
			&out->scaler_cip_int, &out->scaler_cip_rmd);

		scl_factor_in = in->scaler_ver_factor_in;
		scl_factor_out = in->scaler_ver_factor_out;
		initial_phase = 0;
		last_phase = initial_phase+
			scl_factor_in * (in->scaler_out_height - 1);
		tap_ver = in->scaler_y_ver_tap > in->scaler_uv_ver_tap ?
			in->scaler_y_ver_tap : in->scaler_uv_ver_tap;
		tap_ver += 2;
		tap_ver_uv = tap_ver;

		start = slice->start_row + slice->overlap_bad_up +
			slice->deci_y_align - 1;
		end = slice->end_row + 1 - slice->overlap_bad_down +
			slice->deci_y_align - 1;

		if (frm_trim0->start_y >= slice->start_row &&
			(frm_trim0->start_y <= slice->end_row + 1)) {
			phase_in = 0;
			if (out->scaler_in_height ==
				frm_trim0->size_y / slice->deci_y)
				phase_tmp = last_phase;
			else
				phase_tmp = (out->scaler_in_height-
				tap_ver_uv / 2) * scl_factor_out - 1;
			out_tmp = (phase_tmp - phase_in) / scl_factor_in + 1;
			if (out_tmp % 2 == 1)
				out_tmp -= 1;
			out->scaler_out_height = out_tmp;
		} else {
			phase_in = (tap_ver_uv / 2) * scl_factor_out;
			if (slice->y == slice->slice_row_num - 1) {
				phase_tmp = last_phase-
					(frm_trim0->size_y / slice->deci_y -
					out->scaler_in_height) * scl_factor_out;
				out_tmp =
					(phase_tmp - phase_in) / scl_factor_in
					+ 1;
				if (out_tmp % 2 == 1)
					out_tmp -= 1;
				if (in->odata_mode == 1 && out_tmp % 4 != 0)
					out_tmp = out_tmp / 4 * 4;
				out->scaler_out_height = out_tmp;
				phase_in = phase_tmp - (
					out_tmp - 1) * scl_factor_in;
			} else {
				if (slice->trim0_end_y >= slice->start_row &&
					(slice->trim0_end_y <= slice->end_row+1
					-slice->overlap_bad_down)) {
					phase_tmp = last_phase-
					(frm_trim0->size_y / slice->deci_y-
					out->scaler_in_height) * scl_factor_out;
					out_tmp = (phase_tmp - phase_in)/
						scl_factor_in + 1;
					if (out_tmp % 2 == 1)
						out_tmp -= 1;
					if (in->odata_mode == 1 &&
						out_tmp % 4 != 0)
						out_tmp = out_tmp / 4 * 4;
					out->scaler_out_height = out_tmp;
					phase_in = phase_tmp - (out_tmp - 1)
						*scl_factor_in;
				} else {
					initial_phase = (((start/
					slice->deci_y_align *
						slice->deci_y_align
					-frm_trim0->start_y) / slice->deci_y+
					(tap_ver_uv / 2)) * (scl_factor_out)+
					(scl_factor_in - 1)) / (
						scl_factor_in * 2)
					*(scl_factor_in * 2);
					slice->scaler_out_height_temp =
						(last_phase - initial_phase)/
						scl_factor_in + 1;
					scl_temp = (end / slice->deci_y_align*
					slice->deci_y_align -
						frm_trim0->start_y)/
					slice->deci_y;
					last_phase = ((
						scl_temp - tap_ver_uv / 2)*
					(scl_factor_out) - 1) / scl_factor_in
					*scl_factor_in;
					out_tmp = (last_phase - initial_phase)/
						scl_factor_in + 1;
					if (out_tmp % 2 == 1)
						out_tmp -= 1;
					if (in->odata_mode == 1 &&
						out_tmp % 4 != 0)
						out_tmp = out_tmp / 4 * 4;
					out->scaler_out_height = out_tmp;
					phase_in = initial_phase - (start/
					slice->deci_y_align *
						slice->deci_y_align-
					frm_trim0->start_y) / slice->deci_y
					*scl_factor_out;
				}
			}
		}

		calc_scaler_phase(phase_in, scl_factor_out,
			&out->scaler_ip_int_ver, &out->scaler_ip_rmd_ver);
		if (in->odata_mode == 1) {
			phase_in /= 2;
			scl_factor_out /= 2;
		}
		calc_scaler_phase(phase_in, scl_factor_out,
			&out->scaler_cip_int_ver, &out->scaler_cip_rmd_ver);

		if (out->scaler_ip_int >= 16) {
			tmp = out->scaler_ip_int;
			n = (tmp >> 3) - 1;
			out->trim0_start_x += 8 * n * slice->deci_x;
			out->trim0_size_x -= 8 * n * slice->deci_x;
			out->scaler_ip_int -= 8 * n;
			out->scaler_cip_int -= 4 * n;
		}
		if (out->scaler_ip_int >= 16)
			pr_err("fail to get horizontal slice initial phase, overflow!\n");
		if (out->scaler_ip_int_ver >= 16) {
			tmp = out->scaler_ip_int_ver;
			n = (tmp >> 3) - 1;
			out->trim0_start_y += 8 * n * slice->deci_y;
			out->trim0_size_y -= 8 * n * slice->deci_y;
			out->scaler_ip_int_ver -= 8 * n;
			out->scaler_cip_int_ver -= 8 * n;
		}
		if (out->scaler_ip_int_ver >= 16)
			pr_err("fail to get vertical slice initial phase, overflow!\n");
	} else {
		out->scaler_out_width = out->scaler_in_width;
		out->scaler_out_height = out->scaler_in_height;
		start = slice->start_col + slice->overlap_bad_left +
			slice->trim0_start_adjust_x + slice->deci_x_align - 1;
		slice->scaler_out_width_temp = (frm_trim0->size_x - (start/
			slice->deci_x_align * slice->deci_x_align-
			frm_trim0->start_x)) / slice->deci_x;
		start = slice->start_row + slice->overlap_bad_up +
			slice->trim0_start_adjust_y + slice->deci_y_align - 1;
		slice->scaler_out_height_temp = (frm_trim0->size_y - (start/
			slice->deci_y_align * slice->deci_y_align-
			frm_trim0->start_y)) / slice->deci_y;
	}
}

void cfg_spath_trim1_info(
		struct isp_scaler_slice_tmp *slice,
		struct img_trim *frm_trim0,
		struct isp_scaler_info *in,
		struct slice_scaler_info *out)
{
	uint32_t trim_sum_x = slice->trim1_sum_x;
	uint32_t trim_sum_y = slice->trim1_sum_y;
	uint32_t pix_align = 8;

	if (frm_trim0->start_x >= slice->start_col &&
		(frm_trim0->start_x <= slice->end_col + 1)) {
		out->trim1_start_x = 0;
		if (out->scaler_in_width == frm_trim0->size_x)
			out->trim1_size_x = out->scaler_out_width;
		else
			out->trim1_size_x = out->scaler_out_width & ~(
				pix_align - 1);
	} else {
		if (slice->x == slice->slice_col_num - 1) {
			out->trim1_size_x =
				in->scaler_out_width - trim_sum_x;
			out->trim1_start_x =
				out->scaler_out_width - out->trim1_size_x;
		} else {
			if (slice->trim0_end_x >= slice->start_col &&
				(slice->trim0_end_x <= slice->end_col+1
				-slice->overlap_bad_right)) {
				out->trim1_size_x = in->scaler_out_width
					-trim_sum_x;
				out->trim1_start_x = out->scaler_out_width-
					out->trim1_size_x;
			} else {
				out->trim1_start_x =
					slice->scaler_out_width_temp-
					(in->scaler_out_width - trim_sum_x);
				out->trim1_size_x = (out->scaler_out_width-
					out->trim1_start_x) & ~(pix_align - 1);
			}
		}
	}

	if (frm_trim0->start_y >= slice->start_row &&
		(frm_trim0->start_y <= slice->end_row + 1)) {
		out->trim1_start_y = 0;
		if (out->scaler_in_height == frm_trim0->size_y)
			out->trim1_size_y = out->scaler_out_height;
		else
			out->trim1_size_y = out->scaler_out_height & ~(
				pix_align - 1);
	} else {
		if (slice->y == slice->slice_row_num - 1) {
			out->trim1_size_y = in->scaler_out_height - trim_sum_y;
			out->trim1_start_y = out->scaler_out_height-
				out->trim1_size_y;
		} else {
			if (slice->trim0_end_y >= slice->start_row &&
				(slice->trim0_end_y <= slice->end_row+1
				-slice->overlap_bad_down)) {
				out->trim1_size_y = in->scaler_out_height
					-trim_sum_y;
				out->trim1_start_y = out->scaler_out_height-
					out->trim1_size_y;
			} else {
				out->trim1_start_y =
					slice->scaler_out_height_temp-
					(in->scaler_out_height - trim_sum_y);
				out->trim1_size_y = (out->scaler_out_height-
					out->trim1_start_y) & ~(pix_align - 1);
			}
		}
	}
}

static int cfg_slice_thumbscaler(
	struct isp_slice_desc *cur_slc,
	struct img_trim *frm_trim0,
	struct isp_thumbscaler_info *scalerFrame,
	struct slice_thumbscaler_info *scalerSlice)
{
	int ret = 0;
	uint32_t half;
	uint32_t deci_w, deci_h, trim_w, trim_h;
	uint32_t frm_start_col, frm_end_col;
	uint32_t frm_start_row, frm_end_row;
	uint32_t slc_start_col, slc_end_col;
	uint32_t slc_start_row, slc_end_row;
	struct img_size src;

	scalerSlice->scaler_bypass = scalerFrame->scaler_bypass;
	scalerSlice->odata_mode = scalerFrame->odata_mode;
	scalerSlice->out_of_range = 0;
	frm_start_col = frm_trim0->start_x;
	frm_end_col = frm_trim0->size_x + frm_trim0->start_x - 1;
	frm_start_row = frm_trim0->start_y;
	frm_end_row = frm_trim0->size_y + frm_trim0->start_y - 1;
	if ((cur_slc->slice_pos_orig.end_col < frm_start_col) ||
		(cur_slc->slice_pos_orig.start_col > frm_end_col)) {
		scalerSlice->out_of_range = 1;
		cur_slc->path_en[ISP_SPATH_FD] = 0;
		return 0;
	}
	deci_w = scalerFrame->y_deci.deci_x;
	deci_h = scalerFrame->y_deci.deci_y;

	src.w = cur_slc->slice_pos.end_col - cur_slc->slice_pos.start_col + 1;
	src.h = cur_slc->slice_pos.end_row - cur_slc->slice_pos.start_row + 1;
	scalerSlice->src0 = src;

	slc_start_col = MAX(cur_slc->slice_pos_orig.start_col, frm_start_col);
	slc_start_row = MAX(cur_slc->slice_pos_orig.start_row, frm_start_row);
	slc_end_col = MIN(cur_slc->slice_pos_orig.end_col, frm_end_col);
	slc_end_row = MIN(cur_slc->slice_pos_orig.end_row, frm_end_row);
	trim_w = slc_end_col - slc_start_col + 1;
	trim_h = slc_end_row - slc_start_row + 1;
	scalerSlice->y_trim.start_x = slc_start_col -
		cur_slc->slice_pos.start_col;
	scalerSlice->y_trim.start_y = slc_start_row -
		cur_slc->slice_pos.start_row;
	scalerSlice->y_trim.size_x = trim_w;
	scalerSlice->y_trim.size_y = trim_h;
	scalerSlice->uv_trim.start_x = scalerSlice->y_trim.start_x / 2;
	scalerSlice->uv_trim.start_y = scalerSlice->y_trim.start_y;
	scalerSlice->uv_trim.size_x = trim_w / 2;
	scalerSlice->uv_trim.size_y = trim_h;

	scalerSlice->y_factor_in.w = trim_w / deci_w;
	scalerSlice->y_factor_in.h = trim_h / deci_h;

	half = (scalerFrame->y_factor_out.w + 1) / 2;
	scalerSlice->y_factor_out.w =
		(scalerSlice->y_factor_in.w * scalerFrame->y_factor_out.w +
			half)
				/ scalerFrame->y_factor_in.w;

	half = (scalerFrame->y_factor_out.h + 1) / 2;
	scalerSlice->y_factor_out.h =
		(scalerSlice->y_factor_in.h * scalerFrame->y_factor_out.h +
			half)
				/ scalerFrame->y_factor_in.h;

	scalerSlice->y_src_after_deci = scalerSlice->y_factor_in;
	scalerSlice->y_dst_after_scaler = scalerSlice->y_factor_out;

	scalerSlice->uv_factor_in.w = trim_w / deci_w / 2;
	scalerSlice->uv_factor_in.h = trim_h / deci_h;

	half = (scalerFrame->uv_factor_out.w + 1) / 2;
	scalerSlice->uv_factor_out.w =
		(scalerSlice->uv_factor_in.w * scalerFrame->uv_factor_out.w +
			half)
				/ scalerFrame->uv_factor_in.w;

	half = (scalerFrame->uv_factor_out.h + 1) / 2;
	scalerSlice->uv_factor_out.h =
		(scalerSlice->uv_factor_in.h * scalerFrame->uv_factor_out.h +
			half)
				/ scalerFrame->uv_factor_in.h;

	scalerSlice->uv_src_after_deci = scalerSlice->uv_factor_in;
	scalerSlice->uv_dst_after_scaler = scalerSlice->uv_factor_out;

	pr_debug("-------------slice (%d %d),  src (%d %d)-------------\n",
		cur_slc->x, cur_slc->y,
		scalerSlice->src0.w, scalerSlice->src0.h);

	pr_debug("Y: (%d %d), (%d %d), (%d %d %d %d), (%d %d)\n",
		scalerSlice->y_factor_in.w, scalerSlice->y_factor_in.h,
		scalerSlice->y_factor_out.w, scalerSlice->y_factor_out.h,
		scalerSlice->y_trim.start_x, scalerSlice->y_trim.start_y,
		scalerSlice->y_trim.size_x, scalerSlice->y_trim.size_y,
		scalerSlice->y_init_phase.w, scalerSlice->y_init_phase.h);
	pr_debug("U: (%d %d), (%d %d), (%d %d %d %d), (%d %d)\n",
		scalerSlice->uv_factor_in.w, scalerSlice->uv_factor_in.h,
		scalerSlice->uv_factor_out.w, scalerSlice->uv_factor_out.h,
		scalerSlice->uv_trim.start_x, scalerSlice->uv_trim.start_y,
		scalerSlice->uv_trim.size_x, scalerSlice->uv_trim.size_y,
		scalerSlice->uv_init_phase.w, scalerSlice->uv_init_phase.h);

	return ret;
}

static int cfg_slice_scaler_info(
		struct slice_cfg_input *in_ptr,
		struct isp_slice_context *slc_ctx)
{
	int i, j;
	uint32_t trim1_sum_x[ISP_SPATH_NUM][SLICE_W_NUM_MAX] = { { 0 }, { 0 } };
	uint32_t trim1_sum_y[ISP_SPATH_NUM][SLICE_H_NUM_MAX] = { { 0 }, { 0 } };
	struct isp_scaler_info  *frm_scaler;
	struct img_deci_info *frm_deci;
	struct img_trim *frm_trim0;
	struct img_trim *frm_trim1;

	struct slice_scaler_info *slc_scaler;
	struct isp_slice_desc *cur_slc;
	struct isp_scaler_slice_tmp sinfo = {0};

	sinfo.slice_col_num = slc_ctx->slice_col_num;
	sinfo.slice_row_num = slc_ctx->slice_row_num;
	sinfo.trim1_sum_x = 0;
	sinfo.trim1_sum_y = 0;
	sinfo.overlap_bad_up =
		slc_ctx->overlap_up - YUVSCALER_OVERLAP_UP;
	sinfo.overlap_bad_down =
		slc_ctx->overlap_down - YUVSCALER_OVERLAP_DOWN;
	sinfo.overlap_bad_left =
		slc_ctx->overlap_left - YUVSCALER_OVERLAP_LEFT;
	sinfo.overlap_bad_right =
		slc_ctx->overlap_right - YUVSCALER_OVERLAP_RIGHT;

	for (i = 0; i < SLICE_NUM_MAX; i++) {
		pr_debug("slice %d valid %d. xy (%d %d)  %p\n",
			i, slc_ctx->slices[i].valid,
			slc_ctx->slices[i].x, slc_ctx->slices[i].y,
			&slc_ctx->slices[i]);
	}

	cur_slc = &slc_ctx->slices[0];
	for (i = 0; i < SLICE_NUM_MAX; i++, cur_slc++) {
		if (cur_slc->valid == 0) {
			pr_debug("slice %d not valid. %p\n", i, cur_slc);
			continue;
		}

		for (j = 0; j < ISP_SPATH_NUM; j++) {

			frm_scaler = in_ptr->frame_scaler[j];
			if (frm_scaler == NULL) {
				/* path is not valid. */
				cur_slc->path_en[j] = 0;
				pr_debug("path %d not enable.\n", j);
				continue;
			}
			cur_slc->path_en[j] = 1;
			pr_debug("path %d  enable.\n", j);

			if (j == ISP_SPATH_FD) {
				cfg_slice_thumbscaler(cur_slc,
					in_ptr->frame_trim0[j],
					(struct isp_thumbscaler_info *)
					in_ptr->frame_scaler[j],
					&cur_slc->slice_thumbscaler);
				continue;
			}

			frm_deci = in_ptr->frame_deci[j];
			frm_trim0 = in_ptr->frame_trim0[j];
			frm_trim1 = in_ptr->frame_trim1[j];

			slc_scaler = &cur_slc->slice_scaler[j];
			slc_scaler->scaler_bypass = frm_scaler->scaler_bypass;
			slc_scaler->odata_mode = frm_scaler->odata_mode;

			sinfo.x = cur_slc->x;
			sinfo.y = cur_slc->y;

			sinfo.start_col = cur_slc->slice_pos.start_col;
			sinfo.end_col = cur_slc->slice_pos.end_col;
			sinfo.start_row = cur_slc->slice_pos.start_row;
			sinfo.end_row = cur_slc->slice_pos.end_row;

			sinfo.start_col_orig =
				cur_slc->slice_pos_orig.start_col;
			sinfo.end_col_orig = cur_slc->slice_pos_orig.end_col;
			sinfo.start_row_orig =
				cur_slc->slice_pos_orig.start_row;
			sinfo.end_row_orig = cur_slc->slice_pos_orig.end_row;

			sinfo.trim0_end_x = frm_trim0->start_x +
				frm_trim0->size_x;
			sinfo.trim0_end_y = frm_trim0->start_y +
				frm_trim0->size_y;

			cfg_spath_trim0_info(&sinfo, frm_trim0, slc_scaler);
			if (slc_scaler->out_of_range) {
				cur_slc->path_en[j] = 0;
				continue;
			}

			cfg_spath_deci_info(&sinfo, frm_deci, frm_trim0,
				slc_scaler);
			cfg_spath_scaler_info(&sinfo, frm_trim0, frm_scaler,
				slc_scaler);

			sinfo.trim1_sum_x = trim1_sum_x[j][cur_slc->x];
			sinfo.trim1_sum_y = trim1_sum_y[j][cur_slc->y];
			cfg_spath_trim1_info(&sinfo, frm_trim0, frm_scaler,
				slc_scaler);

			if (cur_slc->y == 0 &&
			    (cur_slc->x + 1) < SLICE_W_NUM_MAX &&
			    SLICE_W_NUM_MAX > 1)
				trim1_sum_x[j][cur_slc->x + 1] =
					slc_scaler->trim1_size_x +
					trim1_sum_x[j][cur_slc->x];

			if (cur_slc->x == 0 &&
			    (cur_slc->y + 1) < SLICE_H_NUM_MAX &&
			    SLICE_H_NUM_MAX > 1)
				trim1_sum_y[j][cur_slc->y + 1] =
					slc_scaler->trim1_size_y +
					trim1_sum_y[j][cur_slc->y];

			slc_scaler->src_size_x = sinfo.end_col - sinfo.start_col
				+1;
			slc_scaler->src_size_y = sinfo.end_row - sinfo.start_row
				+1;
			slc_scaler->dst_size_x = slc_scaler->scaler_out_width;
			slc_scaler->dst_size_y = slc_scaler->scaler_out_height;
		}

		/* check if all path scaler out of range. */
		/* if yes, invalid this slice. */
		cur_slc->valid = 0;
		for (j = 0; j < ISP_SPATH_NUM; j++)
			cur_slc->valid |= cur_slc->path_en[j];
		pr_debug("final slice (%d, %d) valid = %d\n",
			cur_slc->x, cur_slc->y, cur_slc->valid);
	}

	return 0;
}

static void _cfg_slice_fetch(struct isp_fetch_info *frm_fetch,
			     struct isp_slice_desc *cur_slc)
{
	uint32_t start_col, start_row;
	uint32_t end_col, end_row;
	uint32_t ch_offset[3] = { 0 };
	uint32_t mipi_word_num_start[16] = {
		0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5
	};
	uint32_t mipi_word_num_end[16] = {
		0, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5
	};
	struct slice_fetch_info *slc_fetch;
	struct img_pitch *pitch;

	start_col = cur_slc->slice_pos_fetch.start_col;
	start_row = cur_slc->slice_pos_fetch.start_row;
	end_col = cur_slc->slice_pos_fetch.end_col;
	end_row = cur_slc->slice_pos_fetch.end_row;

	slc_fetch = &cur_slc->slice_fetch;
	pitch = &frm_fetch->pitch;

	switch (frm_fetch->fetch_fmt) {
	case ISP_FETCH_YUV422_3FRAME:
		ch_offset[0] = start_row * pitch->pitch_ch0 + start_col;
		ch_offset[1] = start_row * pitch->pitch_ch1 + (start_col >> 1);
		ch_offset[2] = start_row * pitch->pitch_ch2 + (start_col >> 1);
		break;

	case ISP_FETCH_YUV422_2FRAME:
	case ISP_FETCH_YVU422_2FRAME:
		ch_offset[0] = start_row * pitch->pitch_ch0 + start_col;
		ch_offset[1] = start_row * pitch->pitch_ch1 + start_col;
		break;

	case ISP_FETCH_YUV420_2FRAME:
	case ISP_FETCH_YVU420_2FRAME:
		ch_offset[0] = start_row * pitch->pitch_ch0 + start_col;
		ch_offset[1] = (start_row >> 1) * pitch->pitch_ch1 + start_col;
		break;

	case ISP_FETCH_CSI2_RAW10:
		ch_offset[0] = start_row * pitch->pitch_ch0
			+ (start_col >> 2) * 5 + (start_col & 0x3);

		slc_fetch->mipi_byte_rel_pos = start_col & 0x0f;
		slc_fetch->mipi_word_num =
			((((end_col + 1) >> 4) * 5
			+ mipi_word_num_end[(end_col + 1) & 0x0f])
			-(((start_col + 1) >> 4) * 5
			+ mipi_word_num_start[(start_col + 1)
			& 0x0f]) + 1);
		pr_debug("(%d %d %d %d), pitch %d, offset %d, mipi %d %d\n",
			start_row, start_col, end_row, end_col,
			pitch->pitch_ch0, ch_offset[0],
			slc_fetch->mipi_byte_rel_pos, slc_fetch->mipi_word_num);
		break;

	default:
		ch_offset[0] = start_row * pitch->pitch_ch0 + start_col * 2;
		break;
	}

	slc_fetch->addr.addr_ch0 = frm_fetch->addr.addr_ch0 + ch_offset[0];
	slc_fetch->addr.addr_ch1 = frm_fetch->addr.addr_ch1 + ch_offset[1];
	slc_fetch->addr.addr_ch2 = frm_fetch->addr.addr_ch2 + ch_offset[2];
	slc_fetch->size.h = end_row - start_row + 1;
	slc_fetch->size.w = end_col - start_col + 1;

	pr_debug("slice fetch size %d, %d\n",
		 slc_fetch->size.w, slc_fetch->size.h);
}

static void _cfg_slice_fbd_raw(struct isp_fbd_raw_info *frame_fbd_raw,
			       struct isp_slice_desc *cur_slc)
{
	uint32_t sx, sy, ex, ey;
	uint32_t w0, h0, w, h;
	uint32_t tsx, tsy, tex, tey;
	uint32_t shx, shy, tid;
	struct slice_fbd_raw_info *slc_fbd_raw;

	/* sx, sy, ex, ey: start and stop of fetched region (unit: pixel) */
	sx = cur_slc->slice_pos_fetch.start_col;
	sy = cur_slc->slice_pos_fetch.start_row;
	ex = cur_slc->slice_pos_fetch.end_col;
	ey = cur_slc->slice_pos_fetch.end_row;

	/* w0, h0: size of whole region */
	w0 = frame_fbd_raw->size.w;
	h0 = frame_fbd_raw->size.h;
	/* w, h: size of fetched region */
	w = ex - sx + 1;
	h = ey - sy + 1;
	/* shx, shy: quick for division */
	shx = ffs(DCAM_FBC_TILE_WIDTH) - 1;
	shy = ffs(DCAM_FBC_TILE_HEIGHT) - 1;
	/* tsx, tsy, tex, tey: start and stop of (sx, sy) tile (unit: tile) */
	tsx = sx >> shx;
	tsy = sy >> shy;
	tex = ex >> shx;
	tey = ey >> shy;
	/* tid: start index of (sx, sy) tile in all tiles */
	tid = tsy * frame_fbd_raw->tiles_num_pitch + tsx;

	slc_fbd_raw = &cur_slc->slice_fbd_raw;
	slc_fbd_raw->pixel_start_in_hor = sx & (DCAM_FBC_TILE_WIDTH - 1);
	slc_fbd_raw->pixel_start_in_ver = sy & (DCAM_FBC_TILE_HEIGHT - 1);
	slc_fbd_raw->height = h;
	slc_fbd_raw->width = w;
	slc_fbd_raw->tiles_num_in_hor = tex - tsx + 1;
	slc_fbd_raw->tiles_num_in_ver = tey - tsy + 1;
	slc_fbd_raw->tiles_start_odd = tid & 0x1;

	slc_fbd_raw->header_addr_init =
		frame_fbd_raw->tile_addr_init_x256 - (tid >> 1);
	slc_fbd_raw->tile_addr_init_x256 =
		frame_fbd_raw->tile_addr_init_x256 + (tid << 8);
	slc_fbd_raw->low_bit_addr_init =
		frame_fbd_raw->low_bit_addr_init + (sx >> 1) + ((sy * w0) >> 2);

	/* have to copy these here for fmcu cmd queue, sad */
	slc_fbd_raw->tiles_num_pitch = frame_fbd_raw->tiles_num_pitch;
	slc_fbd_raw->low_bit_pitch = frame_fbd_raw->low_bit_pitch;
	slc_fbd_raw->fetch_fbd_bypass = 0;

	pr_debug("tid %u, w %u h %u\n", tid, w, h);
	pr_debug("fetch (%u %u %u %u) from %ux%u\n",
		 sx, sy, ex, ey, w0, h0);
	pr_debug("tile %u %u %u %u\n", tsx, tsy, tex, tey);
	pr_debug("head %x, tile %x, low2 %x\n",
		 slc_fbd_raw->header_addr_init,
		 slc_fbd_raw->tile_addr_init_x256,
		 slc_fbd_raw->low_bit_addr_init);
}

int isp_cfg_slice_fetch_info(void *cfg_in, struct isp_slice_context *slc_ctx)
{
	int i;
	struct slice_cfg_input *in_ptr = (struct slice_cfg_input *)cfg_in;
	struct isp_slice_desc *cur_slc = &slc_ctx->slices[0];

	for (i = 0; i < SLICE_NUM_MAX; i++, cur_slc++) {
		if (cur_slc->valid == 0)
			continue;

		if (!in_ptr->frame_fbd_raw->fetch_fbd_bypass)
			_cfg_slice_fbd_raw(in_ptr->frame_fbd_raw, cur_slc);
		else
			_cfg_slice_fetch(in_ptr->frame_fetch, cur_slc);
	}

	return 0;
}

int isp_cfg_slice_store_info(
		void *cfg_in, struct isp_slice_context *slc_ctx)
{
	int i, j;
	struct slice_cfg_input *in_ptr = (struct slice_cfg_input *)cfg_in;
	struct isp_store_info *frm_store;
	struct isp_slice_desc *cur_slc;
	struct slice_store_info *slc_store;
	struct slice_scaler_info *slc_scaler;
	struct slice_thumbscaler_info *slc_thumbscaler;

	uint32_t overlap_left, overlap_up;
	uint32_t overlap_right, overlap_down;

	uint32_t start_col, end_col, start_row, end_row;
	uint32_t start_row_out[ISP_SPATH_NUM][SLICE_W_NUM_MAX] = { { 0 },
		{ 0 } };
	uint32_t start_col_out[ISP_SPATH_NUM][SLICE_W_NUM_MAX] = { { 0 },
		{ 0 } };
	uint32_t ch0_offset = 0;
	uint32_t ch1_offset = 0;
	uint32_t ch2_offset = 0;

	cur_slc = &slc_ctx->slices[0];
	for (i = 0; i < SLICE_NUM_MAX; i++, cur_slc++) {
		if (cur_slc->valid == 0)
			continue;
		for (j = 0; j < ISP_SPATH_NUM; j++) {
			frm_store = in_ptr->frame_store[j];
			if (frm_store == NULL || cur_slc->path_en[j] == 0)
				/* path is not valid. */
				continue;

			slc_store = &cur_slc->slice_store[j];
			slc_scaler = &cur_slc->slice_scaler[j];

			if (j == ISP_SPATH_FD) {
				slc_thumbscaler = &cur_slc->slice_thumbscaler;
				slc_store->size.w =
					slc_thumbscaler->y_dst_after_scaler.w;
				slc_store->size.h =
					slc_thumbscaler->y_dst_after_scaler.h;
				if (cur_slc->y == 0 &&
				    (cur_slc->x + 1) < SLICE_W_NUM_MAX &&
				    SLICE_W_NUM_MAX > 1)
					start_col_out[j][cur_slc->x + 1] =
						slc_store->size.w +
						start_col_out[j][cur_slc->x];

				if (cur_slc->x == 0 &&
				    (cur_slc->y + 1) < SLICE_H_NUM_MAX &&
				    SLICE_H_NUM_MAX > 1)
					start_row_out[j][cur_slc->y + 1] =
						slc_store->size.h +
						start_row_out[j][cur_slc->y];

			} else if (slc_scaler->scaler_bypass == 0) {
				slc_store->size.w = slc_scaler->trim1_size_x;
				slc_store->size.h = slc_scaler->trim1_size_y;

				if (cur_slc->y == 0 &&
				    (cur_slc->x + 1) < SLICE_W_NUM_MAX &&
				    SLICE_W_NUM_MAX > 1)
					start_col_out[j][cur_slc->x + 1] =
						slc_store->size.w +
						start_col_out[j][cur_slc->x];

				if (cur_slc->x == 0 &&
				    (cur_slc->y + 1) < SLICE_H_NUM_MAX &&
				    SLICE_H_NUM_MAX > 1)
					start_row_out[j][cur_slc->y + 1] =
						slc_store->size.h +
						start_row_out[j][cur_slc->y];
			} else {
				start_col = cur_slc->slice_pos.start_col;
				start_row = cur_slc->slice_pos.start_row;
				end_col = cur_slc->slice_pos.end_col;
				end_row = cur_slc->slice_pos.end_row;
				overlap_left =
					cur_slc->slice_overlap.overlap_left;
				overlap_right =
					cur_slc->slice_overlap.overlap_right;
				overlap_up = cur_slc->slice_overlap.overlap_up;
				overlap_down =
					cur_slc->slice_overlap.overlap_down;

				pr_debug("slice %d. pos %d %d %d %d, ovl %d %d %d %d\n",
					i, start_col, end_col,
					start_row, end_row,
					overlap_up, overlap_down,
					overlap_left, overlap_right);

				slc_store->size.w = end_col - start_col + 1 -
						overlap_left - overlap_right;
				slc_store->size.h = end_row - start_row + 1 -
						overlap_up - overlap_down;
				slc_store->border.up_border = overlap_left;
				slc_store->border.down_border = overlap_left;
				slc_store->border.left_border = overlap_left;
				slc_store->border.right_border = overlap_left;
				if (cur_slc->y == 0)
					start_col_out[j][cur_slc->x] =
						start_col + overlap_left;
				if (cur_slc->x == 0)
					start_row_out[j][cur_slc->y] =
						start_row + overlap_up;

				pr_debug("scaler bypass .  %d   %d",
						start_row_out[j][cur_slc->y],
						start_col_out[j][cur_slc->x]);
			}

			switch (frm_store->color_fmt) {
			case ISP_STORE_UYVY:
				ch0_offset = start_col_out[j][cur_slc->x] * 2 +
					start_row_out[j][cur_slc->y] *
					frm_store->pitch.pitch_ch0;
				break;
			case ISP_STORE_YUV422_2FRAME:
			case ISP_STORE_YVU422_2FRAME:
				ch0_offset = start_col_out[j][cur_slc->x] +
					start_row_out[j][cur_slc->y] *
					frm_store->pitch.pitch_ch0;
				ch1_offset = start_col_out[j][cur_slc->x] +
					start_row_out[j][cur_slc->y] *
					frm_store->pitch.pitch_ch1;
				break;
			case ISP_STORE_YUV422_3FRAME:
				ch0_offset = start_col_out[j][cur_slc->x] +
					start_row_out[j][cur_slc->y] *
					frm_store->pitch.pitch_ch0;
				ch1_offset = (
					start_col_out[j][cur_slc->x] >> 1) +
					start_row_out[j][cur_slc->y] *
					frm_store->pitch.pitch_ch1;
				ch2_offset = (
					start_col_out[j][cur_slc->x] >> 1) +
					start_row_out[j][cur_slc->y] *
					frm_store->pitch.pitch_ch2;
				break;
			case ISP_STORE_YUV420_2FRAME:
			case ISP_STORE_YVU420_2FRAME:
				ch0_offset = start_col_out[j][cur_slc->x] +
					start_row_out[j][cur_slc->y] *
					frm_store->pitch.pitch_ch0;
				ch1_offset = start_col_out[j][cur_slc->x] +
					start_row_out[j][cur_slc->y] *
					frm_store->pitch.pitch_ch1 / 2;
				break;
			case ISP_STORE_YUV420_3FRAME:
				ch0_offset = start_col_out[j][cur_slc->x] +
					start_row_out[j][cur_slc->y] *
					frm_store->pitch.pitch_ch0;
				ch1_offset = (
					start_col_out[j][cur_slc->x] >> 1) +
					start_row_out[j][cur_slc->y] *
					frm_store->pitch.pitch_ch1 / 2;
				ch2_offset = (
					start_col_out[j][cur_slc->x] >> 1) +
					start_row_out[j][cur_slc->y] *
					frm_store->pitch.pitch_ch2 / 2;
				break;
			default:
				break;
			}
			slc_store->addr.addr_ch0 =
				frm_store->addr.addr_ch0 + ch0_offset;
			slc_store->addr.addr_ch1 =
				frm_store->addr.addr_ch1 + ch1_offset;
			slc_store->addr.addr_ch2 =
				frm_store->addr.addr_ch2 + ch2_offset;
		}
	}

	return 0;
}

int isp_cfg_slice_afbc_store_info(
		void *cfg_in, struct isp_slice_context *slc_ctx)
{
	int i = 0, j = 0, slice_id = 0;
	uint32_t slice_col_num = 0;
	uint32_t slice_start_col = 0;
	uint32_t cur_slice_row = 0;
	uint32_t cur_slice_col = 0;
	uint32_t overlap_left = 0;
	uint32_t overlap_up = 0;
	uint32_t overlap_down = 0;
	uint32_t overlap_right = 0;
	uint32_t start_row = 0, end_row = 0,
		start_col = 0, end_col = 0;
	uint32_t scl_out_width = 0, scl_out_height = 0;
	uint32_t tmp_slice_id = 0;
	uint32_t slice_start_col_tile_num = 0;

	struct slice_cfg_input *in_ptr = (struct slice_cfg_input *)cfg_in;
	struct isp_afbc_store_info *frm_afbc_store = NULL;
	struct isp_slice_desc *cur_slc = NULL;
	struct isp_slice_desc *cur_slc_temp = NULL;
	struct slice_afbc_store_info *slc_afbc_store = NULL;
	struct slice_scaler_info *slc_scaler = NULL;

	slice_col_num = slc_ctx->slice_col_num;
	cur_slc = &slc_ctx->slices[0];
	cur_slc_temp = &slc_ctx->slices[0];
	for (i = 0; i < SLICE_NUM_MAX; i++, cur_slc++) {
		if (cur_slc->valid == 0)
			continue;
		for (j = 0; j < FBC_PATH_NUM; j++) {
			frm_afbc_store = in_ptr->frame_afbc_store[j];
			if (frm_afbc_store == NULL || cur_slc->path_en[j] == 0)
				/* path is not valid. */
				continue;
			slc_afbc_store = &cur_slc->slice_afbc_store[j];
			slc_scaler = &cur_slc->slice_scaler[j];
			slice_id = i;
			cur_slice_row = slice_id / slice_col_num;
			cur_slice_col = slice_id % slice_col_num;
			if (slc_scaler->scaler_bypass == 1) {
				overlap_left = cur_slc->slice_overlap.overlap_left;
				overlap_up = cur_slc->slice_overlap.overlap_up;
				overlap_right = cur_slc->slice_overlap.overlap_right;
				overlap_down = cur_slc->slice_overlap.overlap_down;

				slc_afbc_store->border.up_border = overlap_up;
				slc_afbc_store->border.left_border = overlap_left;
				slc_afbc_store->border.down_border = overlap_down;
				slc_afbc_store->border.right_border = overlap_right;

				start_row = cur_slc->slice_pos.start_row;
				end_row = cur_slc->slice_pos.end_row;
				start_col = cur_slc->slice_pos.start_col;
				end_col = cur_slc->slice_pos.end_col;

				slc_afbc_store->size.w = end_col - start_col + 1
					- overlap_left - overlap_right;
				slc_afbc_store->size.h = end_row - start_row + 1
					- overlap_up - overlap_down;
				slice_start_col = start_col + overlap_left;
			} else {
				overlap_up = 0;
				overlap_left = 0;
				overlap_down = 0;
				overlap_right = 0;
				scl_out_width = slc_scaler->trim1_size_x;
				scl_out_height = slc_scaler->trim1_size_y;

				slc_afbc_store->border.up_border= overlap_up;
				slc_afbc_store->border.left_border = overlap_left;
				slc_afbc_store->border.down_border = overlap_down;
				slc_afbc_store->border.right_border = overlap_right;

				slc_afbc_store->size.h=
					scl_out_height - overlap_up - overlap_down;
				slc_afbc_store->size.w =
					scl_out_width - overlap_left - overlap_right;

				tmp_slice_id = slice_id;
				slice_start_col = 0;
				while ((int)(tmp_slice_id - 1) >=
					(int)(cur_slice_row * slice_col_num)) {
					tmp_slice_id--;
					slice_start_col +=
						(cur_slc_temp[tmp_slice_id].slice_afbc_store[j].size.w +
							AFBC_PADDING_W_YUV420 - 1) /
							AFBC_PADDING_W_YUV420 *
							AFBC_PADDING_W_YUV420;
				}
			}
			slice_start_col_tile_num = slice_start_col /
					AFBC_PADDING_W_YUV420;
			slc_afbc_store->yheader_addr =
				frm_afbc_store->yheader +
					slice_start_col_tile_num * AFBC_HEADER_SIZE;
			slc_afbc_store->yaddr =
				frm_afbc_store->yaddr +
					slice_start_col_tile_num * AFBC_PAYLOAD_SIZE;
			slc_afbc_store->slice_offset =
				frm_afbc_store->header_offset +
					slice_start_col_tile_num * AFBC_PAYLOAD_SIZE;
			slc_afbc_store->slc_afbc_on = 1;
			pr_debug("slice afbc yheader = %x\n",
				slc_afbc_store->yheader_addr);
			pr_debug("slice afbc yaddr = %x\n",
				slc_afbc_store->yaddr);
			pr_debug("slice afbc offset = %x\n",
				slc_afbc_store->slice_offset);
			pr_debug("slice afbc on = %x\n",
				slc_afbc_store->slc_afbc_on);
		}
	}

	return 0;
}

int isp_cfg_slice_ifbc_store_info(
		void *cfg_in, struct isp_slice_context *slc_ctx)
{
	int i = 0, j = 0, slice_id = 0;
	uint32_t slice_col_num = 0;
	uint32_t slice_width ,slice_height ;
	uint32_t store_slice_width,store_slice_height;
	uint32_t uv_tile_w_num,uv_tile_h_num;
	uint32_t y_tile_w_num,y_tile_h_num;
	uint32_t store_left_offset_tiles_num;
	uint32_t start_col,end_col,start_row, end_row;
	uint32_t overlap_up,overlap_down,overlap_left, overlap_right;
	struct slice_cfg_input *in_ptr = (struct slice_cfg_input *)cfg_in;
	struct isp_ifbc_store_info *frm_ifbc_store = NULL;
	struct isp_slice_desc *cur_slc = NULL;
	struct isp_slice_desc *cur_slc_temp = NULL;
	struct slice_ifbc_store_info *slc_ifbc_store = NULL;
	struct slice_scaler_info *slc_scaler = NULL;

	slice_col_num = slc_ctx->slice_col_num;
	cur_slc = &slc_ctx->slices[0];
	cur_slc_temp = &slc_ctx->slices[0];

	cur_slc = &slc_ctx->slices[0];

	for (i = 0; i < SLICE_NUM_MAX; i++, cur_slc++) {
		if (cur_slc->valid == 0)
			continue;
		for (j = 0; j < FBC_PATH_NUM; j++) {
			frm_ifbc_store = in_ptr->frame_ifbc_store[j];
			if (frm_ifbc_store == NULL || cur_slc->path_en[j] == 0)
				/* path is not valid. */
				continue;
			slc_ifbc_store = &cur_slc->slice_ifbc_store[j];
			slc_scaler = &cur_slc->slice_scaler[j];
			slice_id = i;

			start_col = cur_slc->slice_pos.start_col;
			end_col = cur_slc->slice_pos.end_col;
			start_row = cur_slc->slice_pos.start_row;
			end_row = cur_slc->slice_pos.end_row;

			overlap_up = cur_slc->slice_overlap.overlap_up;
			overlap_down = cur_slc->slice_overlap.overlap_down;
			overlap_left = cur_slc->slice_overlap.overlap_left;
			overlap_right = cur_slc->slice_overlap.overlap_right;

			slice_width = end_col - start_col + 1;
			slice_height = end_row - start_row + 1;
			store_slice_width = slice_width - overlap_left - overlap_right;
			store_slice_height = slice_height - overlap_up - overlap_down;

			store_left_offset_tiles_num = (start_col + overlap_left) / ISP_IFBC_PAD_W_YUV420;

			uv_tile_w_num= (store_slice_width + ISP_IFBC_PAD_W_YUV420 - 1) / ISP_IFBC_PAD_W_YUV420;
			uv_tile_w_num = (uv_tile_w_num + 2 - 1) / 2 * 2;
			uv_tile_h_num= (store_slice_height / 2 + ISP_IFBC_PAD_H_YUV420 - 1) / ISP_IFBC_PAD_H_YUV420;
			y_tile_w_num = uv_tile_w_num;
			y_tile_h_num = 2 * uv_tile_h_num;

			slc_ifbc_store->border.up_border = overlap_up;
			slc_ifbc_store->border.left_border = overlap_left;
			slc_ifbc_store->border.down_border = overlap_down;
			slc_ifbc_store->border.right_border = overlap_right;

			slc_ifbc_store->fbc_tile_number = uv_tile_w_num * uv_tile_h_num +
				y_tile_w_num * y_tile_h_num;
			slc_ifbc_store->fbc_size_in_ver = store_slice_height;
			slc_ifbc_store->fbc_size_in_hor = store_slice_width;
			slc_ifbc_store->fbc_y_tile_addr_init_x256 = frm_ifbc_store->y_tile_addr_init_x256
				+ store_left_offset_tiles_num * ISP_IFBC_BASE_ALIGN;
			slc_ifbc_store->fbc_c_tile_addr_init_x256 = frm_ifbc_store->c_tile_addr_init_x256
				+ store_left_offset_tiles_num * ISP_IFBC_BASE_ALIGN;
			slc_ifbc_store->fbc_y_header_addr_init = frm_ifbc_store->y_header_addr_init
				- store_left_offset_tiles_num / 2;
			slc_ifbc_store->fbc_c_header_addr_init = frm_ifbc_store->c_header_addr_init
				- store_left_offset_tiles_num / 2;
			slc_ifbc_store->slice_mode_en = 1;
			pr_debug("[%s] [slice id %d] tile_number %d\n", __func__,
				i, slc_ifbc_store->fbc_tile_number);
		}
	}

	return 0;
}
static int cfg_slice_3dnr_memctrl_info(
		void *cfg_in, struct isp_slice_context *slc_ctx)
{
	int ret = 0, idx = 0;

	struct slice_cfg_input *in_ptr = (struct slice_cfg_input *)cfg_in;
	struct isp_3dnr_ctx_desc *nr3_ctx  = in_ptr->nr3_ctx;
	struct isp_3dnr_mem_ctrl *mem_ctrl = &nr3_ctx->mem_ctrl;
	struct isp_slice_desc *cur_slc;

	struct slice_3dnr_memctrl_info *slc_3dnr_memctrl;

	uint32_t start_col = 0, end_col = 0;
	uint32_t start_row = 0, end_row = 0;

	uint32_t pitch_y = 0, pitch_u = 0;
	uint32_t ch0_offset = 0, ch1_offset = 0;

	pitch_y  = in_ptr->frame_in_size.w;
	pitch_u  = in_ptr->frame_in_size.w;

	cur_slc = &slc_ctx->slices[0];

	if (nr3_ctx->type == NR3_FUNC_OFF) {
		for (idx = 0; idx < SLICE_NUM_MAX; idx++, cur_slc++) {
			if (cur_slc->valid == 0)
				continue;
			slc_3dnr_memctrl = &cur_slc->slice_3dnr_memctrl;
			slc_3dnr_memctrl->bypass = 1;
		}

		return 0;
	}

	for (idx = 0; idx < SLICE_NUM_MAX; idx++, cur_slc++) {
		if (cur_slc->valid == 0)
			continue;
		slc_3dnr_memctrl = &cur_slc->slice_3dnr_memctrl;

		start_col = cur_slc->slice_pos.start_col;
		start_row = cur_slc->slice_pos.start_row;
		end_col = cur_slc->slice_pos.end_col;
		end_row = cur_slc->slice_pos.end_row;

		/* YUV420_2FRAME */
		ch0_offset = start_row * pitch_y + start_col;
		ch1_offset = ((start_row * pitch_u + 1) >> 1) + start_col;

		slc_3dnr_memctrl->addr.addr_ch0 = mem_ctrl->frame_addr.addr_ch0 +
			ch0_offset;
		slc_3dnr_memctrl->addr.addr_ch1 = mem_ctrl->frame_addr.addr_ch1 +
			ch1_offset;

		slc_3dnr_memctrl->bypass = mem_ctrl->bypass;
		slc_3dnr_memctrl->first_line_mode = 0;
		slc_3dnr_memctrl->last_line_mode = 0;
		slc_3dnr_memctrl->start_row    = start_row;
		slc_3dnr_memctrl->start_col    = start_col;
		slc_3dnr_memctrl->src.h   = end_row - start_row + 1;
		slc_3dnr_memctrl->src.w   = end_col - start_col + 1;
		slc_3dnr_memctrl->ft_y.h  = end_row - start_row + 1;
		slc_3dnr_memctrl->ft_y.w  = end_col - start_col + 1;
		slc_3dnr_memctrl->ft_uv.h = (end_row - start_row + 1) >> 1;
		slc_3dnr_memctrl->ft_uv.w = end_col - start_col + 1;

		pr_debug("memctrl param w[%d], h[%d] bypass %d\n",
			slc_3dnr_memctrl->src.w,
			slc_3dnr_memctrl->src.h,
			slc_3dnr_memctrl->bypass);
	}

	return ret;
}

static int cfg_slice_3dnr_fbd_fetch_info(
		void *cfg_in, struct isp_slice_context *slc_ctx)
{
	int ret = 0, idx = 0;
	uint32_t start_col, end_col, start_row, end_row;
	uint32_t overlap_up, overlap_down, overlap_left, overlap_right;

	uint32_t fetch_start_x, fetch_start_y, uv_fetch_start_y;
	uint32_t left_tiles_num, right_tiles_num, middle_tiles_num;
	uint32_t left_size, right_size, up_size, down_size;
	uint32_t up_tiles_num, down_tiles_num, vertical_middle_tiles_num;
	uint32_t left_offset_tiles_num, up_offset_tiles_num;

	int Y_start_x, Y_end_x, Y_start_y, Y_end_y;
	int UV_start_x, UV_end_x, UV_start_y, UV_end_y;
	int mv_x, mv_y;

	uint32_t slice_width, slice_height;
	uint32_t pad_slice_width, pad_slice_height;
	uint32_t tile_col, tile_row;
	uint32_t img_width, fbd_y_tiles_num_pitch;

	struct slice_cfg_input *in_ptr = (struct slice_cfg_input *)cfg_in;
	struct isp_3dnr_ctx_desc *nr3_ctx = in_ptr->nr3_ctx;
	struct isp_3dnr_fbd_fetch *nr3_fbd_fetch = &nr3_ctx->nr3_fbd_fetch;
	struct isp_slice_desc *cur_slc;
	struct slice_3dnr_fbd_fetch_info *slc_3dnr_fbd_fetch;

	img_width = nr3_ctx->mem_ctrl.img_width;
	fbd_y_tiles_num_pitch = (img_width + FBD_NR3_Y_WIDTH - 1) / FBD_NR3_Y_WIDTH;

	mv_x = nr3_ctx->mem_ctrl.mv_x;
	mv_y = nr3_ctx->mem_ctrl.mv_y;

	cur_slc = &slc_ctx->slices[0];
	for (idx = 0; idx < SLICE_NUM_MAX; idx++, cur_slc++) {
		if (cur_slc->valid == 0)
			continue;
		slc_3dnr_fbd_fetch = &cur_slc->slice_3dnr_fbd_fetch;

		start_col = cur_slc->slice_pos.start_col;
		end_col = cur_slc->slice_pos.end_col;
		start_row = cur_slc->slice_pos.start_row;
		end_row = cur_slc->slice_pos.end_row;
		overlap_up = cur_slc->slice_overlap.overlap_up;
		overlap_down = cur_slc->slice_overlap.overlap_down;
		overlap_left = cur_slc->slice_overlap.overlap_left;
		overlap_right = cur_slc->slice_overlap.overlap_right;

		slice_width = end_col - start_col + 1;
		slice_height = end_row - start_row + 1;
		pad_slice_width = (slice_width + FBD_NR3_Y_WIDTH - 1) /
			FBD_NR3_Y_WIDTH * FBD_NR3_Y_WIDTH;
		pad_slice_height = (slice_height + FBD_NR3_Y_PAD_HEIGHT - 1) /
			FBD_NR3_Y_PAD_HEIGHT * FBD_NR3_Y_PAD_HEIGHT;
		tile_col = pad_slice_width / FBD_NR3_Y_WIDTH;
		tile_row = pad_slice_height / FBD_NR3_Y_HEIGHT;

		left_offset_tiles_num = start_col / FBD_NR3_Y_WIDTH;

		slc_3dnr_fbd_fetch->fbd_y_pixel_size_in_hor = slice_width;
		slc_3dnr_fbd_fetch->fbd_y_pixel_size_in_ver = slice_height;
		slc_3dnr_fbd_fetch->fbd_c_pixel_size_in_hor = slice_width;
		slc_3dnr_fbd_fetch->fbd_c_pixel_size_in_ver = slice_height / 2;
		slc_3dnr_fbd_fetch->fbd_y_pixel_start_in_ver = mv_y & 0x1;
		slc_3dnr_fbd_fetch->fbd_c_pixel_start_in_ver = (mv_y / 2) & 0x1;

		fetch_start_x = (mv_x < 0) ? start_col : (start_col + mv_x);
		fetch_start_y = (mv_y < 0) ? start_row : (start_row + mv_y);
		uv_fetch_start_y = (mv_y < 0) ? start_row : (start_row + mv_y / 2);

		Y_start_x = fetch_start_x;
		UV_start_x = fetch_start_x;
		Y_start_y = fetch_start_y;
		UV_start_y = uv_fetch_start_y;
		if (mv_x < 0) {
			if ((0 == start_col) && (mv_x & 1)) {
				Y_start_x = fetch_start_x;
				UV_start_x = fetch_start_x + 2;
			} else if ((0 != start_col) && (mv_x & 1)) {
				Y_start_x = fetch_start_x + mv_x;
				UV_start_x = fetch_start_x + mv_x + 1;
			} else if (0 != start_col) {
				Y_start_x = fetch_start_x + mv_x;
				UV_start_x = fetch_start_x + mv_x;
			} else {
				Y_start_x = fetch_start_x;
				UV_start_x = fetch_start_x;
			}
		}else if (mv_x > 0) {
			if ((0 == start_col) && (mv_x & 1)) {
				Y_start_x = fetch_start_x;
				UV_start_x = fetch_start_x - 1;
			} else if ((0 != start_col) && (mv_x & 1)) {
				Y_start_x = fetch_start_x;
				UV_start_x = fetch_start_x - 1;
			} else {
				Y_start_x = fetch_start_x;
				UV_start_x = fetch_start_x;
			}
		}
		Y_end_x = slice_width + Y_start_x - 1;
		UV_end_x = slice_width + UV_start_x - 1;
		left_offset_tiles_num = Y_start_x / FBD_NR3_Y_WIDTH;
		if (Y_start_x % FBD_NR3_Y_WIDTH == 0) {
			left_tiles_num = 0;
			left_size = 0;
		} else {
			left_tiles_num = 1;
			left_size = FBD_NR3_Y_WIDTH - Y_start_x % FBD_NR3_Y_WIDTH;
		}
		if (((Y_end_x + 1) % FBD_NR3_Y_WIDTH == 0)
			|| (((Y_end_x + 1) > img_width)
			&& ((Y_end_x + 1) % FBD_NR3_Y_WIDTH == 1)))
			right_tiles_num = 0;
		else
			right_tiles_num = 1;
		right_size = (Y_end_x + 1) % FBD_NR3_Y_WIDTH;

		middle_tiles_num = (slice_width - left_size - right_size) / FBD_NR3_Y_WIDTH;
		slc_3dnr_fbd_fetch->fbd_y_pixel_start_in_hor = Y_start_x % FBD_NR3_Y_WIDTH;
		slc_3dnr_fbd_fetch->fbd_y_tiles_num_in_hor = left_tiles_num + right_tiles_num + middle_tiles_num;
		slc_3dnr_fbd_fetch->fbd_y_tiles_start_odd = left_offset_tiles_num % 2;
		left_offset_tiles_num = UV_start_x / FBD_NR3_Y_WIDTH;
		if (UV_start_x % FBD_NR3_Y_WIDTH == 0) {
			left_tiles_num = 0;
			left_size = 0;
		} else {
			left_tiles_num = 1;
			left_size = FBD_NR3_Y_WIDTH - UV_start_x %  FBD_NR3_Y_WIDTH;
		}
		if ((UV_end_x + 1) % FBD_NR3_Y_WIDTH == 0)
			right_tiles_num = 0;
		else
			right_tiles_num = 1;
		right_size = (UV_end_x + 1) % FBD_NR3_Y_WIDTH;
		middle_tiles_num= (slice_width - left_size - right_size) / FBD_NR3_Y_WIDTH;
		slc_3dnr_fbd_fetch->fbd_c_pixel_start_in_hor= UV_start_x % FBD_NR3_Y_WIDTH;
		slc_3dnr_fbd_fetch->fbd_c_tiles_num_in_hor= left_tiles_num + right_tiles_num + middle_tiles_num;
		slc_3dnr_fbd_fetch->fbd_c_tiles_start_odd= left_offset_tiles_num % 2;
		if (mv_y < 0) {
			Y_start_y = fetch_start_y;
			UV_start_y = fetch_start_y;
		} else if (mv_y > 0) {
			Y_start_y = fetch_start_y;
			UV_start_y = uv_fetch_start_y;
		}
		Y_end_y = slice_height + Y_start_y - 1;
		UV_end_y = slice_height + UV_start_y - 1;
		up_offset_tiles_num = Y_start_y / FBD_NR3_Y_HEIGHT;
		if (Y_start_y % FBD_NR3_Y_HEIGHT == 0) {
			up_tiles_num = 0;
			up_size = 0;
		} else {
			up_tiles_num = 1;
			up_size = FBD_NR3_Y_HEIGHT - Y_start_y % FBD_NR3_Y_HEIGHT;
		}
		if ((Y_end_y + 1) % FBD_NR3_Y_HEIGHT == 0)
			down_tiles_num = 0;
		else
			down_tiles_num = 1;
		down_size = (Y_end_y + 1) % FBD_NR3_Y_HEIGHT;
		vertical_middle_tiles_num= (slice_height - up_size - down_size) / FBD_NR3_Y_HEIGHT;
		slc_3dnr_fbd_fetch->fbd_y_pixel_start_in_ver = Y_start_y % FBD_NR3_Y_HEIGHT;
		slc_3dnr_fbd_fetch->fbd_y_tiles_num_in_ver= up_tiles_num + down_tiles_num + vertical_middle_tiles_num;
		slc_3dnr_fbd_fetch->fbd_y_header_addr_init = nr3_fbd_fetch->y_header_addr_init
			- (left_offset_tiles_num + up_offset_tiles_num* fbd_y_tiles_num_pitch) / 2;
		slc_3dnr_fbd_fetch->fbd_y_tile_addr_init_x256 = nr3_fbd_fetch->y_tile_addr_init_x256
			+ (left_offset_tiles_num + up_offset_tiles_num * fbd_y_tiles_num_pitch)* FBC_NR3_BASE_ALIGN;
		up_offset_tiles_num = UV_start_y / FBD_NR3_Y_HEIGHT;
		if (UV_start_y %  FBD_BAYER_HEIGHT == 0) {
			up_tiles_num = 0;
			up_size = 0;
		} else {
			up_tiles_num = 1;
			up_size = FBD_NR3_Y_HEIGHT - UV_start_y %  FBD_NR3_Y_HEIGHT;
		}
		if ((UV_end_y + 1) % FBD_NR3_Y_HEIGHT == 0)
			down_tiles_num = 0;
		else
			down_tiles_num = 1;
		down_size = (UV_end_y + 1) % FBD_NR3_Y_HEIGHT;
		vertical_middle_tiles_num = (slice_height / 2 - up_size- down_size)/FBD_NR3_Y_HEIGHT;
		slc_3dnr_fbd_fetch->fbd_c_pixel_start_in_ver = UV_start_y % FBD_NR3_Y_WIDTH;
		slc_3dnr_fbd_fetch->fbd_c_tiles_num_in_ver = up_tiles_num + down_tiles_num
			+ vertical_middle_tiles_num;
		slc_3dnr_fbd_fetch->fbd_c_header_addr_init = nr3_fbd_fetch->c_header_addr_init
			- (left_offset_tiles_num + up_offset_tiles_num* fbd_y_tiles_num_pitch) / 2;
		slc_3dnr_fbd_fetch->fbd_c_tile_addr_init_x256 = nr3_fbd_fetch->c_tile_addr_init_x256
			+ (left_offset_tiles_num + up_offset_tiles_num* fbd_y_tiles_num_pitch) * FBC_NR3_BASE_ALIGN;

		slc_3dnr_fbd_fetch->fbd_y_tiles_num_pitch = nr3_fbd_fetch->y_tiles_num_pitch;

	}

	return ret;
}

static int cfg_slice_3dnr_store_info(
		void *cfg_in, struct isp_slice_context *slc_ctx)
{
	int ret = 0, idx = 0;

	/* struct slice_overlap_info */
	uint32_t overlap_left  = 0;
	uint32_t overlap_up    = 0;
	uint32_t overlap_right = 0;
	uint32_t overlap_down  = 0;
	/* struct slice_pos_info */
	uint32_t start_col     = 0;
	uint32_t end_col       = 0;
	uint32_t start_row     = 0;
	uint32_t end_row       = 0;

	/* struct slice_pos_info */
	uint32_t orig_s_col    = 0;
	uint32_t orig_s_row    = 0;
	uint32_t orig_e_col    = 0;
	uint32_t orig_e_row    = 0;

	uint32_t pitch_y = 0;
	uint32_t pitch_u = 0;
	uint32_t ch0_offset = 0;
	uint32_t ch1_offset = 0;

	struct slice_cfg_input *in_ptr = (struct slice_cfg_input *)cfg_in;
	struct isp_3dnr_ctx_desc *nr3_ctx  = in_ptr->nr3_ctx;
	struct isp_3dnr_store *nr3_store   = &nr3_ctx->nr3_store;
	struct isp_slice_desc *cur_slc;

	struct slice_3dnr_store_info *slc_3dnr_store;

	pitch_y  = in_ptr->frame_in_size.w;
	pitch_u  = in_ptr->frame_in_size.w;

	cur_slc = &slc_ctx->slices[0];
	if (nr3_ctx->type == NR3_FUNC_OFF) {
		for (idx = 0; idx < SLICE_NUM_MAX; idx++, cur_slc++) {
			if (cur_slc->valid == 0)
				continue;
			slc_3dnr_store = &cur_slc->slice_3dnr_store;
			slc_3dnr_store->bypass = 1;
		}

		return 0;
	}

	for (idx = 0; idx < SLICE_NUM_MAX; idx++, cur_slc++) {
		if (cur_slc->valid == 0)
			continue;
		slc_3dnr_store = &cur_slc->slice_3dnr_store;

		start_col = cur_slc->slice_pos.start_col;
		start_row = cur_slc->slice_pos.start_row;
		end_col   = cur_slc->slice_pos.end_col;
		end_row   = cur_slc->slice_pos.end_row;

		orig_s_col = cur_slc->slice_pos_orig.start_col;
		orig_s_row = cur_slc->slice_pos_orig.start_row;
		orig_e_col = cur_slc->slice_pos_orig.end_col;
		orig_e_row = cur_slc->slice_pos_orig.end_row;

		overlap_left  = cur_slc->slice_overlap.overlap_left;
		overlap_up    = cur_slc->slice_overlap.overlap_up;
		overlap_right = cur_slc->slice_overlap.overlap_right;
		overlap_down  = cur_slc->slice_overlap.overlap_down;

		/* YUV420_2FRAME */
		ch0_offset = orig_s_row * pitch_y + orig_s_col;
		ch1_offset = ((orig_s_row * pitch_u + 1) >> 1) + orig_s_col;

		slc_3dnr_store->bypass = nr3_store->st_bypass;
		slc_3dnr_store->addr.addr_ch0 = nr3_store->st_luma_addr +
			ch0_offset;
		slc_3dnr_store->addr.addr_ch1 = nr3_store->st_chroma_addr +
			ch1_offset;

		slc_3dnr_store->size.w = orig_e_col - orig_s_col + 1;
		slc_3dnr_store->size.h = orig_e_row - orig_s_row + 1;

		pr_debug("store w[%d], h[%d] bypass %d\n", slc_3dnr_store->size.w,
			slc_3dnr_store->size.h, slc_3dnr_store->bypass);
	}

	return ret;
}

static int cfg_slice_3dnr_fbc_store_info(
		void *cfg_in, struct isp_slice_context *slc_ctx)
{
	int ret = 0, idx = 0;

	uint32_t slice_width ,slice_height ;
	uint32_t store_slice_width,store_slice_height;
	uint32_t uv_tile_w_num,uv_tile_h_num;
	uint32_t y_tile_w_num,y_tile_h_num;
	uint32_t store_left_offset_tiles_num;
	uint32_t start_col,end_col,start_row, end_row;
	uint32_t overlap_up,overlap_down,overlap_left, overlap_right;
	struct slice_cfg_input *in_ptr = (struct slice_cfg_input *)cfg_in;
	struct isp_3dnr_ctx_desc *nr3_ctx = in_ptr->nr3_ctx;
	struct isp_3dnr_fbc_store *nr3_fbc_store = &nr3_ctx->nr3_fbc_store;
	struct isp_slice_desc *cur_slc;
	struct slice_3dnr_fbc_store_info *slc_3dnr_fbc_store;

	cur_slc = &slc_ctx->slices[0];

	if (nr3_ctx->type == NR3_FUNC_OFF) {
		for (idx = 0; idx < SLICE_NUM_MAX; idx++, cur_slc++) {
			if (cur_slc->valid == 0)
				continue;
			slc_3dnr_fbc_store = &cur_slc->slice_3dnr_fbc_store;
			slc_3dnr_fbc_store->bypass = 1;
		}

		return 0;
	}

	for (idx = 0; idx < SLICE_NUM_MAX; idx++, cur_slc++) {
		if (cur_slc->valid == 0)
			continue;
		slc_3dnr_fbc_store = &cur_slc->slice_3dnr_fbc_store;

		start_col = cur_slc->slice_pos.start_col;
		end_col = cur_slc->slice_pos.end_col;
		start_row = cur_slc->slice_pos.start_row;
		end_row = cur_slc->slice_pos.end_row;
		overlap_up = cur_slc->slice_overlap.overlap_up;
		overlap_down = cur_slc->slice_overlap.overlap_down;
		overlap_left = cur_slc->slice_overlap.overlap_left;
		overlap_right = cur_slc->slice_overlap.overlap_right;

		slice_width = end_col - start_col + 1;
		slice_height = end_row - start_row + 1;
		store_slice_width = slice_width - overlap_left - overlap_right;
		store_slice_height = slice_height - overlap_up - overlap_down;

		store_left_offset_tiles_num = (start_col + overlap_left) / FBC_NR3_Y_WIDTH;

		uv_tile_w_num= (store_slice_width + FBC_NR3_Y_WIDTH - 1) / FBC_NR3_Y_WIDTH;
		uv_tile_w_num = (uv_tile_w_num + 2 - 1) / 2 * 2;
		uv_tile_h_num= (store_slice_height / 2 + FBC_NR3_Y_HEIGHT - 1) / FBC_NR3_Y_HEIGHT;
		y_tile_w_num = uv_tile_w_num;
		y_tile_h_num = 2 * uv_tile_h_num;

		slc_3dnr_fbc_store->fbc_tile_number = uv_tile_w_num * uv_tile_h_num +
			y_tile_w_num * y_tile_h_num;
		slc_3dnr_fbc_store->fbc_size_in_ver = store_slice_height;
		slc_3dnr_fbc_store->fbc_size_in_hor = store_slice_width;
		slc_3dnr_fbc_store->fbc_y_tile_addr_init_x256 = nr3_fbc_store->y_tile_addr_init_x256
			+ store_left_offset_tiles_num * FBC_NR3_BASE_ALIGN;
		slc_3dnr_fbc_store->fbc_c_tile_addr_init_x256 = nr3_fbc_store->c_tile_addr_init_x256
			+ store_left_offset_tiles_num * FBC_NR3_BASE_ALIGN;
		slc_3dnr_fbc_store->fbc_y_header_addr_init = nr3_fbc_store->y_header_addr_init
			- store_left_offset_tiles_num / 2;
		slc_3dnr_fbc_store->fbc_c_header_addr_init = nr3_fbc_store->c_header_addr_init
			- store_left_offset_tiles_num / 2;
		slc_3dnr_fbc_store->slice_mode_en = 1;
		slc_3dnr_fbc_store->bypass = nr3_fbc_store->bypass;
		pr_debug("[%s] [slice id %d] tile_number %d\n", __func__,
			idx, slc_3dnr_fbc_store->fbc_tile_number);
	}

	return ret;
}

static int cfg_slice_3dnr_crop_info(
		void *cfg_in, struct isp_slice_context *slc_ctx)
{
	int ret = 0;
	int idx = 0;

	struct slice_cfg_input *in_ptr = (struct slice_cfg_input *)cfg_in;
	struct isp_3dnr_ctx_desc *nr3_ctx  = in_ptr->nr3_ctx;

	/* struct slice_overlap_info */
	uint32_t overlap_left  = 0;
	uint32_t overlap_up    = 0;
	uint32_t overlap_right = 0;
	uint32_t overlap_down  = 0;

	struct isp_slice_desc *cur_slc;

	struct slice_3dnr_memctrl_info *slc_3dnr_memctrl;
	struct slice_3dnr_store_info *slc_3dnr_store;
	struct slice_3dnr_crop_info *slc_3dnr_crop;

	cur_slc = &slc_ctx->slices[0];

	if (nr3_ctx->type == NR3_FUNC_OFF) {
		for (idx = 0; idx < SLICE_NUM_MAX; idx++, cur_slc++) {
			if (cur_slc->valid == 0)
				continue;
			slc_3dnr_crop = &cur_slc->slice_3dnr_crop;
			slc_3dnr_crop->bypass = 1;
		}

		return 0;
	}

	for (idx = 0; idx < SLICE_NUM_MAX; idx++, cur_slc++) {
		if (cur_slc->valid == 0)
			continue;
		slc_3dnr_memctrl = &cur_slc->slice_3dnr_memctrl;
		slc_3dnr_store   = &cur_slc->slice_3dnr_store;
		slc_3dnr_crop	 = &cur_slc->slice_3dnr_crop;

		overlap_left  = cur_slc->slice_overlap.overlap_left;
		overlap_up    = cur_slc->slice_overlap.overlap_up;
		overlap_right = cur_slc->slice_overlap.overlap_right;
		overlap_down  = cur_slc->slice_overlap.overlap_down;

		slc_3dnr_crop->bypass = !(overlap_left  || overlap_up ||
						overlap_right || overlap_down);

		slc_3dnr_crop->src.w = slc_3dnr_memctrl->src.w;
		slc_3dnr_crop->src.h = slc_3dnr_memctrl->src.h;
		slc_3dnr_crop->dst.w = slc_3dnr_store->size.w;
		slc_3dnr_crop->dst.h = slc_3dnr_store->size.h;
		slc_3dnr_crop->start_x = overlap_left;
		slc_3dnr_crop->start_y = overlap_up;

		pr_debug("crop src.w[%d], src.h[%d], des.w[%d], des.h[%d]",
			slc_3dnr_crop->src.w, slc_3dnr_crop->src.h,
			slc_3dnr_crop->dst.w, slc_3dnr_crop->dst.h);
		pr_debug("ovx[%d], ovx[%d] bypass %d\n",
			slc_3dnr_crop->start_x, slc_3dnr_crop->start_y,
			slc_3dnr_crop->bypass);
	}

	return ret;
}

static int cfg_slice_3dnr_memctrl_update_info(
		void *cfg_in, struct isp_slice_context *slc_ctx)
{
	int ret = 0, idx = 0;

	struct slice_cfg_input *in_ptr = (struct slice_cfg_input *)cfg_in;
	struct isp_3dnr_ctx_desc *nr3_ctx  = in_ptr->nr3_ctx;
	struct isp_slice_desc *cur_slc;

	struct nr3_slice nr3_fw_in;
	struct nr3_slice_for_blending nr3_fw_out;

	struct slice_3dnr_memctrl_info *slc_3dnr_memctrl;

	memset((void *)&nr3_fw_in, 0, sizeof(nr3_fw_in));
	memset((void *)&nr3_fw_out, 0, sizeof(nr3_fw_out));

	nr3_fw_in.cur_frame_width  = in_ptr->frame_in_size.w;
	nr3_fw_in.cur_frame_height = in_ptr->frame_in_size.h;
	nr3_fw_in.mv_x = nr3_ctx->mv.mv_x;
	nr3_fw_in.mv_y = nr3_ctx->mv.mv_y;

	/* slice_num != 1, just pass current slice info */
	nr3_fw_in.slice_num = slc_ctx->slice_num;

	cur_slc = &slc_ctx->slices[0];

	if (nr3_ctx->type == NR3_FUNC_OFF)
		return 0;

	for (idx = 0; idx < SLICE_NUM_MAX; idx++, cur_slc++) {
		if (cur_slc->valid == 0)
			continue;

		slc_3dnr_memctrl = &cur_slc->slice_3dnr_memctrl;

		nr3_fw_in.end_col = cur_slc->slice_pos.end_col;
		nr3_fw_in.end_row = cur_slc->slice_pos.end_row;
		nr3_fw_out.first_line_mode = slc_3dnr_memctrl->first_line_mode;
		nr3_fw_out.last_line_mode  = slc_3dnr_memctrl->last_line_mode;
		nr3_fw_out.src_lum_addr    = slc_3dnr_memctrl->addr.addr_ch0;
		nr3_fw_out.src_chr_addr    = slc_3dnr_memctrl->addr.addr_ch1;
		nr3_fw_out.ft_y_width      = slc_3dnr_memctrl->ft_y.w;
		nr3_fw_out.ft_y_height     = slc_3dnr_memctrl->ft_y.h;
		nr3_fw_out.ft_uv_width     = slc_3dnr_memctrl->ft_uv.w;
		nr3_fw_out.ft_uv_height    = slc_3dnr_memctrl->ft_uv.h;
		nr3_fw_out.start_col       = slc_3dnr_memctrl->start_col;
		nr3_fw_out.start_row       = slc_3dnr_memctrl->start_row;

		pr_debug("slice_num[%d], frame_width[%d], frame_height[%d]",
			nr3_fw_in.slice_num, nr3_fw_in.cur_frame_width,
			nr3_fw_in.cur_frame_height);
		pr_debug("start_col[%d], start_row[%d]",
			nr3_fw_out.start_col, nr3_fw_out.start_row);
		pr_debug("src_lum_addr[0x%x], src_chr_addr[0x%x]\n",
			nr3_fw_out.src_lum_addr, nr3_fw_out.src_chr_addr);
		pr_debug("ft_y_width[%d], ft_y_height[%d], ft_uv_width[%d]",
			nr3_fw_out.ft_y_width, nr3_fw_out.ft_y_height,
			nr3_fw_out.ft_uv_width);
		pr_debug("ft_uv_height[%d], last_line_mode[%d]",
			nr3_fw_out.ft_uv_height, nr3_fw_out.last_line_mode);
		pr_debug("first_line_mode[%d]\n", nr3_fw_out.first_line_mode);

		isp_3dnr_update_memctrl_slice_info(&nr3_fw_in, &nr3_fw_out);

		slc_3dnr_memctrl->first_line_mode = nr3_fw_out.first_line_mode;
		slc_3dnr_memctrl->last_line_mode  = nr3_fw_out.last_line_mode;
		slc_3dnr_memctrl->addr.addr_ch0   = nr3_fw_out.src_lum_addr;
		slc_3dnr_memctrl->addr.addr_ch1   = nr3_fw_out.src_chr_addr;
		slc_3dnr_memctrl->ft_y.w  = nr3_fw_out.ft_y_width;
		slc_3dnr_memctrl->ft_y.h  = nr3_fw_out.ft_y_height;
		slc_3dnr_memctrl->ft_uv.w = nr3_fw_out.ft_uv_width;
		slc_3dnr_memctrl->ft_uv.h = nr3_fw_out.ft_uv_height;

		pr_debug("slice_num[%d], frame_width[%d], frame_height[%d], ",
			nr3_fw_in.slice_num, nr3_fw_in.cur_frame_width,
			nr3_fw_in.cur_frame_height);
		pr_debug("start_col[%d], start_row[%d], ",
			nr3_fw_out.start_col, nr3_fw_out.start_row);
		pr_debug("src_lum_addr[0x%x], src_chr_addr[0x%x]\n",
			nr3_fw_out.src_lum_addr, nr3_fw_out.src_chr_addr);
		pr_debug("ft_y_width[%d], ft_y_height[%d], ft_uv_width[%d], ",
			nr3_fw_out.ft_y_width, nr3_fw_out.ft_y_height,
			nr3_fw_out.ft_uv_width);
		pr_debug("ft_uv_height[%d], last_line_mode[%d],	",
			nr3_fw_out.ft_uv_height, nr3_fw_out.last_line_mode);
		pr_debug("first_line_mode[%d]\n", nr3_fw_out.first_line_mode);
	}

	return ret;
}

int isp_cfg_slice_3dnr_info(
		void *cfg_in, struct isp_slice_context *slc_ctx)
{
	int ret = 0;
	struct slice_cfg_input *in_ptr = (struct slice_cfg_input *)cfg_in;

	ret = cfg_slice_3dnr_memctrl_info(cfg_in, slc_ctx);
	if (ret) {
		pr_err("fail to set slice 3dnr mem ctrl!\n");
		goto exit;
	}

	ret = cfg_slice_3dnr_memctrl_update_info(cfg_in, slc_ctx);
	if (ret) {
		pr_err("fail to update slice 3dnr mem ctrl!\n");
		goto exit;
	}

	ret = cfg_slice_3dnr_store_info(cfg_in, slc_ctx);
	if (ret) {
		pr_err("fail to set slice 3dnr store info!\n");
		goto exit;
	}

	if (!in_ptr->nr3_ctx->nr3_fbc_store.bypass) {
		ret = cfg_slice_3dnr_fbc_store_info(cfg_in, slc_ctx);
		if (ret) {
			pr_err("fail to set slice 3dnr fbc store info!\n");
			goto exit;
		}
	}

	if (in_ptr->nr3_ctx->mem_ctrl.nr3_ft_path_sel) {
		ret = cfg_slice_3dnr_fbd_fetch_info(cfg_in, slc_ctx);
		if (ret) {
			pr_err("fail to set slice 3dnr fbd fetch ctrl!\n");
			goto exit;
		}
	}

	ret = cfg_slice_3dnr_crop_info(cfg_in, slc_ctx);
	if (ret) {
		pr_err("fail to set slice 3dnr crop info!\n");
		goto exit;
	}

exit:
	return ret;
}

int isp_cfg_slice_ltm_info(void *cfg_in,
	struct isp_slice_context *slc_ctx,
	enum isp_ltm_region ltm_id)
{
	int ret = 0, idx = 0;
	struct slice_cfg_input *in_ptr = (struct slice_cfg_input *)cfg_in;
	struct isp_ltm_ctx_desc *ltm_ctx  = in_ptr->ltm_ctx;
	struct isp_ltm_rtl_param  rtl_param;
	struct isp_ltm_rtl_param  *prtl = &rtl_param;

	struct isp_slice_desc *cur_slc = NULL;
	struct slice_ltm_map_info *slc_ltm_map;
	uint32_t slice_info[4];

	struct isp_ltm_map   *map   = &ltm_ctx->map[ltm_id];

	if (map->bypass)
		return 0;

	if (ltm_ctx->type == MODE_LTM_OFF) {
		for (idx = 0; idx < SLICE_NUM_MAX; idx++) {
			cur_slc = &slc_ctx->slices[idx];
			if (cur_slc->valid == 0)
				continue;
			slc_ltm_map = &cur_slc->slice_ltm_map[ltm_id];
			slc_ltm_map->bypass = 1;
		}

		return 0;
	}

	for (idx = 0; idx < SLICE_NUM_MAX; idx++) {
		cur_slc = &slc_ctx->slices[idx];
		if (cur_slc->valid == 0)
			continue;
		slc_ltm_map = &cur_slc->slice_ltm_map[ltm_id];

		slice_info[0] = cur_slc->slice_pos.start_col;
		slice_info[1] = cur_slc->slice_pos.start_row;
		slice_info[2] = cur_slc->slice_pos.end_col;
		slice_info[3] = cur_slc->slice_pos.end_row;

		isp_ltm_gen_map_slice_config(ltm_ctx, ltm_id, prtl, slice_info);

		slc_ltm_map->tile_width = map->tile_width;
		slc_ltm_map->tile_height = map->tile_height;

		slc_ltm_map->tile_num_x = prtl->tile_x_num_rtl;
		slc_ltm_map->tile_num_y = prtl->tile_y_num_rtl;
		slc_ltm_map->tile_right_flag = prtl->tile_right_flag_rtl;
		slc_ltm_map->tile_left_flag  = prtl->tile_left_flag_rtl;
		slc_ltm_map->tile_start_x = prtl->tile_start_x_rtl;
		slc_ltm_map->tile_start_y = prtl->tile_start_y_rtl;
		slc_ltm_map->mem_addr = map->mem_init_addr +
				prtl->tile_index_xs * 128 * 2;
		pr_debug("ltm slice info: tile_num_x[%d], tile_num_y[%d], tile_right_flag[%d]\
			tile_left_flag[%d], tile_start_x[%d], tile_start_y[%d], mem_addr[0x%x]\n",
			slc_ltm_map->tile_num_x, slc_ltm_map->tile_num_y,
			slc_ltm_map->tile_right_flag, slc_ltm_map->tile_left_flag,
			slc_ltm_map->tile_start_x, slc_ltm_map->tile_start_y,
			slc_ltm_map->mem_addr);
	}

	return ret;
}

int isp_cfg_slice_noisefilter_info(void *cfg_in, struct isp_slice_context *slc_ctx){

	int ret = 0, rtn = 0;
	struct isp_slice_desc *cur_slc;
	struct slice_cfg_input *in_ptr = (struct slice_cfg_input *)cfg_in;
	struct isp_k_block  *isp_k_param = in_ptr->nofilter_ctx;

	if (!slc_ctx){
		pr_err("fail to get input ptr, null.\n");
		return -EFAULT;
	}

	cur_slc = &slc_ctx->slices[0];
	cur_slc->slice_noisefilter_mode.seed_for_mode1 = isp_k_param->seed0_for_mode1;
	cur_slc->slice_noisefilter_mode.yrandom_mode = isp_k_param->yrandom_mode;

	rtn=sprd_ispslice_noisefliter_info_set(cur_slc, slc_ctx);

	return ret;

}

int isp_cfg_slices(void *cfg_in,
		struct isp_slice_context *slc_ctx,
		uint32_t *valid_slc_num)
{
	int ret = 0;
	struct slice_cfg_input *in_ptr = (struct slice_cfg_input *)cfg_in;

	if (!in_ptr || !slc_ctx) {
		pr_err("fail to get input ptr, null.\n");
		return -EFAULT;
	}
	memset(slc_ctx, 0, sizeof(struct isp_slice_context));

	cfg_slice_base_info(in_ptr, slc_ctx, valid_slc_num);

	cfg_slice_scaler_info(in_ptr, slc_ctx);

	cfg_slice_nr_info(in_ptr, slc_ctx);

	return ret;
}

void *get_isp_slice_ctx()
{
	struct isp_slice_context *ptr;

	ptr = vzalloc(sizeof(struct isp_slice_context));
	if (IS_ERR_OR_NULL(ptr))
		return NULL;

	return ptr;
}

int put_isp_slice_ctx(void **slc_ctx)
{
	if (*slc_ctx)
		vfree(*slc_ctx);
	*slc_ctx = NULL;
	return 0;
}



/*  following section is fmcu cmds of register update for slice */
static unsigned long store_base[ISP_SPATH_NUM] = {
	ISP_STORE_PRE_CAP_BASE,
	ISP_STORE_VID_BASE,
	ISP_STORE_THUMB_BASE,
};

static unsigned long scaler_base[ISP_SPATH_NUM] = {
	ISP_SCALER_PRE_CAP_BASE,
	ISP_SCALER_VID_BASE,
	ISP_SCALER_THUMB_BASE,
};

uint32_t ap_fmcu_reg_get(struct isp_fmcu_ctx_desc *fmcu,
	uint32_t reg)
{
	uint32_t addr = 0;
	if (fmcu)
		addr = ISP_GET_REG(reg);
	else
		addr = reg;

	return addr;
}

void ap_fmcu_reg_write(struct isp_fmcu_ctx_desc *fmcu,
	uint32_t ctx_id, uint32_t addr, uint32_t cmd)
{
	if (fmcu)
		FMCU_PUSH(fmcu, addr, cmd);
	else
		ISP_REG_WR(ctx_id, addr, cmd);
}

static int set_fmcu_cfg(struct isp_fmcu_ctx_desc *fmcu,
	enum isp_context_hw_id ctx_id)
{
	uint32_t addr = 0, cmd = 0;
	unsigned long base;
	unsigned long reg_addr[ISP_CONTEXT_HW_NUM] = {
		ISP_CFG_PRE0_START,
		ISP_CFG_CAP0_START,
		ISP_CFG_PRE1_START,
		ISP_CFG_CAP1_START,
	};

	addr = ISP_GET_REG(reg_addr[ctx_id]);
	cmd = 1;
	FMCU_PUSH(fmcu, addr, cmd);

	/*
	 * When setting CFG_TRIGGER_PULSE cmd, fmcu will wait
	 * until CFG module configs isp registers done.
	 */
	if (fmcu->fid == 0)
		base =  ISP_FMCU0_BASE;
	else
		base =  ISP_FMCU1_BASE;
	addr = ISP_GET_REG(base + ISP_FMCU_CMD);
	cmd = CFG_TRIGGER_PULSE;
	FMCU_PUSH(fmcu, addr, cmd);

	return 0;
}

static int set_slice_nr_info(
	struct isp_fmcu_ctx_desc *fmcu,
	struct isp_slice_desc *cur_slc)
{
	uint32_t addr = 0, cmd = 0;

	/* NLM */
	addr = ISP_GET_REG(ISP_NLM_RADIAL_1D_DIST);
	cmd = ((cur_slc->slice_nlm.center_y_relative & 0x3FFF) << 16) |
		(cur_slc->slice_nlm.center_x_relative & 0x3FFF);
	FMCU_PUSH(fmcu, addr, cmd);

	/* Post CDN */
	addr = ISP_GET_REG(ISP_POSTCDN_SLICE_CTRL);
	cmd = cur_slc->slice_postcdn.start_row_mod4;
	FMCU_PUSH(fmcu, addr, cmd);

	/* YNR */
	addr = ISP_GET_REG(ISP_YNR_CFG31);
	cmd = ((cur_slc->slice_ynr.center_offset_y & 0xFFFF) << 16) |
		(cur_slc->slice_ynr.center_offset_x & 0xFFFF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_YNR_CFG33);
	cmd = ((cur_slc->slice_ynr.slice_height & 0xFFFF) << 16) |
		(cur_slc->slice_ynr.slice_width & 0xFFFF);
	FMCU_PUSH(fmcu, addr, cmd);

	return 0;
}

static int set_slice_fetch(struct isp_fmcu_ctx_desc *fmcu,
		struct slice_fetch_info *fetch_info)
{
	uint32_t addr = 0, cmd = 0;

	addr = ISP_GET_REG(ISP_FETCH_MEM_SLICE_SIZE);
	cmd = ((fetch_info->size.h & 0xFFFF) << 16) |
			(fetch_info->size.w & 0xFFFF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_FETCH_SLICE_Y_ADDR);
	cmd = fetch_info->addr.addr_ch0;
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_FETCH_SLICE_U_ADDR);
	cmd = fetch_info->addr.addr_ch1;
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_FETCH_SLICE_V_ADDR);
	cmd = fetch_info->addr.addr_ch2;
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_FETCH_MIPI_INFO);
	cmd = fetch_info->mipi_word_num |
			(fetch_info->mipi_byte_rel_pos << 16);
	FMCU_PUSH(fmcu, addr, cmd);

	/* dispatch size same as fetch size */
	addr = ISP_GET_REG(ISP_DISPATCH_CH0_SIZE);
	cmd = ((fetch_info->size.h & 0xFFFF) << 16) |
			(fetch_info->size.w & 0xFFFF);
	FMCU_PUSH(fmcu, addr, cmd);

	return 0;
}

static int set_slice_spath_thumbscaler(
		struct isp_fmcu_ctx_desc *fmcu,
		uint32_t path_en,
		uint32_t ctx_idx,
		struct slice_thumbscaler_info *slc_scaler)
{
	uint32_t addr = 0, cmd = 0;

	if (!path_en)
		return 0;

	addr = ISP_GET_REG(ISP_THMB_BEFORE_TRIM_SIZE);
	cmd = ((slc_scaler->src0.h & 0x1FFF) << 16) |
		(slc_scaler->src0.w & 0x1FFF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_THMB_Y_SLICE_SRC_SIZE);
	cmd = ((slc_scaler->y_src_after_deci.h & 0x1FFF) << 16) |
		(slc_scaler->y_src_after_deci.w & 0x1FFF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_THMB_Y_DES_SIZE);
	cmd = ((slc_scaler->y_dst_after_scaler.h & 0x3FF) << 16) |
		(slc_scaler->y_dst_after_scaler.w & 0x3FF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_THMB_Y_TRIM0_START);
	cmd = ((slc_scaler->y_trim.start_y & 0x1FFF) << 16) |
		(slc_scaler->y_trim.start_x & 0x1FFF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_THMB_Y_TRIM0_SIZE);
	cmd = ((slc_scaler->y_trim.size_y & 0x1FFF) << 16) |
		(slc_scaler->y_trim.size_x & 0x1FFF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_THMB_Y_INIT_PHASE);
	cmd = ((slc_scaler->y_init_phase.h & 0x3FF) << 16) |
		(slc_scaler->y_init_phase.w & 0x3FF);
	FMCU_PUSH(fmcu, addr, cmd);


	addr = ISP_GET_REG(ISP_THMB_UV_SLICE_SRC_SIZE);
	cmd = ((slc_scaler->uv_src_after_deci.h & 0x1FFF) << 16) |
		(slc_scaler->uv_src_after_deci.w & 0x1FFF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_THMB_UV_DES_SIZE);
	cmd = ((slc_scaler->uv_dst_after_scaler.h & 0x3FF) << 16) |
		(slc_scaler->uv_dst_after_scaler.w & 0x3FF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_THMB_UV_TRIM0_START);
	cmd = ((slc_scaler->uv_trim.start_y & 0x1FFF) << 16) |
		(slc_scaler->uv_trim.start_x & 0x1FFF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_THMB_UV_TRIM0_SIZE);
	cmd = ((slc_scaler->uv_trim.size_y & 0x1FFF) << 16) |
		(slc_scaler->uv_trim.size_x & 0x1FFF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_THMB_UV_INIT_PHASE);
	cmd = ((slc_scaler->uv_init_phase.h & 0x3FF) << 16) |
		(slc_scaler->uv_init_phase.w & 0x3FF);
	FMCU_PUSH(fmcu, addr, cmd);

	return 0;
}

static int set_slice_spath_scaler(
		struct isp_fmcu_ctx_desc *fmcu,
		uint32_t path_en,
		uint32_t ctx_idx,
		enum isp_sub_path_id spath_id,
		struct slice_scaler_info *slc_scaler)
{
	uint32_t addr = 0, cmd = 0;
	unsigned long base = scaler_base[spath_id];

	if (!path_en) {
		addr = ISP_GET_REG(ISP_SCALER_CFG) + base;
		cmd = (0 << 31) | (1 << 8) | (1 << 9);
		FMCU_PUSH(fmcu, addr, cmd);
		return 0;
	}

	/* bit31 enable path */
	addr = ISP_GET_REG(ISP_SCALER_CFG) + base;
	cmd = ISP_REG_RD(ctx_idx, base + ISP_SCALER_CFG);
	cmd |= (1 << 31);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_SCALER_SRC_SIZE) + base;
	cmd = (slc_scaler->src_size_x & 0x3FFF) |
			((slc_scaler->src_size_y & 0x3FFF) << 16);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_SCALER_DES_SIZE) + base;
	cmd = (slc_scaler->dst_size_x & 0x3FFF) |
			((slc_scaler->dst_size_y & 0x3FFF) << 16);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_SCALER_TRIM0_START) + base;
	cmd = (slc_scaler->trim0_start_x & 0x1FFF) |
			((slc_scaler->trim0_start_y & 0x1FFF) << 16);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_SCALER_TRIM0_SIZE) + base;
	cmd = (slc_scaler->trim0_size_x & 0x1FFF) |
			((slc_scaler->trim0_size_y & 0x1FFF) << 16);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_SCALER_IP) + base;
	cmd = (slc_scaler->scaler_ip_rmd & 0x1FFF) |
			((slc_scaler->scaler_ip_int & 0xF) << 16);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_SCALER_CIP) + base;
	cmd = (slc_scaler->scaler_cip_rmd & 0x1FFF) |
			((slc_scaler->scaler_cip_int & 0xF) << 16);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_SCALER_TRIM1_START) + base;
	cmd = (slc_scaler->trim1_start_x & 0x1FFF) |
			((slc_scaler->trim1_start_y & 0x1FFF) << 16);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_SCALER_TRIM1_SIZE) + base;
	cmd = (slc_scaler->trim1_size_x & 0x1FFF) |
			((slc_scaler->trim1_size_y & 0x1FFF) << 16);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_SCALER_VER_IP) + base;
	cmd = (slc_scaler->scaler_ip_rmd_ver & 0x1FFF) |
			((slc_scaler->scaler_ip_int_ver & 0xF) << 16);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_SCALER_VER_CIP) + base;
	cmd = (slc_scaler->scaler_cip_rmd_ver & 0x1FFF) |
			((slc_scaler->scaler_cip_int_ver & 0xF) << 16);
	FMCU_PUSH(fmcu, addr, cmd);

	return 0;
}

static int set_slice_spath_store(
		struct isp_fmcu_ctx_desc *fmcu,
		uint32_t path_en,
		uint32_t ctx_idx,
		enum isp_sub_path_id spath_id,
		struct slice_store_info *slc_store)
{
	uint32_t addr = 0, cmd = 0;
	unsigned long base = store_base[spath_id];

	if (!path_en) {
		/* bit0 bypass store */
		addr = ISP_GET_REG(ISP_STORE_PARAM) + base;
		cmd = 1;
		FMCU_PUSH(fmcu, addr, cmd);
		return 0;
	}
	addr = ISP_GET_REG(ISP_STORE_PARAM) + base;
	cmd = ISP_REG_RD(ctx_idx, base + ISP_STORE_PARAM) & ~1;
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_STORE_SLICE_SIZE) + base;
	cmd = ((slc_store->size.h & 0xFFFF) << 16) |
			(slc_store->size.w & 0xFFFF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_STORE_BORDER) + base;
	cmd = (slc_store->border.up_border & 0xFF) |
			((slc_store->border.down_border & 0xFF) << 8) |
			((slc_store->border.left_border & 0xFF) << 16) |
			((slc_store->border.right_border & 0xFF) << 24);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_STORE_SLICE_Y_ADDR) + base;
	cmd = slc_store->addr.addr_ch0;
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_STORE_SLICE_U_ADDR) + base;
	cmd = slc_store->addr.addr_ch1;
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_STORE_SLICE_V_ADDR) + base;
	cmd = slc_store->addr.addr_ch2;
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_STORE_SHADOW_CLR) + base;
	cmd = 1;
	FMCU_PUSH(fmcu, addr, cmd);

	return 0;
}

static int set_slice_3dnr_memctrl(
		struct isp_fmcu_ctx_desc *fmcu,
		struct slice_3dnr_memctrl_info *mem_ctrl)
{
	uint32_t addr = 0, cmd = 0;

	if (mem_ctrl->bypass)
		return 0;

	addr = ISP_GET_REG(ISP_3DNR_MEM_CTRL_PARAM1);
	cmd = ((mem_ctrl->start_col & 0x1FFF) << 16) |
		(mem_ctrl->start_row & 0x1FFF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_3DNR_MEM_CTRL_PARAM3);
	cmd = ((mem_ctrl->src.h & 0x1FFF) << 16) |
		(mem_ctrl->src.w & 0x1FFF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_3DNR_MEM_CTRL_PARAM4);
	cmd = ((mem_ctrl->ft_y.h & 0x1FFF) << 16) |
		(mem_ctrl->ft_y.w & 0x1FFF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_3DNR_MEM_CTRL_PARAM5);
	cmd = ((mem_ctrl->ft_uv.h & 0x1FFF) << 16) |
		(mem_ctrl->ft_uv.w & 0x1FFF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_3DNR_MEM_CTRL_FT_CUR_LUMA_ADDR);
	cmd = mem_ctrl->addr.addr_ch0;
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_3DNR_MEM_CTRL_FT_CUR_CHROMA_ADDR);
	cmd = mem_ctrl->addr.addr_ch1;
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_3DNR_MEM_CTRL_LINE_MODE);
	cmd = ((mem_ctrl->last_line_mode & 0x1) << 1) |
		(mem_ctrl->first_line_mode & 0x1);
	FMCU_PUSH(fmcu, addr, cmd);

	return 0;
}

static int set_slice_3dnr_store(
		struct isp_fmcu_ctx_desc *fmcu,
		struct slice_3dnr_store_info *store)
{
	uint32_t addr = 0, cmd = 0;

	if (store->bypass)
		return 0;

	addr = ISP_GET_REG(ISP_3DNR_STORE_SIZE);
	cmd = ((store->size.h & 0xFFFF) << 16) |
		(store->size.w & 0xFFFF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_3DNR_STORE_LUMA_ADDR);
	cmd = store->addr.addr_ch0;
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_3DNR_STORE_CHROMA_ADDR);
	cmd = store->addr.addr_ch1;
	FMCU_PUSH(fmcu, addr, cmd);

	return 0;
}

static int set_slice_3dnr_crop(
		struct isp_fmcu_ctx_desc *fmcu,
		struct slice_3dnr_crop_info *crop)
{
	uint32_t addr = 0, cmd = 0;

	if (crop->bypass)
		return 0;

	addr = ISP_GET_REG(ISP_3DNR_MEM_CTRL_PRE_PARAM0);
	cmd = crop->bypass & 0x1;
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_3DNR_MEM_CTRL_PRE_PARAM1);
	cmd = ((crop->src.h & 0xFFFF) << 16) |
		(crop->src.w & 0xFFFF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_3DNR_MEM_CTRL_PRE_PARAM2);
	cmd = ((crop->dst.h & 0xFFFF) << 16) |
		(crop->dst.w & 0xFFFF);
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_3DNR_MEM_CTRL_PRE_PARAM3);
	cmd = ((crop->start_x & 0xFFFF) << 16) |
		(crop->start_y & 0xFFFF);
	FMCU_PUSH(fmcu, addr, cmd);

	return 0;
}

static int set_slice_3dnr(
		struct isp_fmcu_ctx_desc *fmcu,
		struct isp_slice_desc *cur_slc,
		struct cam_hw_info *hw,
		struct isp_pipe_context *pctx)
{
/*
 * struct slice_3dnr_memctrl_info slice_3dnr_memctrl;
 * struct slice_3dnr_store_info   slice_3dnr_store;
 * struct slice_3dnr_crop_info    slice_3dnr_crop;
 */
	set_slice_3dnr_memctrl(fmcu, &cur_slc->slice_3dnr_memctrl);
	set_slice_3dnr_store(fmcu, &cur_slc->slice_3dnr_store);
	if (pctx->nr3_fbc_fbd) {
		hw->hw_ops.core_ops.isp_nr3_fbc_slice_set(fmcu, &cur_slc->slice_3dnr_fbc_store);
		hw->hw_ops.core_ops.isp_nr3_fbd_slice_set(fmcu, &cur_slc->slice_3dnr_fbd_fetch);
	}
	set_slice_3dnr_crop(fmcu, &cur_slc->slice_3dnr_crop);

	return 0;
}

static int set_slice_nofilter(
		struct isp_fmcu_ctx_desc *fmcu,
		struct isp_slice_desc *cur_slc)
{
	uint32_t addr = 0, cmd = 0;
	struct slice_noisefilter_info *map = &cur_slc->noisefilter_info;

	pr_debug("seed0=%d,seed1=%d,seed2=%d,seed3=%d\n", map->seed0, map->seed1,
		map->seed2, map->seed3);
	addr = ISP_GET_REG(ISP_YUV_NF_SEED0);
	cmd = map->seed0;
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_YUV_NF_SEED1);
	cmd = map->seed1;
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_YUV_NF_SEED2);
	cmd = map->seed2;
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_YUV_NF_SEED3);
	cmd = map->seed3;
	FMCU_PUSH(fmcu, addr, cmd);

	return 0;
}

int isp_set_slices_fmcu_cmds(void *fmcu_handle,  void *ctx)
{
	int i, j;
	int sw_ctx_id = 0;
	int hw_ctx_id = 0;
	enum isp_work_mode wmode;
	unsigned long base;
	struct isp_slice_desc *cur_slc;
	struct slice_store_info *slc_store;
	struct slice_afbc_store_info *slc_afbc_store;
	struct slice_ifbc_store_info *slc_ifbc_store;
	struct slice_scaler_info *slc_scaler;
	struct isp_slice_context *slc_ctx;
	struct isp_fmcu_ctx_desc *fmcu;
	struct isp_pipe_context *pctx = NULL;
	struct cam_hw_info *hw = NULL;

	uint32_t reg_off, addr = 0, cmd = 0, yrandom_mode = 0;
	uint32_t shadow_done_cmd[ISP_CONTEXT_HW_NUM] = {
		PRE0_SHADOW_DONE, CAP0_SHADOW_DONE,
		PRE1_SHADOW_DONE, CAP1_SHADOW_DONE,
	};
	uint32_t all_done_cmd[ISP_CONTEXT_HW_NUM] = {
		PRE0_ALL_DONE, CAP0_ALL_DONE,
		PRE1_ALL_DONE, CAP1_ALL_DONE,
	};

	if (!fmcu_handle || !ctx) {
		pr_err("fail to get valid input ptr, fmcu_handle %p, ctx %p\n",
			fmcu_handle, ctx);
		return -EFAULT;
	}

	pctx = (struct isp_pipe_context *)ctx;
	hw = pctx->hw;
	sw_ctx_id = pctx->ctx_id;
	hw_ctx_id = isp_get_hw_context_id(pctx);
	pr_info("get hw context id=%d\n", hw_ctx_id);
	wmode = pctx->dev->wmode;
	slc_ctx = (struct isp_slice_context *)pctx->slice_ctx;
	if (slc_ctx->slice_num < 1) {
		pr_err("fail to use slices, not support here.\n");
		return -EINVAL;
	}

	fmcu = (struct isp_fmcu_ctx_desc *)fmcu_handle;
	if (fmcu->fid == 0)
		base =  ISP_FMCU0_BASE;
	else
		base =  ISP_FMCU1_BASE;

	cur_slc = &slc_ctx->slices[0];
	yrandom_mode = cur_slc->slice_noisefilter_mode.yrandom_mode;
	for (i = 0; i < SLICE_NUM_MAX; i++, cur_slc++) {
		if (cur_slc->valid == 0)
			continue;
		if (wmode == ISP_CFG_MODE)
			set_fmcu_cfg(fmcu, hw_ctx_id);

		set_slice_nr_info(fmcu, cur_slc);

		if (!cur_slc->slice_fbd_raw.fetch_fbd_bypass)
			hw->hw_ops.core_ops.isp_fbd_slice_set(fmcu,
					&cur_slc->slice_fbd_raw);
		else
			set_slice_fetch(fmcu, &cur_slc->slice_fetch);

		set_slice_3dnr(fmcu, cur_slc, hw, pctx);
		if (pctx->ltm_rgb)
			hw->hw_ops.core_ops.isp_ltm_slice_set(fmcu, cur_slc, LTM_RGB);
		if (pctx->ltm_yuv)
			hw->hw_ops.core_ops.isp_ltm_slice_set(fmcu, cur_slc, LTM_YUV);
		if(yrandom_mode == 1)
			set_slice_nofilter(fmcu, cur_slc);

		for (j = 0; j < ISP_SPATH_NUM; j++) {
			slc_store = &cur_slc->slice_store[j];
			if (j == ISP_SPATH_FD) {
				set_slice_spath_thumbscaler(fmcu,
					cur_slc->path_en[j], sw_ctx_id,
					&cur_slc->slice_thumbscaler);
			} else {
				slc_scaler = &cur_slc->slice_scaler[j];
				set_slice_spath_scaler(fmcu,
					cur_slc->path_en[j], sw_ctx_id, j,
					slc_scaler);
			}
			set_slice_spath_store(fmcu,
				cur_slc->path_en[j], sw_ctx_id, j, slc_store);
			if (j < FBC_PATH_NUM) {
				slc_afbc_store = &cur_slc->slice_afbc_store[j];
				slc_ifbc_store = &cur_slc->slice_ifbc_store[j];
				if(slc_afbc_store->slc_afbc_on)
					hw->hw_ops.core_ops.isp_afbc_path_slice_set(
						fmcu, cur_slc->path_en[j],
						sw_ctx_id, j, slc_afbc_store);
				if(slc_ifbc_store->slice_mode_en)
					hw->hw_ops.core_ops.isp_ifbc_path_slice_set(
						fmcu, cur_slc->path_en[j],
						sw_ctx_id, j, slc_ifbc_store);
			}
		}

		if (wmode == ISP_CFG_MODE) {
			reg_off = ISP_CFG_CAP_FMCU_RDY;
			addr = ISP_GET_REG(reg_off);
		} else
			addr = ISP_GET_REG(ISP_FETCH_START);
		cmd = 1;
		FMCU_PUSH(fmcu, addr, cmd);

		addr = ISP_GET_REG(base + ISP_FMCU_CMD);
		cmd = shadow_done_cmd[hw_ctx_id];
		FMCU_PUSH(fmcu, addr, cmd);

		addr = ISP_GET_REG(base + ISP_FMCU_CMD);
		cmd = all_done_cmd[hw_ctx_id];
		FMCU_PUSH(fmcu, addr, cmd);
	}

	return 0;
}

static int update_slice_nr_info(
	uint32_t ctx_id,
	struct isp_slice_desc *cur_slc)
{
	uint32_t addr = 0, cmd = 0;

	/* NLM */
	addr = ISP_NLM_RADIAL_1D_DIST;
	cmd = ((cur_slc->slice_nlm.center_y_relative & 0x3FFF) << 16) |
		(cur_slc->slice_nlm.center_x_relative & 0x3FFF);
	ISP_REG_WR(ctx_id, addr, cmd);

	/* Post CDN */
	addr = ISP_POSTCDN_SLICE_CTRL;
	cmd = cur_slc->slice_postcdn.start_row_mod4;
	ISP_REG_WR(ctx_id, addr, cmd);

	/* YNR */
	addr = ISP_YNR_CFG31;
	cmd = ((cur_slc->slice_ynr.center_offset_y & 0xFFFF) << 16) |
		(cur_slc->slice_ynr.center_offset_x & 0xFFFF);
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_YNR_CFG33;
	cmd = ((cur_slc->slice_ynr.slice_height & 0xFFFF) << 16) |
		(cur_slc->slice_ynr.slice_width & 0xFFFF);
	ISP_REG_WR(ctx_id, addr, cmd);

	return 0;
}

static int update_slice_fetch(uint32_t ctx_id,
		struct slice_fetch_info *fetch_info)
{
	uint32_t addr = 0, cmd = 0;

	addr = ISP_FETCH_MEM_SLICE_SIZE;
	cmd = ((fetch_info->size.h & 0xFFFF) << 16) |
			(fetch_info->size.w & 0xFFFF);
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_FETCH_SLICE_Y_ADDR;
	cmd = fetch_info->addr.addr_ch0;
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_FETCH_SLICE_U_ADDR;
	cmd = fetch_info->addr.addr_ch1;
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_FETCH_SLICE_V_ADDR;
	cmd = fetch_info->addr.addr_ch2;
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_FETCH_MIPI_INFO;
	cmd = fetch_info->mipi_word_num |
			(fetch_info->mipi_byte_rel_pos << 16);
	ISP_REG_WR(ctx_id, addr, cmd);

	/* dispatch size same as fetch size */
	addr = ISP_DISPATCH_CH0_SIZE;
	cmd = ((fetch_info->size.h & 0xFFFF) << 16) |
			(fetch_info->size.w & 0xFFFF);
	ISP_REG_WR(ctx_id, addr, cmd);

	return 0;
}

static int update_slice_spath_scaler(
		uint32_t path_en,
		uint32_t ctx_id,
		enum isp_sub_path_id spath_id,
		struct slice_scaler_info *slc_scaler)
{
	uint32_t addr = 0, cmd = 0;
	uint32_t base = (uint32_t)scaler_base[spath_id];

	if (!path_en) {
		addr = ISP_SCALER_CFG + base;
		cmd = (0 << 31) | (1 << 8) | (1 << 9);
		ISP_REG_WR(ctx_id, addr, cmd);
		return 0;
	}

	/* bit31 enable path */
	addr = ISP_SCALER_CFG + base;
	cmd = ISP_REG_RD(ctx_id, base + ISP_SCALER_CFG);
	cmd |= (1 << 31);
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_SCALER_SRC_SIZE + base;
	cmd = (slc_scaler->src_size_x & 0x3FFF) |
			((slc_scaler->src_size_y & 0x3FFF) << 16);
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_SCALER_DES_SIZE + base;
	cmd = (slc_scaler->dst_size_x & 0x3FFF) |
			((slc_scaler->dst_size_y & 0x3FFF) << 16);
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_SCALER_TRIM0_START + base;
	cmd = (slc_scaler->trim0_start_x & 0x1FFF) |
			((slc_scaler->trim0_start_y & 0x1FFF) << 16);
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_SCALER_TRIM0_SIZE + base;
	cmd = (slc_scaler->trim0_size_x & 0x1FFF) |
			((slc_scaler->trim0_size_y & 0x1FFF) << 16);
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_SCALER_IP + base;
	cmd = (slc_scaler->scaler_ip_rmd & 0x1FFF) |
			((slc_scaler->scaler_ip_int & 0xF) << 16);
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_SCALER_CIP + base;
	cmd = (slc_scaler->scaler_cip_rmd & 0x1FFF) |
			((slc_scaler->scaler_cip_int & 0xF) << 16);
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_SCALER_TRIM1_START + base;
	cmd = (slc_scaler->trim1_start_x & 0x1FFF) |
			((slc_scaler->trim1_start_y & 0x1FFF) << 16);
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_SCALER_TRIM1_SIZE + base;
	cmd = (slc_scaler->trim1_size_x & 0x1FFF) |
			((slc_scaler->trim1_size_y & 0x1FFF) << 16);
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_SCALER_VER_IP + base;
	cmd = (slc_scaler->scaler_ip_rmd_ver & 0x1FFF) |
			((slc_scaler->scaler_ip_int_ver & 0xF) << 16);
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_SCALER_VER_CIP + base;
	cmd = (slc_scaler->scaler_cip_rmd_ver & 0x1FFF) |
			((slc_scaler->scaler_cip_int_ver & 0xF) << 16);
	ISP_REG_WR(ctx_id, addr, cmd);

	return 0;
}


static int update_slice_spath_store(
		uint32_t path_en,
		uint32_t ctx_id,
		enum isp_sub_path_id spath_id,
		struct slice_store_info *slc_store)
{
	uint32_t addr = 0, cmd = 0;
	uint32_t base = (uint32_t)store_base[spath_id];

	if (!path_en) {
		/* bit0 bypass store */
		addr = ISP_STORE_PARAM + base;
		cmd = 1;
		ISP_REG_WR(ctx_id, addr, cmd);
		return 0;
	}
	addr = ISP_STORE_PARAM + base;
	cmd = ISP_REG_RD(ctx_id, base + ISP_STORE_PARAM) & ~1;
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_STORE_SLICE_SIZE + base;
	cmd = ((slc_store->size.h & 0xFFFF) << 16) |
			(slc_store->size.w & 0xFFFF);
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_STORE_BORDER + base;
	cmd = (slc_store->border.up_border & 0xFF) |
			((slc_store->border.down_border & 0xFF) << 8) |
			((slc_store->border.left_border & 0xFF) << 16) |
			((slc_store->border.right_border & 0xFF) << 24);
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_STORE_SLICE_Y_ADDR + base;
	cmd = slc_store->addr.addr_ch0;
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_STORE_SLICE_U_ADDR + base;
	cmd = slc_store->addr.addr_ch1;
	ISP_REG_WR(ctx_id, addr, cmd);

	addr = ISP_STORE_SLICE_V_ADDR + base;
	cmd = slc_store->addr.addr_ch2;
	ISP_REG_WR(ctx_id, addr, cmd);

	return 0;
}

int isp_update_slice(
		void *pctx_handle,
		uint32_t ctx_id,
		uint32_t slice_id)
{
	int j;
	struct isp_slice_desc *cur_slc;
	struct slice_store_info *slc_store;
	struct slice_afbc_store_info *slc_afbc_store;
	struct slice_ifbc_store_info *slc_ifbc_store;
	struct slice_scaler_info *slc_scaler;
	struct isp_slice_context *slc_ctx;
	struct isp_pipe_context *pctx = NULL;
	struct cam_hw_info * hw = NULL;

	if (!pctx_handle) {
		pr_err("fail to get input ptr, null.\n");
		return -EFAULT;
	}

	pctx = (struct isp_pipe_context *)pctx_handle;
	hw = pctx->hw;
	slc_ctx = (struct isp_slice_context *)pctx->slice_ctx;
	if (slc_ctx->slice_num < 1) {
		pr_err("fail to use slices, not support here.\n");
		return -EINVAL;
	}

	if ((slice_id >= SLICE_NUM_MAX) ||
		(slc_ctx->slices[slice_id].valid == 0)) {
		pr_err("fail to get valid slice id %d\n", slice_id);
		return -EINVAL;
	}

	cur_slc = &slc_ctx->slices[slice_id];
	update_slice_nr_info(ctx_id, cur_slc);
	update_slice_fetch(ctx_id, &cur_slc->slice_fetch);
	for (j = 0; j < ISP_SPATH_NUM; j++) {
		slc_store = &cur_slc->slice_store[j];
		if (j != ISP_SPATH_FD) {
			slc_scaler = &cur_slc->slice_scaler[j];
			update_slice_spath_scaler(
				cur_slc->path_en[j], ctx_id, j,
				slc_scaler);
		}
		update_slice_spath_store(
			cur_slc->path_en[j], ctx_id, j, slc_store);
		if (j < FBC_PATH_NUM) {
			slc_afbc_store = &cur_slc->slice_afbc_store[j];
			slc_ifbc_store = &cur_slc->slice_ifbc_store[j];
			if(slc_afbc_store->slc_afbc_on)
				hw->hw_ops.core_ops.isp_afbc_path_slice_set(
					NULL, cur_slc->path_en[j],
					ctx_id, j, slc_afbc_store);
			if(slc_ifbc_store->slice_mode_en)
				hw->hw_ops.core_ops.isp_ifbc_path_slice_set(
					NULL, cur_slc->path_en[j],
					ctx_id, j, slc_ifbc_store);
		}
	}
	return 0;
}

int isp_set_slw_fmcu_cmds(void *fmcu_handle, struct isp_pipe_context *pctx)
{
	int i;
	unsigned long base, sbase;
	uint32_t ctx_idx;
	uint32_t reg_off, addr = 0, cmd = 0;
	struct isp_fmcu_ctx_desc *fmcu;
	struct isp_path_desc *path;
	struct img_addr *fetch_addr, *store_addr;
	struct isp_afbc_store_info *afbc_store_addr;
	struct isp_ifbc_store_info *ifbc_store_addr;
	struct cam_hw_info *hw = NULL;

	uint32_t shadow_done_cmd[ISP_CONTEXT_HW_NUM] = {
		PRE0_SHADOW_DONE, CAP0_SHADOW_DONE,
		PRE1_SHADOW_DONE, CAP1_SHADOW_DONE,
	};
	uint32_t all_done_cmd[ISP_CONTEXT_HW_NUM] = {
		PRE0_ALL_DONE, CAP0_ALL_DONE,
		PRE1_ALL_DONE, CAP1_ALL_DONE,
	};

	if (!fmcu_handle || !pctx) {
		pr_err("fail to get valid input ptr, fmcu_handle %p, pctx %p\n",
			fmcu_handle, pctx);
		return -EFAULT;
	}

	fmcu = (struct isp_fmcu_ctx_desc *)fmcu_handle;
	base = (fmcu->fid == 0) ? ISP_FMCU0_BASE : ISP_FMCU1_BASE;
	fetch_addr = &pctx->fetch.addr;
	ctx_idx = pctx->ctx_id;
	hw = pctx->hw;

	set_fmcu_cfg(fmcu, ctx_idx);

	addr = ISP_GET_REG(ISP_FETCH_SLICE_Y_ADDR);
	cmd = fetch_addr->addr_ch0;
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_FETCH_SLICE_U_ADDR);
	cmd = fetch_addr->addr_ch1;
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(ISP_FETCH_SLICE_V_ADDR);
	cmd = fetch_addr->addr_ch2;
	FMCU_PUSH(fmcu, addr, cmd);

	for (i = 0; i < ISP_SPATH_NUM; i++) {
		path = &pctx->isp_path[i];

		if (atomic_read(&path->user_cnt) < 1)
			continue;

		store_addr = &path->store.addr;
		sbase = store_base[i];
		afbc_store_addr = &path->afbc_store;
		ifbc_store_addr = &path->ifbc_store;

		addr = ISP_GET_REG(ISP_STORE_SLICE_Y_ADDR) + sbase;
		cmd = store_addr->addr_ch0;
		FMCU_PUSH(fmcu, addr, cmd);

		addr = ISP_GET_REG(ISP_STORE_SLICE_U_ADDR) + sbase;
		cmd = store_addr->addr_ch1;
		FMCU_PUSH(fmcu, addr, cmd);

		addr = ISP_GET_REG(ISP_STORE_SLICE_V_ADDR) + sbase;
		cmd = store_addr->addr_ch2;
		FMCU_PUSH(fmcu, addr, cmd);

		addr = ISP_GET_REG(ISP_STORE_SHADOW_CLR) + sbase;
		cmd = 1;
		FMCU_PUSH(fmcu, addr, cmd);

		if ((i < FBC_PATH_NUM) && (path->afbc_store.bypass == 0))
			hw->hw_ops.core_ops.isp_afbc_fmcu_addr_set(fmcu,
					afbc_store_addr, i);
		if ((i < FBC_PATH_NUM) && (path->ifbc_store.bypass == 0))
			hw->hw_ops.core_ops.isp_ifbc_fmcu_addr_set(fmcu,
					ifbc_store_addr, i);
	}

	reg_off = ISP_CFG_CAP_FMCU_RDY;
	addr = ISP_GET_REG(reg_off);
	cmd = 1;
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(base + ISP_FMCU_CMD);
	cmd = shadow_done_cmd[ctx_idx];
	FMCU_PUSH(fmcu, addr, cmd);

	addr = ISP_GET_REG(base + ISP_FMCU_CMD);
	cmd = all_done_cmd[ctx_idx];
	FMCU_PUSH(fmcu, addr, cmd);

	return 0;
}
