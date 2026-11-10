// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021 XiaoMi Inc.
 *
 * FEAS Devfreq Remap Driver for SM8250.
 * Dynamically hooks Devfreq (DDR and LLCC/L3 bus bandwidth)
 * to provide hardware memory bandwidth floor boosts during CPU rescue spikes.
 */

#define pr_fmt(fmt) "perfmgr_remap: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sysfs.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/devfreq.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/jiffies.h>
#include "perfmgr.h"

extern int update_devfreq(struct devfreq *devfreq);

#define MAX_REMAP 4
#define DDR_DEV_NAME_1 "qcom,cpu-llcc-ddr-bw"
#define DDR_DEV_NAME_2 "cpu-llcc-ddr-bw"
#define LLCC_DEV_NAME_1 "qcom,cpu-cpu-llcc-bw"
#define LLCC_DEV_NAME_2 "cpu-cpu-llcc-bw"

static int d_boost;
module_param(d_boost, int, 0644);
MODULE_PARM_DESC(d_boost, "FEAS DDR frequency boost enable");

static int l3freq_boost;
module_param(l3freq_boost, int, 0644);
MODULE_PARM_DESC(l3freq_boost, "FEAS LLCC/L3 frequency boost enable");

static int dcvs_timeout_ms = 1500;
module_param(dcvs_timeout_ms, int, 0644);
MODULE_PARM_DESC(dcvs_timeout_ms, "FEAS DCVS devfreq timeout in ms");

struct dcvsfreq_remap {
	unsigned int cpufreq_khz;
	unsigned int dcvsfreq_khz;
};

static struct dcvsfreq_remap ddr_remap_table[MAX_REMAP];
static struct dcvsfreq_remap l3_remap_table[MAX_REMAP];

static struct devfreq *ddr_devfreq;
static struct devfreq *llcc_devfreq;
static DEFINE_MUTEX(remap_lock);

static struct delayed_work dcvsfreq_release_work;

void perfmgr_register_ddr_devfreq(struct devfreq *df)
{
	mutex_lock(&remap_lock);
	ddr_devfreq = df;
	mutex_unlock(&remap_lock);
}
EXPORT_SYMBOL(perfmgr_register_ddr_devfreq);

void perfmgr_register_llcc_devfreq(struct devfreq *df)
{
	mutex_lock(&remap_lock);
	llcc_devfreq = df;
	mutex_unlock(&remap_lock);
}
EXPORT_SYMBOL(perfmgr_register_llcc_devfreq);

static struct devfreq *get_ddr_devfreq(void)
{
	if (!ddr_devfreq) {
		ddr_devfreq = devfreq_get_devfreq_by_name(DDR_DEV_NAME_1);
		if (IS_ERR_OR_NULL(ddr_devfreq))
			ddr_devfreq =
				devfreq_get_devfreq_by_name(DDR_DEV_NAME_2);
		if (IS_ERR(ddr_devfreq))
			ddr_devfreq = NULL;
	}
	return ddr_devfreq;
}

static struct devfreq *get_llcc_devfreq(void)
{
	if (!llcc_devfreq) {
		llcc_devfreq = devfreq_get_devfreq_by_name(LLCC_DEV_NAME_1);
		if (IS_ERR_OR_NULL(llcc_devfreq))
			llcc_devfreq =
				devfreq_get_devfreq_by_name(LLCC_DEV_NAME_2);
		if (IS_ERR(llcc_devfreq))
			llcc_devfreq = NULL;
	}
	return llcc_devfreq;
}

/*
 * Convert DDR frequency (kHz) or bandwidth (MB/s) to devbw target (MB/s).
 * Kona bus width = 4: (mhz * 1000000 * 4) / (1024 * 1024)
 */
static unsigned long convert_ddr_bw(unsigned int val)
{
	unsigned long long mhz;

	if (val == 0)
		return 0;
	if (val > 50000) {
		mhz = val / 1000;
		return (unsigned long)((mhz * 4000000ULL) >> 20);
	}
	return (unsigned long)val;
}

/*
 * Convert LLCC frequency (kHz) or bandwidth (MB/s) to devbw target (MB/s).
 * Kona LLCC bus width = 16: (mhz * 1000000 * 16) / (1024 * 1024)
 */
static unsigned long convert_llcc_bw(unsigned int val)
{
	unsigned long long mhz;

	if (val == 0)
		return 0;
	if (val > 50000) {
		mhz = val / 1000;
		return (unsigned long)((mhz * 16000000ULL) >> 20);
	}
	return (unsigned long)val;
}

