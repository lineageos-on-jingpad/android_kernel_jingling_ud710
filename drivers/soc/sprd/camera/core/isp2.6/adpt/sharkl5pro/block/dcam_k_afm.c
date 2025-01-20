/*
 * Copyright(C) 2017-2018 Spreadtrum Communications	Inc.
 *
 * This	software is licensed under the terms of	the GNU	General	Public
 * License version 2, as published by the Free Software	Foundation, and
 * may be copied, distributed, and modified under those	terms.
 *
 * This	program	is distributed in the hope that	it will	be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the
 * GNU General Public License for more details.
 */

#include <linux/uaccess.h>
#include <sprd_mm.h>

#include "sprd_isp_hw.h"
#include "dcam_reg.h"
#include "dcam_path.h"

#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) "AFM: %d %d %s : "\
	fmt, current->pid, __LINE__, __func__

enum {
	_UPDATE_BYPASS = BIT(0),
	_UPDATE_INFO = BIT(1),
	_UPDATE_WIN = BIT(2),
	_UPDATE_WIN_NUM = BIT(3),
	_UPDATE_MODE = BIT(4),
	_UPDATE_SKIP = BIT(5),
	_UPDATE_CROP_EB = BIT(6),
	_UPDATE_CROP_SIZE = BIT(7),
	_UPDATE_TILE = BIT(8),
};

int dcam_k_afm_block(struct dcam_dev_param *param)
{
	int ret = 0;
	uint32_t idx = param->idx;
	int i = 0;
	uint32_t val = 0;
	struct dcam_dev_afm_info *p; /* af_param; */

	if (param == NULL)
		return -1;
	/* update ? */
	if (!(param->afm.update & _UPDATE_INFO))
		return 0;
	param->afm.update &= (~(_UPDATE_INFO));

	p = &(param->afm.af_param);

#if 0 // afm block just set NR parameters. AFM control should be set by algo
	if (g_dcam_bypass[idx] & (1 << _E_AFM))
		p->bypass = 1;
	val = (p->bypass & 0x1) |
		((p->afm_mode_sel & 0x1) << 2) |
		((p->afm_mul_enable & 0x1) << 3) |
		((p->afm_skip_num & 0x7) << 4);
	DCAM_REG_MWR(idx, ISP_AFM_FRM_CTRL, 0x7C, val);
	/* TODO bad to set same register in different locations */
	dcam_path_set_skip_num(param->dev, DCAM_PATH_AFM,
			       param->afm.af_param.afm_skip_num);
	if (p->bypass)
		return 0;

	/* 0 - single mode , trigger afm_sgl_start */
	if (p->afm_mode_sel == 0)
		DCAM_REG_MWR(idx, ISP_AFM_FRM_CTRL1, BIT_0, 1);

	/* afm_skip_num_clr */
	DCAM_REG_MWR(idx, ISP_AFM_FRM_CTRL1, BIT_1, 1 << 1);
#endif
	val = (p->afm_cg_dis & 0x1) |
		((p->afm_iir_enable & 0x1) << 2) |
		((p->afm_lum_stat_chn_sel & 0x3) << 4) |
		((p->afm_done_tile_num_y & 0xF) << 8) |
		((p->afm_done_tile_num_x & 0x1F) << 12);
	DCAM_REG_MWR(idx, ISP_AFM_PARAMETERS, 0x1FF35, val);

	/* IIR cfg */
	val = ((p->afm_iir_g1 & 0xFFF) << 16) |
		(p->afm_iir_g0 & 0xFFF);
	DCAM_REG_WR(idx, ISP_AFM_IIR_FILTER0, val);

	for (i = 0; i < 10; i += 2) {
		val = ((p->afm_iir_c[i + 1] & 0xFFF) << 16) |
			(p->afm_iir_c[i] & 0xFFF);
		DCAM_REG_WR(idx, ISP_AFM_IIR_FILTER1 + 4 * (i >> 1), val);
	}

	/* enhance control cfg */
	val = (p->afm_channel_sel & 0x3) |
		((p->afm_denoise_mode & 0x3) << 2) |
		((p->afm_center_weight & 0x3) << 4) |
		((p->afm_clip_en0 & 0x1) << 6) |
		((p->afm_clip_en1 & 0x1) << 7) |
		((p->afm_fv0_shift & 0x7) << 8) |
		((p->afm_fv1_shift & 0x7) << 12);
	DCAM_REG_WR(idx, ISP_AFM_ENHANCE_CTRL, val);

	val = (p->afm_fv0_th.min & 0xFFF) |
		((p->afm_fv0_th.max & 0xFFFFF) << 12);
	DCAM_REG_WR(idx, ISP_AFM_ENHANCE_FV0_THD, val);

	val = (p->afm_fv1_th.min & 0xFFF) |
		((p->afm_fv1_th.max & 0xFFFFF) << 12);
	DCAM_REG_WR(idx, ISP_AFM_ENHANCE_FV1_THD, val);

	for (i = 0; i < 4; i++) {
		val = ((p->afm_fv1_coeff[i][4] & 0x3F) << 24) |
	((p->afm_fv1_coeff[i][3] & 0x3F) << 18) |
	((p->afm_fv1_coeff[i][2] & 0x3F) << 12) |
	((p->afm_fv1_coeff[i][1] & 0x3F) << 6) |
	(p->afm_fv1_coeff[i][0] & 0x3F);
		DCAM_REG_WR(idx, ISP_AFM_ENHANCE_FV1_COEFF00 + 8 * i, val);

		val = ((p->afm_fv1_coeff[i][8] & 0x3F) << 18) |
	((p->afm_fv1_coeff[i][7] & 0x3F) << 12) |
	((p->afm_fv1_coeff[i][6] & 0x3F) << 6) |
	(p->afm_fv1_coeff[i][5] & 0x3F);
		DCAM_REG_WR(idx, ISP_AFM_ENHANCE_FV1_COEFF01 + 8 * i, val);
	}

	return ret;
}

