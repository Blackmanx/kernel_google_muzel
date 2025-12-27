// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Google LLC
 */

#include <linux/debugfs.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gfp_types.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <trace/hooks/ufshcd.h>
#include <ufs/ufshcd.h>

#include "../pinctrl/google/common.h"
#include "core/ufshcd-priv.h"
#include "ufs-google-dbg.h"
#include "ufs-google-platform.h"
#include "ufs-google.h"

static bool dump_registers_upon_error = true;
module_param(dump_registers_upon_error, bool, 0444);
MODULE_PARM_DESC(
	dump_registers_upon_error,
	"enable/disable printing of the debug dump (registers and voltages)");

enum register_dump_status {
	REGISTER_DUMP_VALID,
	REGISTER_DUMP_INVALID,
};

struct ufs_google_dbg {
	struct ufs_google_host *host;
	struct dentry *debugfs;

	void __iomem *psm_status_hsios_mmio;
	void __iomem *psm_status_ufs_hc_mmio;
	void __iomem *psm_status_ufs_phy_mmio;
	void __iomem *hsios_stby_bk_mmio;
	void __iomem *hsios_bk_mmio;

	struct work_struct register_dump_work;
	/*
	 * The following members are protected by register_snapshot_lock
	 * to synchronize the dump request and the worker thread.
	 */
	spinlock_t register_snapshot_lock;
	enum register_dump_status reg_dump_status;
	u32 *regs_snapshot_values;
	size_t snapshot_reg_count;
	int vcc_uv_snapshot;
	int vccq_uv_snapshot;
};

enum reg_base {
	BASE_UFS_HC,
	BASE_UFS_TOP,
	BASE_CLK_MUX,
	BASE_CLK_DIV,
	BASE_PSM_STATUS_HSIOS,
	BASE_PSM_STATUS_UFS_HC,
	BASE_PSM_STATUS_UFS_PHY,
	BASE_UFS_RESETB,
	BASE_UFS_PWR_EN,
	BASE_UFS_REFCLK,
	BASE_LEN,

	BASE_CUSTOM, // Customized dump range
	BASE_INVALID
};

struct reg_group {
	const char *name;
	enum reg_base base;
	const struct reg_info *reg_list;
};

/*
 * Since we are only interested in PSM status instead of entire PSM
 * register, mmio is only allowed to access the filed of PSM status.
 */
enum psm_status_offset {
	PSM_STATUS = 0x0,
};

/* clang-format off */
static const struct reg_info psm_status_layout[] = {
	REG_INFO(PSM_STATUS),
	{}
};
/* clang-format on */

static void *__iomem base_to_addr(struct ufs_google_host *host,
				  enum reg_base base)
{
	struct ufs_google_dbg *dbg = host->dbg;

	switch (base) {
	case BASE_UFS_HC:
		return host->hba->mmio_base;
	case BASE_UFS_TOP:
		return host->ufs_top_mmio;
	case BASE_PSM_STATUS_HSIOS:
		return dbg->psm_status_hsios_mmio;
	case BASE_PSM_STATUS_UFS_HC:
		return dbg->psm_status_ufs_hc_mmio;
	case BASE_PSM_STATUS_UFS_PHY:
		return dbg->psm_status_ufs_phy_mmio;
	case BASE_UFS_RESETB:
		return dbg->hsios_bk_mmio;
	case BASE_UFS_PWR_EN:
		return dbg->hsios_bk_mmio;
	case BASE_UFS_REFCLK:
		return dbg->hsios_stby_bk_mmio;
	/* TODO: add support to dump clk setting (mux / div) */
	default:
		return ERR_PTR(EINVAL);
	}
}