static void perfmgr_boost_dcvsfreq_online(int freq, u32 hw_type)
{
	struct devfreq *df;
	unsigned long target_val = 0;
	int i;

	if (hw_type == DCVS_DDR) {
		df = get_ddr_devfreq();
		if (!df)
			return;

		if (freq > 0) {
			for (i = 0; i < MAX_REMAP; i++) {
				if (ddr_remap_table[i].cpufreq_khz &&
				    freq > ddr_remap_table[i].cpufreq_khz) {
					target_val = convert_ddr_bw(
						ddr_remap_table[i].dcvsfreq_khz);
					break;
				}
			}
			if (target_val == 0 &&
			    ddr_remap_table[MAX_REMAP - 1].cpufreq_khz)
				target_val = convert_ddr_bw(
					ddr_remap_table[MAX_REMAP - 1]
						.dcvsfreq_khz);
		} else {
			target_val = 0;
		}

		mutex_lock(&df->lock);
		df->min_freq = target_val;
		update_devfreq(df);
		mutex_unlock(&df->lock);

		perfmgr_dbg(
			debug_mask_perfmgr, PERFMGR_DCVS,
			"%s: DDR devfreq boost cpufreq=%d, target_bw=%lu MB/s\n",
			__func__, freq, target_val);
	} else if (hw_type == DCVS_L3) {
		df = get_llcc_devfreq();
		if (!df)
			return;

		if (freq > 0) {
			for (i = 0; i < MAX_REMAP; i++) {
				if (l3_remap_table[i].cpufreq_khz &&
				    freq > l3_remap_table[i].cpufreq_khz) {
					target_val = convert_llcc_bw(
						l3_remap_table[i].dcvsfreq_khz);
					break;
				}
			}
			if (target_val == 0 &&
			    l3_remap_table[MAX_REMAP - 1].cpufreq_khz)
				target_val = convert_llcc_bw(
					l3_remap_table[MAX_REMAP - 1]
						.dcvsfreq_khz);
		} else {
			target_val = 0;
		}

		mutex_lock(&df->lock);
		df->min_freq = target_val;
		update_devfreq(df);
		mutex_unlock(&df->lock);

		perfmgr_dbg(
			debug_mask_perfmgr, PERFMGR_DCVS,
			"%s: LLCC/L3 devfreq boost cpufreq=%d, target_bw=%lu MB/s\n",
			__func__, freq, target_val);
	}
}

static int perfmgr_ddrfreq_remap_set(const char *buf,
				     const struct kernel_param *param)
{
	int ret = 0;
	char *sptr, *token, *str;
	u32 val, i;

	str = kstrdup(buf, GFP_KERNEL);
	if (!str)
		return -ENOMEM;
	sptr = str;
	for (i = 0; i < MAX_REMAP; i++) {
		token = strsep(&sptr, ":");
		if (!token) {
			ret = -EINVAL;
			break;
		}
		ret = kstrtouint(token, 10, &val);
		if (ret < 0)
			break;
		ddr_remap_table[i].cpufreq_khz = val;

		token = strsep(&sptr, " ");
		if (!token) {
			ret = -EINVAL;
			break;
		}
		ret = kstrtouint(token, 10, &val);
		if (ret < 0)
			break;
		ddr_remap_table[i].dcvsfreq_khz = val;
	}
	kfree(str);
	return ret;
}

static int perfmgr_l3freq_remap_set(const char *buf,
				    const struct kernel_param *param)
{
	int ret = 0;
	char *sptr, *token, *str;
	u32 val, i;

	str = kstrdup(buf, GFP_KERNEL);
	if (!str)
		return -ENOMEM;
	sptr = str;
	for (i = 0; i < MAX_REMAP; i++) {
		token = strsep(&sptr, ":");
		if (!token) {
			ret = -EINVAL;
			break;
		}
		ret = kstrtouint(token, 10, &val);
		if (ret < 0)
			break;
		l3_remap_table[i].cpufreq_khz = val;

		token = strsep(&sptr, " ");
		if (!token) {
			ret = -EINVAL;
			break;
		}
		ret = kstrtouint(token, 10, &val);
		if (ret < 0)
			break;
		l3_remap_table[i].dcvsfreq_khz = val;
	}
	kfree(str);
	return ret;
}

static int perfmgr_ddrfreq_remap_get(char *buf,
				     const struct kernel_param *param)
{
	unsigned int cnt = 0;
	unsigned int i;

	if (d_boost) {
		cnt += scnprintf(buf, PAGE_SIZE,
				 "CPU freq (kHz)\tMem freq/bw (kHz/MBps)\n");
		for (i = 0; i < MAX_REMAP && ddr_remap_table[i].cpufreq_khz;
		     i++) {
			cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
					 "%14u\t%14u\n",
					 ddr_remap_table[i].cpufreq_khz,
					 ddr_remap_table[i].dcvsfreq_khz);
		}
		if (cnt < PAGE_SIZE)
			cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	}
	return cnt;
}

