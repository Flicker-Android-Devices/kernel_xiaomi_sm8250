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

/*
 * ====================================================================
 *                          设备配置区域
 * ====================================================================
 */
#define TARGET_VENDOR_ID  0x1915
#define TARGET_PRODUCT_ID 0xeaea
/* ==================================================================== */

/*
 * ==============================================================================
 *          推荐的目标键码 (Target Key Codes)
 * ==============================================================================
 * 您可以通过 `echo <value> > /sys/kernel/pen_btn_remapper/<device_name>/remap_pageup_to`
 * 来设置新的映射值。可以使用十进制或十六进制 (例如: echo 0x14a > ...)。
 * 写入 0 将会禁用重映射，恢复按键原始功能。
 * 
 * --- 手写笔专用 (Stylus Specific) ---
 * 320 (0x140) - BTN_STYLUS   : 主要的笔侧键 (建议)
 * 321 (0x141) - BTN_STYLUS2  : 次要的笔侧键 (建议)
 * 330 (0x14a) - BTN_TOUCH    : 笔尖接触
 * 
 * --- 媒体控制 (Media Control) ---
 * 164 (0x0a4) - KEY_PLAYPAUSE
 * 163 (0x0a3) - KEY_NEXTSONG
 * 165 (0x0a5) - KEY_PREVIOUSSONG
 * 115 (0x073) - KEY_VOLUMEUP
 * 114 (0x072) - KEY_VOLUMEDOWN
 * 166 (0x0a6) - KEY_STOPCD
 *
 * --- 相机/截图 (Camera/Screenshot) ---
 * 212 (0x0d4) - KEY_CAMERA          : 相机快门
 * 210 (0x0d2) - KEY_CAMERA_FOCUS
 * 99  (0x063) - KEY_SYSRQ           : 通常映射为截图 (Print Screen)
 *
 * --- 系统/应用控制 (System/App Control) ---
 * 139 (0x08b) - KEY_MENU
 * 158 (0x09e) - KEY_BACK
 * 172 (0x0ac) - KEY_HOMEPAGE        : 浏览器主页或系统主屏幕
 * 217 (0x0d9) - KEY_SEARCH
 * 155 (0x09b) - KEY_CYCLEWINDOWS    : 切换窗口/最近应用 (Alt-Tab)
 * 142 (0x08e) - KEY_SLEEP
 * 161 (0x0a1) - KEY_SCREENLOCK
 *
 * --- 通用功能键 (Generic Function Keys, 最佳兼容性) ---
 * F13 到 F24 是非常好的选择，因为它们不会与标准键盘冲突，
 * 可以由用户空间 App (如 Tasker) 自由捕获并定义为任何复杂操作。
 * 183 (0x0b7) - KEY_F13
 * 184 (0x0b8) - KEY_F14
 * ...
 * 194 (0x0c2) - KEY_F24
 * ==============================================================================
 */


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
    struct kobject kobj;
    struct mutex config_lock;
    unsigned int remap_pageup_to;
    unsigned int remap_pagedown_to;
};

static struct kobject *remapper_kobj;

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

    mutex_lock(&data->config_lock);
    switch (code) {
        case KEY_PAGEUP:
            remapped_code = data->remap_pageup_to;
            break;
        case KEY_PAGEDOWN:
            remapped_code = data->remap_pagedown_to;
            break;
    }
    mutex_unlock(&data->config_lock);

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
    struct pen_btn_remapper_handle_data *data = container_of(kobj, struct pen_btn_remapper_handle_data, kobj);
    unsigned int val = 0;

    mutex_lock(&data->config_lock);
    if (strcmp(attr->attr.name, "remap_pageup_to") == 0) {
        val = data->remap_pageup_to;
    } else if (strcmp(attr->attr.name, "remap_pagedown_to") == 0) {
        val = data->remap_pagedown_to;
    }
    mutex_unlock(&data->config_lock);

    return sysfs_emit(buf, "%u\n", val);
}