static struct reg_group ufs_reg_groups[] = {
	{
		.name = "psm_status_hsios",
		.base = BASE_PSM_STATUS_HSIOS,
		.reg_list = psm_status_layout,
	},
	{
		.name = "psm_status_ufs_hc",
		.base = BASE_PSM_STATUS_UFS_HC,
		.reg_list = psm_status_layout,
	},
	{
		.name = "psm_status_ufs_phy",
		.base = BASE_PSM_STATUS_UFS_PHY,
		.reg_list = psm_status_layout,
	},
	{
		.name = "ufs_hc",
		.base = BASE_UFS_HC,
		.reg_list = hc_registers,
	},
	{
		.name = "ufs_top",
		.base = BASE_UFS_TOP,
		.reg_list = top_registers,
	},
	{
		.name = "ufs_resetb",
		.base = BASE_UFS_RESETB,
		.reg_list = ufs_resetb_registers,
	},
	{
		.name = "ufs_pwr_en",
		.base = BASE_UFS_PWR_EN,
		.reg_list = ufs_pwr_en_registers,
	},
	{
		.name = "ufs_refclk",
		.base = BASE_UFS_REFCLK,
		.reg_list = ufs_refclk_registers,
	},
	{} /* Sentinel element */
};

static void ufs_google_capture_vregs_snapshot(struct ufs_google_dbg *dbg)
{
	struct ufs_vreg_info *info = &dbg->host->hba->vreg_info;

	if (info->vcc && info->vcc->reg)
		dbg->vcc_uv_snapshot = regulator_get_voltage(info->vcc->reg);

	if (info->vccq && info->vccq->reg)
		dbg->vccq_uv_snapshot = regulator_get_voltage(info->vccq->reg);
}

static void ufs_google_print_vregs_snapshot(struct ufs_google_dbg *dbg)
{
	struct ufs_vreg_info *info = &dbg->host->hba->vreg_info;
	struct device *dev = dbg->host->hba->dev;

	dev_err(dev, "==Regulator Voltages==\n");

	if (info->vcc && info->vcc->reg) {
		dev_err(dev, "%-5s: ao=%s, en=%s, volt=%d(uV)\n",
			info->vcc->name, str_yes_no(info->vcc->always_on),
			str_yes_no(info->vcc->enabled), dbg->vcc_uv_snapshot);
	}

	if (info->vccq && info->vccq->reg) {
		dev_err(dev, "%-5s: ao=%s, en=%s, volt=%d(uV)\n",
			info->vccq->name, str_yes_no(info->vccq->always_on),
			str_yes_no(info->vccq->enabled), dbg->vccq_uv_snapshot);
	}
}

static size_t ufs_google_count_regs(void)
{
	struct reg_group *group;
	size_t count = 0;

	for (group = ufs_reg_groups; group->name; group++) {
		const struct reg_info *reg;

		if (!group->reg_list)
			continue;
		for (reg = group->reg_list; reg->name; reg++)
			count++;
	}
	return count;
}

static void ufs_google_capture_reg_snapshot(struct ufs_google_dbg *dbg)
{
	struct reg_group *group;
	int i = 0;

	for (group = ufs_reg_groups; group->name; group++) {
		const struct reg_info *reg_array = group->reg_list;
		void __iomem *base_addr = base_to_addr(dbg->host, group->base);

		if (IS_ERR_OR_NULL(base_addr) || !reg_array)
			continue;

		for (const struct reg_info *reg = reg_array; reg->name; reg++) {
			if (WARN_ON_ONCE(i >= dbg->snapshot_reg_count))
				return;
			dbg->regs_snapshot_values[i] =
				readl(base_addr + reg->offset);
			i++;
		}
	}
}

static void ufs_google_print_reg_snapshot(struct ufs_google_dbg *dbg)
{
	struct device *dev = dbg->host->hba->dev;
	struct reg_group *group;
	int i = 0;

	dev_err(dev, "==UFS Registers==\n");
	for (group = ufs_reg_groups; group->name; group++) {
		const struct reg_info *reg_array = group->reg_list;

		if (!reg_array)
			continue;
		for (const struct reg_info *reg = reg_array; reg->name; reg++) {
			if (WARN_ON_ONCE(i >= dbg->snapshot_reg_count))
				return;
			dev_err(dev, "[%-20s] %-40s(0x%04x) = 0x%08x\n",
				group->name, reg->name, reg->offset,
				dbg->regs_snapshot_values[i]);
			i++;
		}
	}
}

