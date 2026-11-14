/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2011-2014, 2016, 2018, The Linux Foundation. All rights reserved.
 */

#ifndef _ASM_ARCH_MSM_RESTART_H_
#define _ASM_ARCH_MSM_RESTART_H_

#include <linux/types.h>

#define RESTART_NORMAL 0x0
#define RESTART_DLOAD  0x1

/*
 * Crash reason tags read back by the boot loader's crash dump screen,
 * stored little-endian as four ASCII characters.
 */
#define CRASH_REASON_PANIC	0x43494e50	/* "PNIC" */
#define CRASH_REASON_WDT_BARK	0x4b524257	/* "WBRK" */
#define CRASH_REASON_USER_PANIC	0x41505355	/* "USPA" */
#define CRASH_REASON_CMD_REBOOT	0x42444d43	/* "CMDB" */

void msm_set_restart_mode(int mode);
extern int pmic_reset_irq;

#if IS_ENABLED(CONFIG_POWER_RESET_MSM) || IS_ENABLED(CONFIG_POWER_RESET_QCOM)
void msm_set_crash_reason(u32 reason);
#else
static inline void msm_set_crash_reason(u32 reason) { }
#endif

#endif

