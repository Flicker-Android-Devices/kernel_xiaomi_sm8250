/* Pen Button Remapper driver
 *
 * Copyright (C) 2025 Alcatraz323 <alcatraz32323@gmail.com>. All rights reserved.
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/atomic.h>

#define TARGET_VENDOR_ID  0x1915
#define TARGET_PRODUCT_ID 0xeaea // Xiaomi Smart Pen

// Note: EV_KEY def of linux and Android's KeyEvent def are diffrent.
// Use this with Generic.kl or your devices .kl files

struct queued_event {
    struct list_head list;
    unsigned int type;
    unsigned int code;
    int value;
};

struct pen_btn_remapper_handle_data {
    struct work_struct work;
    struct list_head event_queue;
    spinlock_t queue_lock;
    struct input_handle *handle;
};

static struct kobject *remapper_kobj;
static unsigned int remap_pageup_to = 0;
static unsigned int remap_pagedown_to = 0;
static DEFINE_MUTEX(config_lock);

static atomic_t connected_devices = ATOMIC_INIT(0);
static struct input_dev *first_dev = NULL;


static void pen_btn_remapper_work_handler(struct work_struct *work);

static bool pen_btn_remapper_filter(struct input_handle *handle, unsigned int type, unsigned int code, int value)
{
    struct pen_btn_remapper_handle_data *data = handle->private;
    unsigned int remapped_code = 0;
    struct queued_event *event_node;
    unsigned long flags;

    if (type != EV_KEY) {
        return false;
    }

    mutex_lock(&config_lock);
    switch (code) {
        case KEY_PAGEUP:
            remapped_code = remap_pageup_to;
            break;
        case KEY_PAGEDOWN:
            remapped_code = remap_pagedown_to;
            break;
    }
    mutex_unlock(&config_lock);

    if (remapped_code == 0) {
        return false;
    }

    event_node = kmalloc(sizeof(*event_node), GFP_ATOMIC);
    if (!event_node) {
        return true;
    }

    event_node->type = type;
    event_node->code = remapped_code;
    event_node->value = value;

    spin_lock_irqsave(&data->queue_lock, flags);
    list_add_tail(&event_node->list, &data->event_queue);
    spin_unlock_irqrestore(&data->queue_lock, flags);

    schedule_work(&data->work);
    
    return true;
}

static void pen_btn_remapper_work_handler(struct work_struct *work)
{
    struct pen_btn_remapper_handle_data *data = container_of(work, struct pen_btn_remapper_handle_data, work);
    struct input_handle *handle = data->handle;
    struct queued_event *event_node;
    unsigned long flags;

    while (true) {
        spin_lock_irqsave(&data->queue_lock, flags);
        if (list_empty(&data->event_queue)) {
            spin_unlock_irqrestore(&data->queue_lock, flags);
            break;
        }
        event_node = list_first_entry(&data->event_queue, struct queued_event, list);
        list_del(&event_node->list);
        spin_unlock_irqrestore(&data->queue_lock, flags);

        input_event(handle->dev, event_node->type, event_node->code, event_node->value);
        input_sync(handle->dev);

        kfree(event_node);
    }
}

static ssize_t remap_attr_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    unsigned int val = 0;

    mutex_lock(&config_lock);
    if (strcmp(attr->attr.name, "remap_pageup_to") == 0) {
        val = remap_pageup_to;
    } else if (strcmp(attr->attr.name, "remap_pagedown_to") == 0) {
        val = remap_pagedown_to;
    }
    mutex_unlock(&config_lock);

    return sysfs_emit(buf, "%u\n", val);
}

static ssize_t remap_attr_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    unsigned int new_val;
    unsigned int *target_map = NULL;
    unsigned int old_val = 0;
    int ret;

    ret = kstrtouint(buf, 0, &new_val);
    if (ret)
        return ret;

    if (new_val >= KEY_MAX)
        return -EINVAL;

    mutex_lock(&config_lock);

    if (strcmp(attr->attr.name, "remap_pageup_to") == 0) {
        target_map = &remap_pageup_to;
    } else if (strcmp(attr->attr.name, "remap_pagedown_to") == 0) {
        target_map = &remap_pagedown_to;
    }

    if (target_map) {
        old_val = *target_map;
        if (old_val != new_val) {
            if (first_dev) {
                if (old_val)
                    clear_bit(old_val, first_dev->keybit);
                if (new_val)
                    set_bit(new_val, first_dev->keybit);
            }
            *target_map = new_val;
        }
    }

    mutex_unlock(&config_lock);

    return count;
}


static struct kobj_attribute remap_pageup_kobj_attr = {
    .attr = { .name = "remap_pageup_to", .mode = 0664 },
    .show = remap_attr_show,
    .store = remap_attr_store,
};
static struct kobj_attribute remap_pagedown_kobj_attr = {
    .attr = { .name = "remap_pagedown_to", .mode = 0664 },
    .show = remap_attr_show,
    .store = remap_attr_store,
};

static struct attribute *remap_attrs[] = {
    &remap_pageup_kobj_attr.attr,
    &remap_pagedown_kobj_attr.attr,
    NULL,
};

static struct attribute_group remap_attr_group = {
    .attrs = remap_attrs,
};


static int pen_btn_remapper_connect(struct input_handler *handler, struct input_dev *dev, const struct input_device_id *id)
{
    struct input_handle *handle;
    struct pen_btn_remapper_handle_data *data;
    int error;

    if (dev->id.vendor != TARGET_VENDOR_ID || dev->id.product != TARGET_PRODUCT_ID) {
        return -ENODEV;
    }

    data = kzalloc(sizeof(*data), GFP_KERNEL);
    if (!data) return -ENOMEM;
    
    INIT_WORK(&data->work, pen_btn_remapper_work_handler);
    INIT_LIST_HEAD(&data->event_queue);
    spin_lock_init(&data->queue_lock);
    
    handle = kzalloc(sizeof(*handle), GFP_KERNEL);
    if (!handle) {
        kfree(data);
        return -ENOMEM;
    }

    handle->dev = dev;
    handle->handler = handler;
    handle->name = "pen_btn_remapper_handle";
    handle->private = data;
    data->handle = handle;

    error = input_register_handle(handle);
    if (error) {
        kfree(handle);
        kfree(data);
        return error;
    }
    
    error = input_open_device(handle);
    if (error) {
        input_unregister_handle(handle);
        return error;
    }

    if (atomic_inc_return(&connected_devices) == 1) {
        first_dev = dev;
        mutex_lock(&config_lock);
        if (remap_pageup_to)
            set_bit(remap_pageup_to, dev->keybit);
        if (remap_pagedown_to)
            set_bit(remap_pagedown_to, dev->keybit);
        mutex_unlock(&config_lock);
    }


    pr_info("pen_btn_remapper: Connected to device: %s\n", dev->name);
    return 0;
}

static void pen_btn_remapper_disconnect(struct input_handle *handle)
{
    struct pen_btn_remapper_handle_data *data = handle->private;
    struct input_dev *dev = handle->dev;
    struct queued_event *event_node, *next;
    unsigned long flags;

    pr_info("pen_btn_remapper: Disconnected from device: %s\n", handle->dev->name);
    
    cancel_work_sync(&data->work);

    spin_lock_irqsave(&data->queue_lock, flags);
    list_for_each_entry_safe(event_node, next, &data->event_queue, list) {
        list_del(&event_node->list);
        kfree(event_node);
    }
    spin_unlock_irqrestore(&data->queue_lock, flags);

    if (atomic_dec_and_test(&connected_devices)) {
        mutex_lock(&config_lock);
        if (remap_pageup_to)
            clear_bit(remap_pageup_to, dev->keybit);
        if (remap_pagedown_to)
            clear_bit(remap_pagedown_to, dev->keybit);
        mutex_unlock(&config_lock);
        first_dev = NULL;
    }

    input_close_device(handle);
    input_unregister_handle(handle);
    
    kfree(data);
    kfree(handle);
}

static const struct input_device_id pen_btn_remapper_ids[] = {
    { .driver_info = 1 },
    { },
};

static struct input_handler pen_btn_remapper_handler = {
    .filter     = pen_btn_remapper_filter,
    .connect    = pen_btn_remapper_connect,
    .disconnect = pen_btn_remapper_disconnect,
    .name       = "pen_btn_remapper",
    .id_table   = pen_btn_remapper_ids,
};

static int __init pen_btn_remapper_init(void)
{
    int error;

    remapper_kobj = kobject_create_and_add("pen_btn_remapper", kernel_kobj);
    if (!remapper_kobj) {
        pr_err("pen_btn_remapper: Failed to create kobject\n");
        return -ENOMEM;
    }

    error = sysfs_create_group(remapper_kobj, &remap_attr_group);
    if (error) {
        pr_err("pen_btn_remapper: Failed to create sysfs group\n");
        kobject_put(remapper_kobj);
        return error;
    }

    return input_register_handler(&pen_btn_remapper_handler);
}

static void __exit pen_btn_remapper_exit(void)
{
    sysfs_remove_group(remapper_kobj, &remap_attr_group);
    input_unregister_handler(&pen_btn_remapper_handler);
    kobject_put(remapper_kobj);
}

module_init(pen_btn_remapper_init);
module_exit(pen_btn_remapper_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alcatraz/Flicker");
MODULE_DESCRIPTION("Globally remap input events via a single Sysfs interface.");
MODULE_VERSION("2.3.1-global-final");
