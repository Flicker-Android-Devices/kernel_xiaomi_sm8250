/*
 * Copyright (C) 2021 Xiaomi Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <asm/page.h>
#include <linux/workqueue.h>
#include <linux/version.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/proc_fs.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include "perfmgr.h"

void (*perfmgr_notify_qudeq_fp)(int pid, unsigned long long identifier,
				int hint);
void (*perfmgr_notify_connect_fp)(int pid, unsigned long long identifier,
				  int connectedAPI);
void (*perfmgr_notify_gpu_hint_fp)(int start, int pid,
				   unsigned long long frameNum);

#ifdef CONFIG_MI_PERFMGR_GPU
static struct proc_dir_entry *perfmgr_gpu_proc;
extern int show_gpu_load(struct seq_file *m, void *v);
#endif

static int device_show(struct seq_file *m, void *v)
{
	return 0;
}

static int device_open(struct inode *inode, struct file *file)
{
	return single_open(file, device_show, inode->i_private);
}

static long device_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	ssize_t ret = 0;
	struct _BUFFER_PACKAGE *msg = NULL;
	struct _BUFFER_PACKAGE *msgUM = (struct _BUFFER_PACKAGE *)arg;
	struct _BUFFER_PACKAGE smsg;

	msg = &smsg;
	if (copy_from_user(msg, msgUM, sizeof(struct _BUFFER_PACKAGE))) {
		ret = -EFAULT;
		goto ret_ioctl;
	}
	pr_debug("%s %d: cmd %x start %d id %llu\n", __FILE__, __LINE__, cmd,
		 msg->start, msg->identifier);

	switch (cmd) {
	case BUFFER_QUEUE:
		if ((perfmgr_notify_qudeq_fp) && (msg->start))
			perfmgr_notify_qudeq_fp(msg->pid, msg->identifier,
						msg->start);
		break;
	case BUFFER_QUEUE_CONNECT:
		if (perfmgr_notify_connect_fp)
			perfmgr_notify_connect_fp(msg->pid, msg->identifier,
						  msg->connectedAPI);
		break;
	case BUFFER_GPU_HINT:
		if (perfmgr_notify_gpu_hint_fp)
			perfmgr_notify_gpu_hint_fp(msg->start, msg->pid,
						   msg->frame_id);
		else
			pr_debug("perfmgr_notify_gpu_hint_fp is null\n");
		break;
	default:
		pr_debug("%s %d: unknown cmd %x\n", __FILE__, __LINE__, cmd);
		ret = -1;
		goto ret_ioctl;
	}

ret_ioctl:
	return ret;
}

/* Linux 4.19 uses struct file_operations for procfs */
static const struct file_operations Fops = {
	.owner = THIS_MODULE,
	.open = device_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.unlocked_ioctl = device_ioctl,
	.compat_ioctl = device_ioctl,
};

#ifdef CONFIG_MI_PERFMGR_GPU
static int gpu_load_show(struct seq_file *m, void *v)
{
	seq_printf(m, "1 gpu load history\n");
	show_gpu_load(m, v);
	return 0;
}

static int gpu_open(struct inode *inode, struct file *file)
{
	return single_open(file, gpu_load_show, NULL);
}

static const struct file_operations gpu_fops = {
	.owner = THIS_MODULE,
	.open = gpu_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

int init_perfctl(struct proc_dir_entry *parent)
{
	struct proc_dir_entry *pe;
	int ret_val = 0;

	pr_debug("Start to init perf_ioctl driver\n");
	pe = proc_create("perf_ioctl", 0664, parent, &Fops);
	if (!pe) {
		pr_debug("%s failed with %d\n", "Creating file node ", ret_val);
		ret_val = -ENOMEM;
		goto out_wq;
	}
#ifdef CONFIG_MI_PERFMGR_GPU
	perfmgr_gpu_proc = proc_create("perfmgr_gpu", 0664, parent, &gpu_fops);
#endif
	pr_debug("init perf_ioctl driver done\n");
	return 0;
out_wq:
	return ret_val;
}
