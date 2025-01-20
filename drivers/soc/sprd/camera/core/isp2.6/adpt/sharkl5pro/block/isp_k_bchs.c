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

#include <linux/uaccess.h>
#include <sprd_mm.h>
#include "sprd_isp_hw.h"

#include "isp_reg.h"
#include "cam_types.h"
#include "cam_block.h"

#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) "BCHS: %d %d %s : "\
	fmt, current->pid, __LINE__, __func__

static int isp_k_bchs_block(struct isp_io_param *param, uint32_t idx)
{
	int ret = 0;
	struct isp_dev_bchs_info bchs_info;

	ret = copy_from_user((void *)&bchs_info,
		param->property_param, sizeof(bchs_info));
	if (ret != 0) {
		pr_err("fail to copy from user, ret = %d\n", ret);
		return -1;
	}
	if (g_isp_bypass[idx] & (1 << _EISP_BCHS))
		bchs_info.bchs_bypass = 1;
	ISP_REG_MWR(idx, ISP_BCHS_PARAM, BIT_0, bchs_info.bchs_bypass);
	if (bchs_info.bchs_bypass)
		return 0;

	ISP_REG_MWR(idx, ISP_BCHS_PARAM, 0xffffffff,
		(bchs_info.cnta_en << 4) |
		(bchs_info.brta_en << 3) |
		(bchs_info.hua_en << 2) |
		(bchs_info.csa_en << 1) |
		(bchs_info.bchs_bypass << 0));

	ISP_REG_MWR(idx, ISP_CSA_FACTOR, 0xffffffff,
		(bchs_info.csa_factor_u << 8) |
		(bchs_info.csa_factor_v << 0));

	ISP_REG_MWR(idx, ISP_HUA_FACTOR, 0xffffffff,
		((bchs_info.hua_cos_value & 0x1ff) << 16) |
		((bchs_info.hua_sina_value & 0x1ff) << 0));

	ISP_REG_MWR(idx, ISP_BRTA_FACTOR, 0xffffffff,
		(bchs_info.brta_factor << 0));

	ISP_REG_MWR(idx, ISP_CNTA_FACTOR, 0xffffffff,
		(bchs_info.cnta_factor << 0));

	return ret;
}

static int isp_k_brightness_block(struct isp_io_param *param, uint32_t idx)
{
	int ret = 0;
	uint32_t bchs_bypass = 0;
	uint32_t bright_en;
	struct isp_dev_brightness_info brightness_info;

	ret = copy_from_user((void *)&brightness_info,
			param->property_param,
			sizeof(brightness_info));
	if (ret != 0) {
		pr_err("fail to copy from user, ret = %d\n", ret);
		return -EPERM;
	}

	bright_en = !brightness_info.bypass;
	ISP_REG_MWR(idx, ISP_BCHS_PARAM, BIT_3, bright_en << 3);
	if (bright_en == 0) {
		pr_debug("brightness is bypass.");
		return 0;
	}
	ISP_REG_MWR(idx, ISP_BCHS_PARAM, BIT_0, bchs_bypass);
	ISP_REG_WR(idx, ISP_BRTA_FACTOR, brightness_info.factor);
	return ret;
}

static int isp_k_contrast_block(struct isp_io_param *param, uint32_t idx)
{
	int ret = 0;
	uint32_t bchs_bypass = 0;
	uint32_t ctra_en;
	struct isp_dev_contrast_info contrast_info;

	ret = copy_from_user((void *)&contrast_info,
			param->property_param,
			sizeof(contrast_info));
	if (ret != 0) {
		pr_err("fail to copy from user, ret = %d\n", ret);
		return -EPERM;
	}

	ctra_en = !contrast_info.bypass;
	ISP_REG_MWR(idx, ISP_BCHS_PARAM, BIT_4, ctra_en << 4);
	if (ctra_en == 0) {
		pr_debug("contrast is bypass.");
		return 0;
	}
	ISP_REG_MWR(idx, ISP_BCHS_PARAM, BIT_0, bchs_bypass);
	ISP_REG_WR(idx, ISP_CNTA_FACTOR, contrast_info.factor);
	return ret;
}

static int isp_k_satuation_block(struct isp_io_param *param, uint32_t idx)
{
	int ret = 0;
	uint32_t bchs_bypass = 0;
	uint32_t csa_en;
	struct isp_dev_csa_info csa_info;

	ret = copy_from_user((void *)&csa_info,
			param->property_param,
			sizeof(csa_info));
	if (ret != 0) {
		pr_err("fail to copy from user, ret = %d\n", ret);
		return -EPERM;
	}

	csa_en = !csa_info.bypass;
	ISP_REG_MWR(idx, ISP_BCHS_PARAM, BIT_1, csa_en << 1);
	if (csa_en == 0) {
		pr_debug("satucation is bypass.");
		return 0;
	}
	ISP_REG_MWR(idx, ISP_BCHS_PARAM, BIT_0, bchs_bypass);
	ISP_REG_MWR(idx, ISP_CNTA_FACTOR, 0xffff,
		(csa_info.csa_factor_u << 8) | csa_info.csa_factor_v);

	return ret;
}

static int isp_k_hue_block(struct isp_io_param *param, uint32_t idx)
{
	int ret = 0;
	uint32_t bchs_bypass = 0;
	uint32_t hue_en;
	struct isp_dev_hue_info hue_info;

	ret = copy_from_user((void *)&hue_info,
			param->property_param,
			sizeof(hue_info));
	if (ret != 0) {
		pr_err("fail to copy from user, ret = %d\n", ret);
		return -EPERM;
	}

	hue_en = !hue_info.bypass;
	ISP_REG_MWR(idx, ISP_BCHS_PARAM, BIT_2, hue_en << 2);
	if (hue_en == 0) {
		pr_debug("satucation is bypass.");
		return 0;
	}
	ISP_REG_MWR(idx, ISP_BCHS_PARAM, BIT_0, bchs_bypass);
	ISP_REG_MWR(idx, ISP_HUA_FACTOR, 0x1ff01ff,
		((hue_info.hua_cos_value & 0x1ff) << 16) |
		((hue_info.hua_sin_value & 0x1ff) << 0));

	return ret;
}

int isp_k_cfg_bchs(struct isp_io_param *param, uint32_t idx)
{
	int ret = 0;

	switch (param->property) {
	case ISP_PRO_BCHS_BLOCK:
		ret = isp_k_bchs_block(param, idx);
		break;
	case ISP_PRO_BCHS_BRIGHT:
		ret = isp_k_brightness_block(param, idx);
		break;
	case ISP_PRO_BCHS_CONTRAST:
		ret = isp_k_contrast_block(param, idx);
		break;
	case ISP_PRO_BCHS_SATUATION:
		ret = isp_k_satuation_block(param, idx);
		break;
	case ISP_PRO_BCHS_HUE:
		ret = isp_k_hue_block(param, idx);
		break;
	default:
		pr_err("fail to support cmd id = %d\n",
				param->property);
		break;
	}

	return ret;
}
