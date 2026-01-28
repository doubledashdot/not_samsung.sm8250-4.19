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
 
#include <linux/init.h>
#include <linux/rom_notifier.h>
#include <linux/string.h>

bool is_aosp __read_mostly = false;
static int __init parse_aosp(char *str)
{
	if (!strncmp(str, "1", 1))
		is_aosp = true;

	return 0;
}
__setup("android.is_aosp=", parse_aosp);

bool is_sunny __read_mostly = false;
static int __init parse_sunny(char *str)
{
	if (!strncmp(str, "1", 1))
		is_sunny = true;

	return 0;
}
__setup("android.is_sunny=", parse_sunny);

bool is_bpf __read_mostly = false;
static int __init parse_bpf(char *str)
{
	if (!strncmp(str, "1", 1))
		is_bpf = true;

	return 0;
}
__setup("android.is_bpf=", parse_bpf);

bool is_bpfl __read_mostly = false;
static int __init parse_bpfl(char *str)
{
	if (!strncmp(str, "1", 1))
		is_bpfl = true;

	return 0;
}
__setup("android.is_bpfl=", parse_bpfl);