static void ufs_google_dbg_dump_work(struct work_struct *work)
{
	struct ufs_google_dbg *dbg =
		container_of(work, struct ufs_google_dbg, register_dump_work);
	unsigned long flags;

	ufs_google_capture_vregs_snapshot(dbg);

	spin_lock_irqsave(&dbg->register_snapshot_lock, flags);
	if (dump_registers_upon_error) {
		ufs_google_print_vregs_snapshot(dbg);
		ufs_google_print_reg_snapshot(dbg);
	}

	dbg->reg_dump_status = REGISTER_DUMP_INVALID;
	spin_unlock_irqrestore(&dbg->register_snapshot_lock, flags);
}

struct reg_file_context {
	struct ufs_hba *hba;
	void __iomem *base;
	u32 offset;
};

static int reg_file_show(struct seq_file *s, void *data)
{
	struct reg_file_context *ctx = s->private;
	struct ufs_hba *hba = ctx->hba;
	u32 value;
	int ret;

	ret = pm_runtime_get_sync(hba->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(hba->dev);
		return ret;
	}

	ufshcd_hold(hba);
	value = readl(ctx->base + ctx->offset);
	ufshcd_release(hba);

	pm_runtime_put_sync(hba->dev);

	seq_printf(s, "0x%08x\n", value);
	return 0;
}

static int reg_file_open(struct inode *inode, struct file *file)
{
	return single_open(file, reg_file_show, inode->i_private);
}