static ssize_t remap_attr_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    struct pen_btn_remapper_handle_data *data = container_of(kobj, struct pen_btn_remapper_handle_data, kobj);
    struct input_dev *dev = data->handle->dev;
    unsigned int new_val;
    unsigned int *target_map = NULL;
    unsigned int old_val = 0;
    int ret;

    ret = kstrtouint(buf, 0, &new_val);
    if (ret)
        return ret;

    if (new_val >= KEY_MAX)
        return -EINVAL;

    mutex_lock(&data->config_lock);

    if (strcmp(attr->attr.name, "remap_pageup_to") == 0) {
        target_map = &data->remap_pageup_to;
    } else if (strcmp(attr->attr.name, "remap_pagedown_to") == 0) {
        target_map = &data->remap_pagedown_to;
    }

    if (target_map) {
        old_val = *target_map;
        if (old_val != new_val) {
            if (old_val)
                clear_bit(old_val, dev->keybit);
            if (new_val)
                set_bit(new_val, dev->keybit);
            *target_map = new_val;
        }
    }

    mutex_unlock(&data->config_lock);

    return count;
}


static struct kobj_attribute remap_pageup_kobj_attr = {
    .attr = {
        .name = "remap_pageup_to",
        .mode = 0664,
    },
    .show = remap_attr_show,
    .store = remap_attr_store,
};

static struct kobj_attribute remap_pagedown_kobj_attr = {
    .attr = {
        .name = "remap_pagedown_to",
        .mode = 0664,
    },
    .show = remap_attr_show,
    .store = remap_attr_store,
};

static struct attribute *remap_attrs[] = {
    &remap_pageup_kobj_attr.attr,
    &remap_pagedown_kobj_attr.attr,
    NULL,
};

static void pen_btn_remapper_release(struct kobject *kobj) { /* No-op */ }
static struct kobj_type remap_ktype = {
    .sysfs_ops = &kobj_sysfs_ops,
    .release = pen_btn_remapper_release,
    .default_attrs = remap_attrs,
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
    mutex_init(&data->config_lock);
    
    data->remap_pageup_to = 0;
    data->remap_pagedown_to = 0;

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

    error = kobject_init_and_add(&data->kobj, &remap_ktype, remapper_kobj, "%s", dev->name);
    if (error) {
        pr_err("pen_btn_remapper: kobject_init_and_add failed\n");
        kobject_put(&data->kobj);
        kfree(handle);
        kfree(data);
        return error;
    }

    error = input_register_handle(handle);
    if (error) goto err_kobject;
    
    error = input_open_device(handle);
    if (error) goto err_input_handle;

    pr_info("pen_btn_remapper: Connected to device: %s\n", dev->name);
    return 0;

err_input_handle:
    input_unregister_handle(handle);
err_kobject:
    kobject_put(&data->kobj);
    return error;
}

static void pen_btn_remapper_disconnect(struct input_handle *handle)
{
    struct pen_btn_remapper_handle_data *data = handle->private;
    struct input_dev *dev = handle->dev;
    struct queued_event *event_node, *next;
    unsigned long flags;

    pr_info("pen_btn_remapper: Disconnected from device: %s\n", handle->dev->name);
    
    kobject_put(&data->kobj);

    cancel_work_sync(&data->work);

    spin_lock_irqsave(&data->queue_lock, flags);
    list_for_each_entry_safe(event_node, next, &data->event_queue, list) {
        list_del(&event_node->list);
        kfree(event_node);
    }
    spin_unlock_irqrestore(&data->queue_lock, flags);

    if (data->remap_pageup_to)
        clear_bit(data->remap_pageup_to, dev->keybit);
    if (data->remap_pagedown_to)
        clear_bit(data->remap_pagedown_to, dev->keybit);

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
    remapper_kobj = kobject_create_and_add("pen_btn_remapper", kernel_kobj);
    if (!remapper_kobj) {
        pr_err("pen_btn_remapper: Failed to create kobject\n");
        return -ENOMEM;
    }
    return input_register_handler(&pen_btn_remapper_handler);
}

static void __exit pen_btn_remapper_exit(void)
{
    input_unregister_handler(&pen_btn_remapper_handler);
    kobject_put(remapper_kobj);
}

module_init(pen_btn_remapper_init);
module_exit(pen_btn_remapper_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alcatraz/Flicker");
MODULE_DESCRIPTION("Dynamically remap input events via Sysfs for specific devices.");
MODULE_VERSION("2.2.1-final");
