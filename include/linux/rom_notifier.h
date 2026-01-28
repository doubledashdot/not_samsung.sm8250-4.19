// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Samuel Pascua <pascua.samuel.14@gmail.com>.
 */

/*
 * aosp = aosp
 * sunny = OneUI with the new usb notify sysfs naming:
 
 static const char *const LOCK_STATE_NAMES[] = {
    "SUNNY_WORK_MODE",     // 0: USB_NOTIFY_UNLOCK
    "CLOUDY_WORK_MODE",    // 1: USB_NOTIFY_LOCK_USB_WORK
    "RAINY_RESTRICT_MODE",     // 2: USB_NOTIFY_LOCK_USB_RESTRICT
    "SKY_DEFAULT"        // 3: default
};
 
 * bpf = rom requires 5.10 uname (bpf)
 * bpfl = rom requires 4.19 uname (bpf legacy)
 */

#include <linux/cache.h>
#include <linux/types.h>

extern bool is_aosp __read_mostly;
extern bool is_sunny __read_mostly;
extern bool is_bpf __read_mostly;
extern bool is_bpfl __read_mostly;
