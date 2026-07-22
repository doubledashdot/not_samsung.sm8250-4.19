// E404 kernel helper by Project 113 (kvsnr113)

#include <linux/e404_attributes.h>

struct e404_attributes e404_data = {
    .kgsl_skip_zeroing          = 0,
    .file_sync                  = 1,
    .avoid_dirty_pte = 1,
    .bg_blocklist               = "com.shopee.id,com.lazada.android,com.tokopedia.tkpd",
};

static int  blocked_cnt;
static u8   blocked_len[E404_MAX_BLOCKED];
static char blocked[E404_MAX_BLOCKED][TASK_COMM_LEN];

static struct kobject *e404_kobj;

bool e404_comm_blocked(const char *comm)
{
    int i;

    for (i = 0; i < blocked_cnt; i++) {
        if (!strncmp(comm,
                     blocked[i],
                     blocked_len[i]))
            return true;
    }

    return false;
}
EXPORT_SYMBOL_GPL(e404_comm_blocked);

static void e404_rebuild_blocklist(char *buf)
{
    char *p = buf;
    char *token;

    blocked_cnt = 0;
    while ((token = strsep(&p, ",")) &&
           blocked_cnt < E404_MAX_BLOCKED) {

        if (!*token)
            continue;

        strscpy(blocked[blocked_cnt],
                token,
                TASK_COMM_LEN);

        blocked_len[blocked_cnt] =
            strlen(blocked[blocked_cnt]);

        pr_alert("E404: blocking '%s'\n", blocked[blocked_cnt]);
        blocked_cnt++;
    }
    pr_alert("E404: total blocked apps = %d\n", blocked_cnt);
}

static ssize_t bg_blocklist_show(struct kobject *kobj,
                                 struct kobj_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%s\n",
                     e404_data.bg_blocklist);
}

static ssize_t bg_blocklist_store(struct kobject *kobj,
                                  struct kobj_attribute *attr,
                                  const char *buf, size_t count)
{
    char tmp[E404_BLOCKLIST_STRLEN];

    strscpy(tmp, buf, sizeof(tmp));
    strreplace(tmp, '\n', '\0');
    strscpy(e404_data.bg_blocklist,
            tmp,
            sizeof(e404_data.bg_blocklist));

    e404_rebuild_blocklist(tmp);

    return count;
}

static struct kobj_attribute bg_blocklist_attr =
    __ATTR(bg_blocklist, 0664, bg_blocklist_show, bg_blocklist_store);

#define E404_ATTR_RO(name) \
static ssize_t name##_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) { \
    return sprintf(buf, "%d\n", e404_data.name); \
} \
static struct kobj_attribute name##_attr = __ATTR(name, 0444, name##_show, NULL);

#define E404_ATTR_RW(name) \
static ssize_t name##_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) { \
    return sprintf(buf, "%d\n", e404_data.name); \
} \
static ssize_t name##_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) { \
    int ret, val, old_val; \
    ret = kstrtoint(buf, 10, &val); \
    if (ret) return ret; \
    old_val = e404_data.name; \
    e404_data.name = val; \
    pr_alert("E404: %s changed from %d to %d\n", #name, old_val, val); \
    sysfs_notify(e404_kobj, NULL, #name); \
    return count; \
} \
static struct kobj_attribute name##_attr = __ATTR(name, 0664, name##_show, name##_store);

E404_ATTR_RW(kgsl_skip_zeroing);
E404_ATTR_RW(file_sync);
E404_ATTR_RW(avoid_dirty_pte);

static struct attribute *e404_attrs[] = {
    &kgsl_skip_zeroing_attr.attr,
    &file_sync_attr.attr,
    &avoid_dirty_pte_attr.attr,
    &bg_blocklist_attr.attr,
    NULL,
};

static struct attribute_group e404_group = {
    .attrs = e404_attrs,
};

static int __init e404_init(void) {
    int ret;
    char tmp[E404_BLOCKLIST_STRLEN];

    if (e404_data.bg_blocklist[0]) {
        strscpy(tmp, e404_data.bg_blocklist, sizeof(tmp));
        e404_rebuild_blocklist(tmp);
    }

    e404_kobj = kobject_create_and_add("e404", kernel_kobj);
    if (!e404_kobj)
        return -ENOMEM;

    ret = sysfs_create_group(e404_kobj, &e404_group);
    if (ret)
        goto fail_kobj;

    pr_alert("E404: Helper Init !\n");
    return 0;

fail_kobj:
    kobject_put(e404_kobj);
    return ret;
}

static void __exit e404_exit(void) {
    sysfs_remove_group(e404_kobj, &e404_group);
    kobject_put(e404_kobj);
    pr_alert("E404: Helper Exit !\n");
}

core_initcall(e404_init);
module_exit(e404_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kvsnr113");
MODULE_DESCRIPTION("E404 kernel helper for features & stuff");
MODULE_VERSION("1.6");
