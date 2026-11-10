/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Xiaomi Inc.
 */

#ifndef _LINUX_MI_PERFMGR_H
#define _LINUX_MI_PERFMGR_H

#include <linux/types.h>

struct devfreq;

#if IS_ENABLED(CONFIG_MI_PERFMGR_GPU)
extern void get_perfmgr_devfreq(struct devfreq *df);
extern int perfmgr_get_gpu_hook(unsigned long *freq, bool *check_return);
extern int (*perfmgr_notify_gpu_freq_fp)(bool need_update);
#else
static inline void get_perfmgr_devfreq(struct devfreq *df)
{
}

static inline int perfmgr_get_gpu_hook(unsigned long *freq, bool *check_return)
{
	return -1;
}
#endif

#endif /* _LINUX_MI_PERFMGR_H */