static const struct file_operations reg_file_fops = {
	.owner = THIS_MODULE,
	.open = reg_file_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ufs_create_reg_debugfs(struct ufs_hba *hba, struct dentry *parent,
				  const struct reg_group *group)
{
	struct ufs_google_host *host = ufshcd_get_variant(hba);
	const struct reg_info *reg_array;
	const struct reg_info *reg;
	struct dentry *root;
	void __iomem *base;

	base = base_to_addr(host, group->base);
	if (IS_ERR_OR_NULL(base)) {
		dev_err(hba->dev, "Failed to get base for %s\n", group->name);
		return PTR_ERR(base);
	}

	reg_array = group->reg_list;
	if (!reg_array)
		return 0;

	root = debugfs_create_dir(group->name, parent);
	if (IS_ERR_OR_NULL(root)) {
		dev_err(hba->dev, "Failed to create dir for %s\n", group->name);
		return PTR_ERR(root);
	}

	for (reg = reg_array; reg->name; reg++) {
		struct reg_file_context *ctx;

		ctx = devm_kzalloc(hba->dev, sizeof(*ctx), GFP_KERNEL);
		if (!ctx)
			continue;

		ctx->hba = hba;
		ctx->base = base;
		ctx->offset = reg->offset;

		debugfs_create_file(reg->name, 0444, root, ctx, &reg_file_fops);
	}

	return 0;
}

static int ufs_init_reg_debugfs_files(struct ufs_hba *hba,
				      struct dentry *parent)
{
	struct reg_group *group;
	struct dentry *reg_root;

	reg_root = debugfs_create_dir("reg", parent);
	if (IS_ERR_OR_NULL(reg_root)) {
		dev_err(hba->dev, "Failed to create debugfs dir reg (%ld)\n",
			PTR_ERR(reg_root));
		return PTR_ERR(reg_root);
	}

	for (group = ufs_reg_groups; group->name; group++)
		ufs_create_reg_debugfs(hba, reg_root, group);

	return 0;
}

struct debugfs_file_info {
	const char *name;
	umode_t mode;
	const struct file_operations *fops;
};

struct dme_cmd_context {
	struct mutex lock;
	struct ufs_hba *hba;
	int status;
	u32 attr_sel;
	u32 value;
	bool has_run;
	u8 attr_set;
	u8 peer;
};

#define UFS_GOOGLE_DEBUGFS_FILE(NAME, MODE) \
	{ .name = #NAME, .mode = MODE, .fops = &NAME##_fops }

static int dme_cmd_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static int dme_cmd_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static ssize_t dme_get_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct dme_cmd_context *ctx = file->private_data;
	struct ufs_hba *hba = ctx->hba;
	u32 attr_sel, mib_val = 0;
	char cmd_buf[64] = {};
	u8 peer;
	int ret;

	if (count >= sizeof(cmd_buf))
		return -EINVAL;

	if (copy_from_user(cmd_buf, buf, count))
		return -EFAULT;

	ret = sscanf(cmd_buf, "%x %hhx", &attr_sel, &peer);
	if (ret != 2)
		return -EINVAL;

	mutex_lock(&ctx->lock);

	ctx->attr_sel = attr_sel;
	ctx->peer = peer;

	ret = pm_runtime_get_sync(hba->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(hba->dev);
		ctx->status = ret;
		goto out_unlock;
	}

	ufshcd_hold(hba);
	ctx->status = ufshcd_dme_get_attr(hba, UIC_ARG_MIB(ctx->attr_sel),
					  &mib_val, ctx->peer);
	ufshcd_release(hba);
	pm_runtime_put_sync(hba->dev);

	if (ctx->status == 0)
		ctx->value = mib_val;
	else
		ctx->value = 0;

	ctx->has_run = true;
out_unlock:
	mutex_unlock(&ctx->lock);

	return ctx->status < 0 ? ctx->status : count;
}

static ssize_t dme_get_read(struct file *file, char __user *buf, size_t count,
			    loff_t *ppos)
{
	struct dme_cmd_context *ctx = file->private_data;
	char result_buf[64];
	int len;

	mutex_lock(&ctx->lock);
	if (!ctx->has_run) {
		len = scnprintf(result_buf, sizeof(result_buf),
				"No command run yet\n");
	} else {
		len = scnprintf(result_buf, sizeof(result_buf),
				"status=%d, attr=%#x, peer=%u, value=%#x\n",
				ctx->status, ctx->attr_sel, ctx->peer,
				ctx->value);
	}
	mutex_unlock(&ctx->lock);

	return simple_read_from_buffer(buf, count, ppos, result_buf, len);
}

static ssize_t dme_set_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct dme_cmd_context *ctx = file->private_data;
	struct ufs_hba *hba = ctx->hba;
	char cmd_buf[128] = {};
	u32 attr_sel, mib_val;
	u8 attr_set, peer;
	int ret;

	if (count >= sizeof(cmd_buf))
		return -EINVAL;

	if (copy_from_user(cmd_buf, buf, count))
		return -EFAULT;

	ret = sscanf(cmd_buf, "%x %hhx %x %hhx", &attr_sel, &attr_set, &mib_val,
		     &peer);
	if (ret != 4)
		return -EINVAL;

	mutex_lock(&ctx->lock);

	ret = pm_runtime_get_sync(hba->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(hba->dev);
		goto out_unlock;
	}

	ufshcd_hold(hba);
	ret = ufshcd_dme_set_attr(hba, UIC_ARG_MIB(attr_sel), attr_set, mib_val,
				  peer);
	ufshcd_release(hba);
	pm_runtime_put_sync(hba->dev);

out_unlock:
	mutex_unlock(&ctx->lock);

	return ret < 0 ? ret : count;
}

static const struct file_operations dme_get_fops = {
	.owner = THIS_MODULE,
	.open = dme_cmd_open,
	.release = dme_cmd_release,
	.write = dme_get_write,
	.read = dme_get_read,
};

static const struct file_operations dme_set_fops = {
	.owner = THIS_MODULE,
	.open = dme_cmd_open,
	.release = dme_cmd_release,
	.write = dme_set_write,
};