int dcam_k_afm_bypass(struct dcam_dev_param *param)
{
	int ret = 0;
	uint32_t idx = param->idx;

	if (param == NULL)
		return -1;
	/* update ? */
	if (!(param->afm.update & _UPDATE_BYPASS))
		return 0;
	param->afm.update &= (~(_UPDATE_BYPASS));
	DCAM_REG_MWR(idx, ISP_AFM_FRM_CTRL, BIT_0, param->afm.bypass);

	return ret;
}

int dcam_k_afm_win(struct dcam_dev_param *param)
{
	int ret = 0;
	uint32_t idx = param->idx;
	struct isp_img_rect *p; /* win; */

	if (param == NULL)
		return -1;
	/* update ? */
	if (!(param->afm.update & _UPDATE_WIN))
		return 0;
	param->afm.update &= (~(_UPDATE_WIN));

	p = &(param->afm.win);
	DCAM_REG_WR(idx, ISP_AFM_WIN_RANGE0S,
			(p->y & 0x1FFF) << 16 | (p->x & 0x1FFF));

	DCAM_REG_WR(idx, ISP_AFM_WIN_RANGE0E,
			(p->h & 0x7FF) << 16 | (p->w & 0X7FF));

	return ret;
}

int dcam_k_afm_win_num(struct dcam_dev_param *param)
{
	int ret = 0;
	uint32_t idx = param->idx;
	struct isp_img_size *p; /* win_num; */

	if (param == NULL)
		return -1;
	/* update ? */
	if (!(param->afm.update & _UPDATE_WIN_NUM))
		return 0;
	param->afm.update &= (~(_UPDATE_WIN_NUM));

	p = &(param->afm.win_num);

	DCAM_REG_WR(idx, ISP_AFM_WIN_RANGE1S,
			(p->height & 0xF) << 16 | (p->width & 0x1F));

	return ret;
}

int dcam_k_afm_mode(struct dcam_dev_param *param)
{
	int ret = 0;
	uint32_t idx = param->idx;
	uint32_t mode = 0;

	if (param == NULL)
		return -1;
	/* update ? */
	if (!(param->afm.update & _UPDATE_MODE))
		return 0;
	param->afm.update &= (~(_UPDATE_MODE));

	mode = param->afm.mode;
	DCAM_REG_MWR(idx, ISP_AFM_FRM_CTRL,
		BIT_2, mode << 2);

	/* 0 - single mode , trigger afm_sgl_start */
	/* 1 - multi mode, trigger mul mode enable */
	if (mode == 0)
		DCAM_REG_MWR(idx, ISP_AFM_FRM_CTRL1, BIT_0, 1);
	else
		DCAM_REG_MWR(idx, ISP_AFM_FRM_CTRL,
			BIT_3, 1 << 3);

	return ret;
}

