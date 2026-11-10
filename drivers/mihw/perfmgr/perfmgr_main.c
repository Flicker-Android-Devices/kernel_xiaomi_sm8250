/*
 * Copyright (C) 2021 XiaoMi Inc.
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

#include <linux/sched.h>
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

#define ENABLE_DELAYED_USECS 15000000 // 15s

static int buffer_bypass = 1;
module_param(buffer_bypass, int, 0644);
MODULE_PARM_DESC(buffer_bypass, "buffer_bypass");

static int buffer_stop = 0;
module_param(buffer_stop, int, 0644);
MODULE_PARM_DESC(buffer_stop, "buffer_stop");

static int single_layer = 0;
module_param(single_layer, int, 0644);
MODULE_PARM_DESC(single_layer, "single_layer");

static int buffer_timeout_us = 80000;
module_param(buffer_timeout_us, int, 0644);
MODULE_PARM_DESC(buffer_timeout_us, "buffer_timeout_us");

static struct mutex notify_lock;
static struct mutex list_lock;
struct list_head connected_buffer_list;
struct workqueue_struct *qbuffer_notifyworkqueue;
extern int perfmgr_enable;
extern void (*perfmgr_notify_qudeq_fp)(int pid, unsigned long long identifier,
				       int hint);

#ifdef CONFIG_MI_PERFMGR_GPU
extern unsigned long target_gpu_freq;
extern unsigned int perfmgr_gpu_cur_history_items;
#endif

void perfmgr_notify_connect(int pid, unsigned long long identifier,
			    int connectedAPI);

#define MAX_CONNECTED_BUFFER 8
void *perfmgr_alloc_atomic(int i32Size)
{
	void *pvBuf;

	if (i32Size <= PAGE_SIZE)
		pvBuf = kmalloc(i32Size, GFP_ATOMIC);
	else
		pvBuf = vmalloc(i32Size);
	return pvBuf;
}

void perfmgr_free(void *pvBuf, int i32Size)
{
	if (!pvBuf)
		return;
	if (i32Size <= PAGE_SIZE)
		kfree(pvBuf);
	else
		vfree(pvBuf);
}

int perfmgr_is_enable(void)
{
	ktime_t current_time;
	s64 delta_usecs64;
	int enable = 0;
	static ktime_t last_time;
	static int qudeq_count = 0;
	static int need_delayed_enable = 0;

	if (perfmgr_enable == 0) {
		need_delayed_enable = 1;
		qudeq_count = 0;
		enable = 0;
	} else if ((perfmgr_enable == 1) && (need_delayed_enable == 1)) {
		current_time = ktime_get();
		qudeq_count++;
		if (qudeq_count == 1)
			last_time = current_time;
		delta_usecs64 = ktime_to_us(ktime_sub(current_time, last_time));
		if ((perfmgr_enable == 1) &&
		    (delta_usecs64 >= ENABLE_DELAYED_USECS)) {
			need_delayed_enable = 0;
			qudeq_count = 0;
			enable = 1;
		}
	} else {
		mutex_lock(&notify_lock);
		enable = perfmgr_enable;
		mutex_unlock(&notify_lock);
	}
	pr_debug("[perfmgr_CTRL] isenable %d\n", enable);
	return enable;
}

void perfmgr_reset_buffer(struct connected_buffer *priv)
{
	priv->is_rendering = 0;
	priv->target_fps = 0;
	priv->pid = 0;
	priv->identifier = 0;
	priv->frame_count = 0;
	priv->count_time = 0;
	priv->last_time = 0;
}

void perfmgr_init_buffer(struct connected_buffer *priv, int pid,
			 unsigned long long identifier)
{
	priv->pid = pid;
	priv->identifier = identifier;
	priv->is_rendering = 0;
	priv->target_fps = 0;
	priv->frame_count = 0;
	priv->count_time = 0;
	priv->last_time = 0;
}

int perfmgr_set_buffer(unsigned long long identifier)
{
	struct connected_buffer *priv = NULL;
	ktime_t current_time;
	s64 frame_delta_usecs64;

	current_time = ktime_get();

	mutex_lock(&list_lock);
	list_for_each_entry (priv, &connected_buffer_list, list) {
		frame_delta_usecs64 =
			ktime_to_us(ktime_sub(current_time, priv->last_time));
		if (priv->is_rendering && (priv->identifier != identifier)) {
			if (frame_delta_usecs64 > buffer_timeout_us) {
				pr_debug(
					"%s pid:%d identifier %llu reset buffer\n",
					__func__, priv->pid, priv->identifier);
				perfmgr_reset_buffer(priv);
#ifdef CONFIG_MI_PERFMGR_GPU
				target_gpu_freq = 0;
				perfmgr_gpu_cur_history_items = 0;
				pr_debug(
					"PERFMGR_GPU new id reset target_gpu_freq to %ld cur_history_items to %d\n",
					target_gpu_freq,
					perfmgr_gpu_cur_history_items);
#endif
			}
		}
	}
	mutex_unlock(&list_lock);
	return 0;
}

int perfmgr_do_filter(int pid, unsigned long long identifier, int hint)
{
	static int stop_buffer = 0;
	struct task_struct *task = NULL;
	struct pid *pid_struct = NULL;
	static ktime_t last_stop_time;
	ktime_t current_time;
	s64 frame_delta_usecs64;
	struct connected_buffer *priv = NULL;

	current_time = ktime_get();
	frame_delta_usecs64 =
		ktime_to_us(ktime_sub(current_time, last_stop_time));
	pr_debug("%s id：%llu frame_delta_usecs64 %lld stop_buffer %d 00\n",
		 __func__, identifier, frame_delta_usecs64, stop_buffer);
	if (stop_buffer && (frame_delta_usecs64 < 50000))
		return 0;
	else
		stop_buffer = 0;

	if (single_layer) {
		mutex_lock(&list_lock);
		list_for_each_entry (priv, &connected_buffer_list, list) {
			if (priv->is_rendering &&
			    (priv->identifier != identifier)) {
				pr_info("return only support single layer\n");
				stop_buffer = 1;
				last_stop_time = current_time;
				mutex_unlock(&list_lock);
				return 0;
			}
		}
		mutex_unlock(&list_lock);
	}

	pid_struct = find_get_pid(pid);
	if (pid_struct == NULL) {
		pr_err("%s fail to find_get_pid\n", __func__);
		return 1;
	}
	task = get_pid_task(pid_struct, PIDTYPE_PID);
	if (task == NULL) {
		pr_err("%s fail to get pid_task\n", __func__);
		put_pid(pid_struct);
		return 1;
	}

	if (buffer_stop && (!strcmp(task->comm, "surfaceflinger"))) {
		pr_info("%s return due to surfaceflinger\n", __func__);
		stop_buffer = 1;
		last_stop_time = current_time;
		put_task_struct(task);
		put_pid(pid_struct);
		return 0;
	}

	if (hint != POLICY_SF_COMPLETION && buffer_bypass &&
	    ((!strcmp(task->comm, "surfaceflinger")) ||
	     (!strcmp(task->comm, "curitycenter:ui")))) {
		pr_debug("return du to surfaceflinger\n");
		put_task_struct(task);
		put_pid(pid_struct);
		return 0;
	}
	put_task_struct(task);
	put_pid(pid_struct);
	return 1;
}

int perfmgr_notify_connect_cb(int pid, unsigned long long identifier, int hint)
{
	struct connected_buffer *node = NULL;
	int caculate_buffer = 0;

	pr_debug("%s pid:%d id %llu ", __func__, pid, identifier);
	perfmgr_set_buffer(identifier);
	if (!perfmgr_do_filter(pid, identifier, hint))
		return 0;

	mutex_lock(&list_lock);
	list_for_each_entry (node, &connected_buffer_list, list) {
		if (node->identifier == identifier) {
			pr_debug("%s pid:%d id %llu,list exist\n", __func__,
				 pid, identifier);
			mutex_unlock(&list_lock);
			return 1;
		}
	}
	mutex_unlock(&list_lock);

	mutex_lock(&list_lock);
	list_for_each_entry (node, &connected_buffer_list, list) {
		if (node->is_rendering == 0) {
			pr_debug(
				" %s pid:%d  node_pid:%d id %llu node_id %llu instead node\n",
				__func__, pid, node->pid, identifier,
				node->identifier);
			perfmgr_init_buffer(node, pid, identifier);
#ifdef CONFIG_MI_PERFMGR_GPU
			target_gpu_freq = 0;
			perfmgr_gpu_cur_history_items = 0;
			pr_debug(
				"PERFMGR_GPU new id init target_gpu_freq to %ld cur_history_items to %d\n",
				target_gpu_freq, perfmgr_gpu_cur_history_items);
#endif
			mutex_unlock(&list_lock);
			return 1;
		}
	}
	mutex_unlock(&list_lock);

	mutex_lock(&list_lock);
	list_for_each_entry (node, &connected_buffer_list, list) {
		caculate_buffer++;
		pr_debug("%s connected_buffer_list: pid:%d identifier：%llu "
			 "caculate_buffer num =%d\n",
			 __func__, node->pid, node->identifier,
			 caculate_buffer);
	}
	mutex_unlock(&list_lock);

	if (caculate_buffer >= MAX_CONNECTED_BUFFER) {
		pr_debug(
			"%s pid:%d identifier %llu connectedAPI reached max buffer\n",
			__func__, pid, identifier);
		return 0;
	}
	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node) {
		return -ENOMEM;
	}
	memset(node, 0, sizeof(struct connected_buffer));
	perfmgr_init_buffer(node, pid, identifier);
	mutex_lock(&list_lock);
	list_add_tail(&node->list, &connected_buffer_list);
	mutex_unlock(&list_lock);
	pr_debug("%s pid:%d id %llu add node list\n", __func__, pid,
		 identifier);
	return 1;
}

int perfmgr_notify_qudeq_cb(int pid, unsigned long long identifier, int hint)
{
	struct connected_buffer *priv = NULL;

	pr_debug("%s pid:%d id：%llu hint%d\n", __func__, pid, identifier,
		 hint);
	if (perfmgr_notify_connect_cb(pid, identifier, hint)) {
		mutex_lock(&list_lock);
		list_for_each_entry (priv, &connected_buffer_list, list) {
			if (priv->identifier == identifier) {
				if (hint == POLICY_EGL_QUEUE) {
					perfmgr_do_policy(priv);
				} else if (hint == POLICY_SF_COMPLETION) {
					perfmgr_update_sf_msg(priv);
				}
				break;
			}
		}
		mutex_unlock(&list_lock);
	}
	return 0;
}

static void perfmgr_notifer_wq_cb(struct work_struct *psWork)
{
	struct PERFMGR_NOTIFIER_PUSH_TAG *vpPush =
		container_of(psWork, struct PERFMGR_NOTIFIER_PUSH_TAG, sWork);

	if (!vpPush) {
		pr_debug("[perfmgr_CTRL] ERROR\n");
		return;
	}
	pr_debug("[perfmgr_CTRL] perfmgr_notifer_wq_cb push type = %d\n",
		 vpPush->ePushType);

	switch (vpPush->ePushType) {
	case PERFMGR_NOTIFIER_QUEUE_DEQUEUE:
		perfmgr_notify_qudeq_cb(vpPush->pid, vpPush->identifier,
					vpPush->hint);
		break;
	default:
		pr_debug("[perfmgr_CTRL] unhandled push type  = %d\n",
			 vpPush->ePushType);
		break;
	}
	perfmgr_free(vpPush, sizeof(struct PERFMGR_NOTIFIER_PUSH_TAG));
}

void perfmgr_notify_qudeq(int pid, unsigned long long identifier, int hint)
{
	struct PERFMGR_NOTIFIER_PUSH_TAG *vpPush = NULL;

	pr_debug("%s pid %d id %llu\n", __func__, pid, identifier);
	if (!perfmgr_is_enable())
		return;
	vpPush = (struct PERFMGR_NOTIFIER_PUSH_TAG *)perfmgr_alloc_atomic(
		sizeof(struct PERFMGR_NOTIFIER_PUSH_TAG));
	if (!vpPush) {
		pr_debug("[perfmgr_CTRL] OOM\n");
		return;
	}
	if (!qbuffer_notifyworkqueue) {
		pr_debug("[perfmgr_CTRL] NULL WorkQueue\n");
		perfmgr_free(vpPush, sizeof(struct PERFMGR_NOTIFIER_PUSH_TAG));
		return;
	}
	vpPush->ePushType = PERFMGR_NOTIFIER_QUEUE_DEQUEUE;
	vpPush->pid = pid;
	vpPush->hint = hint;
	vpPush->identifier = identifier;
	INIT_WORK(&vpPush->sWork, perfmgr_notifer_wq_cb);
	queue_work(qbuffer_notifyworkqueue, &vpPush->sWork);
}

static int perfmgr_probe(struct platform_device *dev)
{
	return 0;
}

struct platform_device perfmgr_device = {
	.name = "perfmgr",
	.id = -1,
};

static int perfmgr_suspend(struct device *dev)
{
	return 0;
}

static int perfmgr_resume(struct device *dev)
{
	return 0;
}

static int perfmgr_remove(struct platform_device *dev)
{
	return 0;
}

static struct platform_driver perfmgr_driver = {
	.probe = perfmgr_probe,
	.remove = perfmgr_remove,
	.driver = {
		.name = "perfmgr",
		.pm = &(const struct dev_pm_ops){
			.suspend = perfmgr_suspend,
			.resume = perfmgr_resume,
		},
	},
};

int init_perfmgr(void)
{
	int ret = 0;
	struct proc_dir_entry *perfmgr_root = NULL;

	ret = platform_device_register(&perfmgr_device);
	if (ret)
		return ret;
	ret = platform_driver_register(&perfmgr_driver);
	if (ret)
		return ret;
	qbuffer_notifyworkqueue = alloc_ordered_workqueue(
		"%s", WQ_MEM_RECLAIM | WQ_HIGHPRI, "perfmgr_notifier_wq");
	if (qbuffer_notifyworkqueue == NULL)
		return -EFAULT;

	mutex_init(&notify_lock);
	mutex_init(&list_lock);
	INIT_LIST_HEAD(&connected_buffer_list);
	perfmgr_notify_qudeq_fp = perfmgr_notify_qudeq;
	perfmgr_root = proc_mkdir("perfmgr", NULL);
	init_perfctl(perfmgr_root);
	perfmgr_policy_init();
#ifdef CONFIG_MI_PERFMGR_GPU
	perfmgr_gpu_init();
#endif
	perfmgr_remap_init();
	return 0;
}

static int perfmgr_module_init(void)
{
	init_perfmgr();
	return 0;
}
module_init(perfmgr_module_init);
MODULE_LICENSE("GPL v2");
