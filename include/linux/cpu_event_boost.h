/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2018-2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 */
#ifndef _CPU_EVENT_BOOST_H_
#define _CPU_EVENT_BOOST_H_

#ifdef CONFIG_CPU_EVENT_BOOST
void cpu_event_boost_kick_max(unsigned int duration_ms);
#else
static inline void cpu_event_boost_kick_max(unsigned int duration_ms)
{
}
#endif

#endif /* _CPU_EVENT_BOOST_H_ */