static const struct debugfs_file_info dme_debugfs_files[] = {
	UFS_GOOGLE_DEBUGFS_FILE(dme_get, 0644),
	UFS_GOOGLE_DEBUGFS_FILE(dme_set, 0200),
	{ .name = NULL }
};

static void
ufs_create_dme_debugfs_file(struct ufs_hba *hba, struct dentry *parent,
			    const struct debugfs_file_info *file_info)
{
	struct dme_cmd_context *ctx;

	ctx = devm_kzalloc(hba->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return;

	mutex_init(&ctx->lock);
	ctx->hba = hba;

	if (IS_ERR(debugfs_create_file(file_info->name, file_info->mode, parent,
				       ctx, file_info->fops)))
		dev_err(hba->dev, "Failed to create dme debugfs entry '%s'\n",
			file_info->name);
}

int ufs_init_dme_debugfs_files(struct ufs_hba *hba, struct dentry *parent)
{
	const struct debugfs_file_info *file_info;
	struct dentry *dme_root;

	dme_root = debugfs_create_dir("dme", parent);
	if (IS_ERR_OR_NULL(dme_root))
		return PTR_ERR(dme_root);

	for (file_info = dme_debugfs_files; file_info->name; file_info++)
		ufs_create_dme_debugfs_file(hba, dme_root, file_info);

	return 0;
}

int ufs_google_init_debugfs(struct ufs_hba *hba)
{
	struct ufs_google_host *host;
	struct ufs_google_dbg *dbg;
	struct dentry *root;

	host = ufshcd_get_variant(hba);
	dbg = host->dbg;

	root = debugfs_create_dir("google", hba->debugfs_root);
	if (IS_ERR_OR_NULL(root))
		return -EPERM;

	dbg->debugfs = root;

	ufs_init_reg_debugfs_files(hba, root);
	ufs_init_dme_debugfs_files(hba, root);

	return 0;
}

void ufs_google_remove_debugfs(struct ufs_hba *hba)
{
	struct ufs_google_host *host;
	struct ufs_google_dbg *dbg;

	host = ufshcd_get_variant(hba);
	dbg = host->dbg;

	debugfs_remove_recursive(dbg->debugfs);
}

static void __iomem *ufs_google_get_pinctrl_base(struct device *dev,
						 const char *phandle_name,
						 int index,
						 const char *controller_name)
{
	struct platform_device *pinctrl_pdev;
	struct device_node *controller_node;
	struct device_node *config_node;
	struct google_pinctrl *gctl;
	void __iomem *csr_base;

	config_node = of_parse_phandle(dev->of_node, phandle_name, index);
	if (!config_node) {
		dev_err(dev, "failed to parse phandle for %s\n",
			controller_name);
		return ERR_PTR(-EINVAL);
	}

	controller_node = of_get_parent(config_node);
	of_node_put(config_node);
	if (!controller_node) {
		dev_err(dev, "failed to get parent for %s\n", controller_name);
		return ERR_PTR(-ENODEV);
	}

	pinctrl_pdev = of_find_device_by_node(controller_node);
	of_node_put(controller_node);
	if (!pinctrl_pdev) {
		dev_err(dev, "failed to find platform device for %s\n",
			controller_name);
		return ERR_PTR(-ENODEV);
	}

	gctl = platform_get_drvdata(pinctrl_pdev);
	if (!gctl) {
		dev_err(dev, "failed to get drvdata for %s\n", controller_name);
		csr_base = ERR_PTR(-ENODEV);
		goto out_put_device;
	}

	csr_base = gctl->csr_base;

out_put_device:
	put_device(&pinctrl_pdev->dev);

	return csr_base;
}

int ufs_google_init_dbg(struct ufs_hba *hba)
{
	struct ufs_google_host *host;
	struct ufs_google_dbg *dbg;
	struct device *dev = hba->dev;
	size_t reg_count;

	host = ufshcd_get_variant(hba);

	dbg = devm_kzalloc(dev, sizeof(struct ufs_google_dbg), GFP_KERNEL);
	if (!dbg)
		return -ENOMEM;

	host->dbg = dbg;
	dbg->host = host;

	reg_count = ufs_google_count_regs();
	dbg->regs_snapshot_values =
		devm_kcalloc(dev, reg_count, sizeof(*dbg->regs_snapshot_values),
			     GFP_KERNEL);
	if (!dbg->regs_snapshot_values)
		return -ENOMEM;
	dbg->snapshot_reg_count = reg_count;

	spin_lock_init(&dbg->register_snapshot_lock);
	INIT_WORK(&dbg->register_dump_work, ufs_google_dbg_dump_work);
	dbg->reg_dump_status = REGISTER_DUMP_INVALID;

	dbg->psm_status_hsios_mmio = host->hsios_psm_status_mmio;
	dbg->psm_status_ufs_hc_mmio = host->ufs_hc_psm_status_mmio;
	dbg->psm_status_ufs_phy_mmio = host->ufs_phy_psm_status_mmio;

	dbg->hsios_bk_mmio = ufs_google_get_pinctrl_base(dev, "pinctrl-0", 0,
							 "hsios_pinctrl");
	if (IS_ERR(dbg->hsios_bk_mmio)) {
		dev_err(dev, "Failed to get hsios_pinctrl_mmio, error: %ld\n",
			PTR_ERR(dbg->hsios_bk_mmio));
		dbg->hsios_bk_mmio = NULL;
	}

	dbg->hsios_stby_bk_mmio =
		ufs_google_get_pinctrl_base(dev, "pinctrl-1", 0,
					    "hsios_stby_pinctrl");
	if (IS_ERR(dbg->hsios_stby_bk_mmio)) {
		dev_err(dev,
			"Failed to get hsios_stby_pinctrl_mmio, error: %ld\n",
			PTR_ERR(dbg->hsios_stby_bk_mmio));
		dbg->hsios_stby_bk_mmio = NULL;
	}

	return 0;
}

void ufs_google_remove_dbg(struct ufs_hba *hba)
{
	struct ufs_google_host *host;
	struct ufs_google_dbg *dbg;

	host = ufshcd_get_variant(hba);
	dbg = host->dbg;

	devm_kfree(hba->dev, dbg);
	devm_kfree(hba->dev, dbg->regs_snapshot_values);
}

void ufs_google_dbg_register_dump(struct ufs_hba *hba)
{
	bool uart_enabled = device_property_read_bool(hba->dev, "uart-enabled");
	struct ufs_google_host *host = ufshcd_get_variant(hba);
	struct ufs_google_dbg *dbg = host->dbg;

	dev_err(hba->dev, "uart-enabled=%d\n", uart_enabled);

	/*
	 * Use trylock to avoid blocking the caller. If a dump is already
	 * being processed, we want to return immediately without waiting.
	 */
	if (!spin_trylock(&dbg->register_snapshot_lock)) {
		dev_warn(hba->dev,
			 "%s: printing registers is in progress, skipping\n",
			 __func__);
		return;
	}

	if (dbg->reg_dump_status == REGISTER_DUMP_INVALID) {
		ufs_google_capture_reg_snapshot(dbg);
		dbg->reg_dump_status = REGISTER_DUMP_VALID;
		spin_unlock(&dbg->register_snapshot_lock);
		schedule_work(&dbg->register_dump_work);
	} else {
		/*
		 * We acquired the lock, but it is already REGISTER_DUMP_VALID.
		 * This means a snapshot from a previous error is pending and is
		 * waiting to be printed by the scheduled work item.
		 *
		 * We must skip this new request to protect the integrity of that
		 * pending dump. Capturing a new snapshot now would overwrite the
		 * existing data before the worker has a chance to print it.
		 */
		spin_unlock(&dbg->register_snapshot_lock);
		dev_warn(hba->dev,
			 "%s: printing registers is pending, skipping\n",
			 __func__);
	}
}