static int perfmgr_l3freq_remap_get(char *buf, const struct kernel_param *param)
{
	unsigned int cnt = 0;
	unsigned int i;

	if (l3freq_boost) {
		cnt += scnprintf(buf, PAGE_SIZE,
				 "CPU freq (kHz)\tMem freq/bw (kHz/MBps)\n");
		for (i = 0; i < MAX_REMAP && l3_remap_table[i].cpufreq_khz;
		     i++) {
			cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
					 "%14u\t%14u\n",
					 l3_remap_table[i].cpufreq_khz,
					 l3_remap_table[i].dcvsfreq_khz);
		}
		if (cnt < PAGE_SIZE)
			cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	}
	return cnt;
}

module_param_call(d_table, perfmgr_ddrfreq_remap_set, perfmgr_ddrfreq_remap_get,
		  NULL, 0644);
MODULE_PARM_DESC(d_table, "d_table: CPU freq to DDR freq/bw map");

module_param_call(l3freq_remap_table, perfmgr_l3freq_remap_set,
		  perfmgr_l3freq_remap_get, NULL, 0644);
MODULE_PARM_DESC(l3freq_remap_table,
		 "l3freq_remap_table: CPU freq to LLCC/L3 freq/bw map");

static void perfmgr_dcvsfreq_release_work(struct work_struct *work)
{
	perfmgr_dbg(debug_mask_perfmgr, PERFMGR_DCVS,
		    "%s: DCVS devfreq boost timed out\n", __func__);
	if (d_boost)
		perfmgr_boost_dcvsfreq_online(0, DCVS_DDR);
	if (l3freq_boost)
		perfmgr_boost_dcvsfreq_online(0, DCVS_L3);
}

void perfmgr_boost_dcvsfreq_and_timeout(unsigned int maxfreq)
{
	if (d_boost || l3freq_boost) {
		cancel_delayed_work(&dcvsfreq_release_work);
		if (d_boost)
			perfmgr_boost_dcvsfreq_online(maxfreq, DCVS_DDR);
		if (l3freq_boost)
			perfmgr_boost_dcvsfreq_online(maxfreq, DCVS_L3);
		if (maxfreq > 0)
			schedule_delayed_work(
				&dcvsfreq_release_work,
				msecs_to_jiffies(dcvs_timeout_ms));
	}
}
EXPORT_SYMBOL(perfmgr_boost_dcvsfreq_and_timeout);

int perfmgr_remap_init(void)
{
	pr_debug("%s: initializing devfreq remap\n", __func__);

	/* DDR default table: CPU freq (kHz) -> DDR freq (kHz or MBps) */
	ddr_remap_table[0].cpufreq_khz = 1500000;
	ddr_remap_table[0].dcvsfreq_khz = 2736000; /* 10437 MB/s */
	ddr_remap_table[1].cpufreq_khz = 1400000;
	ddr_remap_table[1].dcvsfreq_khz = 2092000; /* 7980 MB/s */
	ddr_remap_table[2].cpufreq_khz = 1300000;
	ddr_remap_table[2].dcvsfreq_khz = 1804000; /* 6881 MB/s */
	ddr_remap_table[3].cpufreq_khz = 1200000;
	ddr_remap_table[3].dcvsfreq_khz = 1555000; /* 5931 MB/s */

	/* L3/LLCC default table: CPU freq (kHz) -> LLCC freq (kHz or MBps) */
	l3_remap_table[0].cpufreq_khz = 1700000;
	l3_remap_table[0].dcvsfreq_khz = 1000000; /* 15258 MB/s */
	l3_remap_table[1].cpufreq_khz = 1500000;
	l3_remap_table[1].dcvsfreq_khz = 806000; /* 12298 MB/s */
	l3_remap_table[2].cpufreq_khz = 1300000;
	l3_remap_table[2].dcvsfreq_khz = 600000; /* 9155 MB/s */
	l3_remap_table[3].cpufreq_khz = 1200000;
	l3_remap_table[3].dcvsfreq_khz = 466000; /* 7110 MB/s */

	INIT_DELAYED_WORK(&dcvsfreq_release_work,
			  perfmgr_dcvsfreq_release_work);
	return 0;
}

void perfmgr_remap_exit(void)
{
	cancel_delayed_work_sync(&dcvsfreq_release_work);
	if (d_boost)
		perfmgr_boost_dcvsfreq_online(0, DCVS_DDR);
	if (l3freq_boost)
		perfmgr_boost_dcvsfreq_online(0, DCVS_L3);
}