int dcam_k_afm_skipnum(struct dcam_dev_param *param)
{
	int ret = 0;
	uint32_t idx = param->idx;
	uint32_t skip_num = 0;

	if (param == NULL)
		return -1;
	/* update ? */
	if (!(param->afm.update & _UPDATE_SKIP))
		return 0;
	param->afm.update &= (~(_UPDATE_SKIP));

	skip_num = param->afm.skip_num;
	DCAM_REG_MWR(idx, ISP_AFM_FRM_CTRL, 0xF0, (skip_num & 0xF) << 4);

	/* afm_skip_num_clr */
	DCAM_REG_MWR(idx, ISP_AFM_FRM_CTRL1, BIT_1, 1 << 1);

	dcam_path_set_skip_num(param->dev, DCAM_PATH_AFM, param->afm.skip_num);

	return ret;
}

int dcam_k_afm_crop_eb(struct dcam_dev_param *param)
{
	int ret = 0;
	uint32_t idx = param->idx;
	uint32_t crop_eb = 0;

	if (param == NULL)
		return -1;
	/* update ? */
	if (!(param->afm.update & _UPDATE_CROP_EB))
		return 0;
	param->afm.update &= (~(_UPDATE_CROP_EB));

	crop_eb = param->afm.crop_eb;
	DCAM_REG_MWR(idx, ISP_AFM_PARAMETERS, BIT_1, crop_eb << 1);

	return ret;
}

static int dcam_afm_lbuf_share_mode(enum dcam_id idx, uint32_t width)
{
	int i = 0;
	int ret = 0;
	uint32_t line_buf = 0;
	uint32_t tb_w[] = {
	/*     dcam0, dcam1 dcam2*/
		6080, 3264, 0,
		4672, 4672, 0,
		3264, 6080, 0,
		3080, 3040, 3264,
	};

	line_buf = (DCAM_AXIM_RD(DCAM_LBUF_SHARE_MODE) >> 4) & 0x3;

	switch (idx) {
	case DCAM_ID_0:
		if (width > tb_w[0])
			pr_err("fail to check param, unsupprot roi size\n");
		else if (width <= tb_w[line_buf * 3])
			pr_debug("no need to update afm line buf\n");
		else {
			for (i = 3; i >= 0; i--) {
				if (width <= tb_w[i * 3])
					break;
			}

			pr_debug("idx[%d] width[%d], line_buf = %d i = %d\n", idx, width, line_buf, i);
			DCAM_AXIM_MWR(DCAM_LBUF_SHARE_MODE, 0x3 << 4, i << 4);
		}
		break;
	case DCAM_ID_1:
		if (width > tb_w[7])
			pr_err("fail to check param, unsupprot roi size\n");
		else if(width <= tb_w[line_buf * 3 + 1])
			pr_debug("no need to update afm line buf\n");
		else {
			{for (i = 0; i <= 3; i++)
				if (width <= tb_w[i * 3 + 1])
					break;
			}
			pr_debug("idx[%d] width[%d], line_buf = %d i = %d\n", idx, width, line_buf, i);
			DCAM_AXIM_MWR(DCAM_LBUF_SHARE_MODE, 0x3 << 4, i << 4);
		}
		break;
	case DCAM_ID_2:
		if (width > tb_w[11])
			pr_err("fail to check param, unsupprot roi size\n");
		else if(width <= tb_w[line_buf * 3 + 2])
			pr_debug("no need to update afm line buf\n");
		else {
			pr_debug("idx[%d] width[%d], line_buf = %d\n", idx, width, line_buf);
			DCAM_AXIM_MWR(DCAM_LBUF_SHARE_MODE, 0x3 << 4, 3 << 4);
		}
		break;
	default:
		pr_err("fail to get valid dcam id %d\n", idx);
		ret = 1;
		break;
	}

	pr_debug("afm_line_buf_share = 0x%x\n", DCAM_AXIM_RD(DCAM_LBUF_SHARE_MODE));
	return ret;
}

int dcam_k_afm_crop_size(struct dcam_dev_param *param)
{
	int ret = 0;
	uint32_t idx = param->idx;
	struct isp_img_rect crop_size;

	if (param == NULL)
		return -1;
	/* update ? */
	if (!(param->afm.update & _UPDATE_CROP_SIZE))
		return 0;
	param->afm.update &= (~(_UPDATE_CROP_SIZE));

	crop_size = param->afm.crop_size;

	DCAM_REG_WR(idx, ISP_AFM_CROP_START,
		(crop_size.y & 0x1FFF) << 16 |
		(crop_size.x & 0X1FFF));
	DCAM_REG_WR(idx, ISP_AFM_CROP_SIZE,
		(crop_size.h & 0x1FFF) << 16 |
		(crop_size.w & 0X1FFF));

	ret = dcam_afm_lbuf_share_mode(idx, crop_size.w);
	if (ret)
		pr_err("fail to set afm line buf share mode\n");

	return ret;
}

