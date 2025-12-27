// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2025 Google LLC
 */

#include "ufs-google-platform.h"
#include "ufs-google.h"
#include "ufs-phy-fw-patch.h"
#include "ufs/ufshcd.h"

static int ufs_lga_plt_get_phy_fw_patch(struct ufs_google_host *host,
					const u32 **data, size_t *sz)
{
	*data = ufs_phy_fw_patch;
	*sz = ufs_phy_fw_patch_sz;
	return 0;
}

static int ufs_lga_plat_get_rmmi_tx_eq_attrs(struct ufs_google_host *host,
					     const struct ufs_dme_attr **attrs,
					     size_t *len)
{
	static const struct ufs_dme_attr rmmi_attrs_tx_eq[] = {
		{ UIC_ARG_MIB_SEL(MPHY_G5_TX_EQ_PRE_DE_0P0_LA, MPHY_TX_LANE1),
		  0x0, DME_LOCAL },
		{ UIC_ARG_MIB_SEL(MPHY_G5_TX_EQ_PRE_DE_0P0_LA, MPHY_TX_LANE2),
		  0x0, DME_LOCAL },
		{ UIC_ARG_MIB(UNIPRO_DME_MPHY_CFG_UPD), 0x1, DME_LOCAL },
	};

	if (ufs_should_apply_phy_patch(host)) {
		/*
		 * PDTE MPHY Setting Update: b/341119164
		 * Set value to 0 for both the lanes before phy reset release
		 */
		*attrs = rmmi_attrs_tx_eq;
		*len = ARRAY_SIZE(rmmi_attrs_tx_eq);
	} else {
		*attrs = NULL;
		*len = 0;
	}

	return 0;
}

static int ufs_lga_plat_get_rmmi_attrs(struct ufs_google_host *host,
				       const struct ufs_dme_attr **attrs,
				       size_t *len)
{
	static const struct ufs_dme_attr rmmi_attrs[] = {
		{ UIC_ARG_MIB(MPHY_G5_CBMPHYBOOTCFG_OFFSET), 0x5, DME_LOCAL },
		{ UIC_ARG_MIB(MPHY_G5_CBATTR2APBCTRL_OFFSET), 0x1, DME_LOCAL },
		{ UIC_ARG_MIB_SEL(MPHY_G5_RX_CDR_ACTIVE_LATENCY_ADJUST_OFFSET,
				  MPHY_RX_LANE1), 0x15, DME_LOCAL },
		{ UIC_ARG_MIB_SEL(MPHY_G5_RX_CDR_ACTIVE_LATENCY_ADJUST_OFFSET,
				  MPHY_RX_LANE2), 0x15, DME_LOCAL },
		{ UIC_ARG_MIB_SEL(MPHY_G5_RX_ASYNC_FILTER_OFFSET,
				  MPHY_RX_LANE1), 0x1, DME_LOCAL },
		{ UIC_ARG_MIB_SEL(MPHY_G5_RX_ASYNC_FILTER_OFFSET,
				  MPHY_RX_LANE2), 0x1, DME_LOCAL },
	};
	*attrs = rmmi_attrs;
	*len = ARRAY_SIZE(rmmi_attrs);
	return 0;
}

static int ufs_lga_plat_poll_phy_ready(struct ufs_google_host *host)
{
	int ret;
	u32 data;
	struct ufs_hba *hba = host->hba;

	ret = read_poll_timeout(ufshcd_dme_get, ret, data == 1,
				MPHY_ENABLE_DELAY_US, MPHY_ENABLE_TIMEOUT_US,
				false, hba,
				UIC_ARG_MIB_SEL(MPHY_G5_TX_FSM_STATE_OFFSET,
						0x0),
				&data);
	if (ret)
		return ret;

	ret = read_poll_timeout(ufshcd_dme_get, ret, data == 1,
				MPHY_ENABLE_DELAY_US, MPHY_ENABLE_TIMEOUT_US,
				false, hba,
				UIC_ARG_MIB_SEL(MPHY_G5_TX_FSM_STATE_OFFSET,
						0x1),
				&data);
	if (ret)
		return ret;

	ret = read_poll_timeout(ufshcd_dme_get, ret, data & 0x7,
				MPHY_ENABLE_DELAY_US, MPHY_ENABLE_TIMEOUT_US,
				false, hba,
				UIC_ARG_MIB_SEL(MPHY_G5_RX_FSM_STATE_OFFSET,
						0x4),
				&data);
	if (ret)
		return ret;

	ret = read_poll_timeout(ufshcd_dme_get, ret, data & 0x7,
				MPHY_ENABLE_DELAY_US, MPHY_ENABLE_TIMEOUT_US,
				false, hba,
				UIC_ARG_MIB_SEL(MPHY_G5_RX_FSM_STATE_OFFSET,
						0x5),
				&data);

	return ret;
}

