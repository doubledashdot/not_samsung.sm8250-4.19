#ifndef _E404_ATTRIBUTES_H
#define _E404_ATTRIBUTES_H

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/sched.h>

#define E404_BLOCKLIST_STRLEN 256
#define E404_MAX_BLOCKED 16

bool e404_comm_blocked(const char *comm);

struct e404_attributes {
    int kgsl_skip_zeroing;
    int file_sync;
    int avoid_dirty_pte;
    char bg_blocklist[E404_BLOCKLIST_STRLEN];
};

extern struct e404_attributes e404_data;

#endif /* _E404_ATTRIBUTES_H */