int dcam_k_afm_done_tilenum(struct dcam_dev_param *param)
{
	int ret = 0;
	uint32_t idx = param->idx;
	uint32_t val = 0;
	struct isp_img_size done_tile_num;

	if (param == NULL)
		return -1;
	/* update ? */
	if (!(param->afm.update & _UPDATE_TILE))
		return 0;
	param->afm.update &= (~(_UPDATE_TILE));

	done_tile_num = param->afm.done_tile_num;

	val = ((done_tile_num.width & 0x1F) << 12) |
		((done_tile_num.height & 0x0F) << 8);
	DCAM_REG_MWR(idx, ISP_AFM_PARAMETERS, 0x1FF00, val);

	return ret;
}

int dcam_k_cfg_afm(struct isp_io_param *param, struct dcam_dev_param *p)
{
	int ret = 0;
	void *pcpy;
	int size;
	int32_t bit_update;
	FUNC_DCAM_PARAM sub_func = NULL;

	switch (param->property) {
	case DCAM_PRO_AFM_BYPASS:
		pcpy = (void *)&(p->afm.bypass);
		size = sizeof(p->afm.bypass);
		bit_update = _UPDATE_BYPASS;
		sub_func = dcam_k_afm_bypass;
		break;
	case DCAM_PRO_AFM_BLOCK:
		pcpy = (void *)&(p->afm.af_param);
		size = sizeof(p->afm.af_param);
		bit_update = _UPDATE_INFO;
		sub_func = dcam_k_afm_block;
		break;
	case DCAM_PRO_AFM_WIN:
		pcpy = (void *)&(p->afm.win);
		size = sizeof(p->afm.win);
		bit_update = _UPDATE_WIN;
		sub_func = dcam_k_afm_win;
		break;
	case DCAM_PRO_AFM_WIN_NUM:
		pcpy = (void *)&(p->afm.win_num);
		size = sizeof(p->afm.win_num);
		bit_update = _UPDATE_WIN_NUM;
		sub_func = dcam_k_afm_win_num;
		break;
	case DCAM_PRO_AFM_MODE:
		pcpy = (void *)&(p->afm.mode);
		size = sizeof(p->afm.mode);
		bit_update = _UPDATE_MODE;
		sub_func = dcam_k_afm_mode;
		break;
	case DCAM_PRO_AFM_SKIPNUM:
		pcpy = (void *)&(p->afm.skip_num);
		size = sizeof(p->afm.skip_num);
		bit_update = _UPDATE_SKIP;
		sub_func = dcam_k_afm_skipnum;
		break;
	case DCAM_PRO_AFM_CROP_EB:
		pcpy = (void *)&(p->afm.crop_eb);
		size = sizeof(p->afm.crop_eb);
		bit_update = _UPDATE_CROP_EB;
		sub_func = dcam_k_afm_crop_eb;
		break;
	case DCAM_PRO_AFM_CROP_SIZE:
		pcpy = (void *)&(p->afm.crop_size);
		size = sizeof(p->afm.crop_size);
		bit_update = _UPDATE_CROP_SIZE;
		sub_func = dcam_k_afm_crop_size;
		break;
	case DCAM_PRO_AFM_DONE_TILENUM:
		pcpy = (void *)&(p->afm.done_tile_num);
		size = sizeof(p->afm.done_tile_num);
		bit_update = _UPDATE_TILE;
		sub_func = dcam_k_afm_done_tilenum;
		break;
	default:
		pr_err("fail to support property %d\n",
			param->property);
		return -EINVAL;
	}
	if (DCAM_ONLINE_MODE) {
		ret = copy_from_user(pcpy, param->property_param, size);
		if (ret) {
			pr_err("fail to copy from user ret=0x%x\n",
				(unsigned int)ret);
			return -EPERM;
		}
		p->afm.update |= bit_update;
		ret = sub_func(p);
	} else {
		mutex_lock(&p->param_lock);
		ret = copy_from_user(pcpy, param->property_param, size);
		if (ret) {
			mutex_unlock(&p->param_lock);
			pr_err("fail to copy from user ret=0x%x\n",
				(unsigned int)ret);
			return -EPERM;
		}
		p->afm.update |= bit_update;
		mutex_unlock(&p->param_lock);
	}

	return ret;
}
