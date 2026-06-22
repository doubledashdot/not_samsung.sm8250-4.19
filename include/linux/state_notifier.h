/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_STATE_NOTIFIER_H
#define _LINUX_STATE_NOTIFIER_H

#include <linux/notifier.h>

#define STATE_NOTIFIER_ACTIVE   0
#define STATE_NOTIFIER_SUSPEND  1

extern bool state_suspended;

#ifdef CONFIG_STATE_NOTIFIER
extern void state_notifier_call_chain(unsigned long val);
#else
static inline void state_notifier_call_chain(unsigned long val) {}
#endif

#endif /* _LINUX_STATE_NOTIFIER_H */