static struct ufs_google_ops lga_gops = {
	.get_phy_fw_patch = ufs_lga_plt_get_phy_fw_patch,
	.get_phy_rmmi_tx_eq_attrs = ufs_lga_plat_get_rmmi_tx_eq_attrs,
	.get_phy_rmmi_attrs = ufs_lga_plat_get_rmmi_attrs,
	.poll_phy_ready = ufs_lga_plat_poll_phy_ready,
};

int ufs_google_plat_set_gops(struct ufs_google_host *host)
{
	host->gops = &lga_gops;
	return 0;
}

const struct reg_info hc_registers[] = {
	/* UFSHCI standard registers */
	REG_INFO(REG_IS),
	REG_INFO(REG_CCAP),
	REG_INFO(REG_CONFIG),
	REG_INFO(REG_MCQCONFIG),
	/* non-standard registers */
	REG_INFO(REG_BUSTHRTL),
	REG_INFO(REG_UFSHC_VER_ID_REG),
	REG_INFO(REG_UFSHC_VER_TYPE_REG),
	REG_INFO(REG_VS_OOO_CONFIG),
	REG_INFO(REG_VS_IAG),
	REG_INFO(REG_VS_CQES),
	REG_INFO(REG_HCLKDIV),
	REG_INFO(REG_EXTND_VENDOR_REG_POINTER),
	REG_INFO(REG_CRYPTOCAP_0),
	REG_INFO(REG_CRYPTOCAP_1),
	REG_INFO(REG_UFS_SFTY_MECHANISM_MONITOR_EN_REG),
	{} /* Sentinel element */
};

/*
 * TOP registers
 *
 * The TOP registers are control and status registers (CSRs) for the UFS
 * subsystem within the SSWRP_HSIO_S IP block. They are used to store
 * ufs_monitor data and to control the UFS IPs. These registers are not
 * part of the UFSHC.
 */
const struct reg_info top_registers[] = {
	REG_INFO(REG_HSIOS_UFS_VERSION),
	REG_INFO(REG_HSIOS_UFS_CARD_DET),
	REG_INFO(REG_HSIOS_UFS_ENC_STAT),
	REG_INFO(REG_HSIOS_UFS_MONITOR_1),
	REG_INFO(REG_HSIOS_UFS_MONITOR_2),
	REG_INFO(REG_HSIOS_UFS_MONITOR_3),
	REG_INFO(REG_HSIOS_UFS_MONITOR_4),
	REG_INFO(REG_HSIOS_UFS_UNIPRO_MON_1),
	REG_INFO(REG_HSIOS_UFS_UNIPRO_MON_2),
	REG_INFO(REG_HSIOS_UFS_UNIPRO_MON_3),
	REG_INFO(REG_HSIOS_UFS_MPHY_DTB),
	REG_INFO(REG_HSIOS_UFS_PWR_CTRL),
	REG_INFO(REG_HSIOS_UFS_REXT_CTRL),
	REG_INFO(REG_HSIOS_UFS_AWUSER_VC),
	REG_INFO(REG_HSIOS_UFS_QOS),
	REG_INFO(REG_HSIOS_UFS_PHYCLK_SEL),
	REG_INFO(REG_HSIOS_UFS_CFG_CLKSEL),
	REG_INFO(REG_HSIOS_UFS_MPHY_CFG),
	REG_INFO(REG_HSIOS_UFS_SRAM_APB_MODE),
	{} /* Sentinel element */
};

const struct reg_info ufs_refclk_registers[] = {
	REG_INFO(REG_XHSIOS_UFS_REFCLK_PARAM),
	REG_INFO(REG_XHSIOS_UFS_REFCLK_DMUXSEL),
	REG_INFO(REG_XHSIOS_UFS_REFCLK_TXDATA),
	{} /* Sentinel element */
};

const struct reg_info ufs_resetb_registers[] = {
	REG_INFO(REG_XHSIOS_UFS_RESETB_PARAM),
	REG_INFO(REG_XHSIOS_UFS_RESETB_DMUXSEL),
	REG_INFO(REG_XHSIOS_UFS_RESETB_TXDATA),
	{} /* Sentinel element */
};

const struct reg_info ufs_pwr_en_registers[] = {
	REG_INFO(REG_XHSIOS_UFS_PWR_EN_PARAM),
	REG_INFO(REG_XHSIOS_UFS_PWR_EN_DMUXSEL),
	REG_INFO(REG_XHSIOS_UFS_PWR_EN_TXDATA),
	{} /* Sentinel element */
};
