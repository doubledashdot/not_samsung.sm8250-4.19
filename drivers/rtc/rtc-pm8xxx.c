// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2010-2011, 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/of.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/rtc.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <asm/unaligned.h>
#ifdef CONFIG_RTC_AUTO_PWRON
#include <linux/reboot.h>
#include <linux/wakelock.h>
#include <linux/alarmtimer.h>
#include <linux/time.h>
#ifdef CONFIG_RTC_AUTO_PWRON_PARAM
#include <linux/sec_param.h>

#define SAPA_KPARAM_MAGIC	0x41504153
extern unsigned int sapa_param_time;
#endif
#define SAPA_START_POLL_TIME   (10LL * NSEC_PER_SEC) /* 10 sec */
#define SAPA_BOOTING_TIME      (5*60)
#define SAPA_POLL_TIME         (15*60)

enum {
	SAPA_DISTANT = 0,
	SAPA_NEAR,
	SAPA_EXPIRED,
	SAPA_OVER
};

#define TO_SECS(arr)        (arr[0] | (arr[1] << 8) | (arr[2] << 16) | \
                            (arr[3] << 24))

extern unsigned int lpcharge;
#endif

/* RTC Register offsets from RTC CTRL REG */
#define PM8XXX_ALARM_CTRL_OFFSET	0x01
#define PM8XXX_RTC_WRITE_OFFSET		0x02
#define PM8XXX_RTC_READ_OFFSET		0x06
#define PM8XXX_ALARM_RW_OFFSET		0x0A

/* RTC_CTRL register bit fields */
#define PM8xxx_RTC_ENABLE		BIT(7)
#define PM8xxx_RTC_ALARM_CLEAR		BIT(0)
#define PM8xxx_RTC_ALARM_ENABLE		BIT(7)
#define NUM_8_BIT_RTC_REGS		0x4

/**
 * struct pm8xxx_rtc_regs - describe RTC registers per PMIC versions
 * @ctrl: base address of control register
 * @write: base address of write register
 * @read: base address of read register
 * @alarm_ctrl: base address of alarm control register
 * @alarm_ctrl2: base address of alarm control2 register
 * @alarm_rw: base address of alarm read-write register
 * @alarm_en: alarm enable mask
 */
struct pm8xxx_rtc_regs {
	unsigned int ctrl;
	unsigned int write;
	unsigned int read;
	unsigned int alarm_ctrl;
	unsigned int alarm_ctrl2;
	unsigned int alarm_rw;
	unsigned int alarm_en;
};

/**
 * struct pm8xxx_rtc -  rtc driver internal structure
 * @rtc:		rtc device for this driver.
 * @regmap:		regmap used to access RTC registers
 * @allow_set_time:	indicates whether writing to the RTC is allowed
 * @rtc_alarm_irq:	rtc alarm irq number.
 * @ctrl_reg:		rtc control register.
 * @rtc_dev:		device structure.
 */
struct pm8xxx_rtc {
	struct rtc_device *rtc;
	struct regmap *regmap;
	bool allow_set_time;
	int rtc_alarm_irq;
	const struct pm8xxx_rtc_regs *regs;
	struct device *rtc_dev;
#ifdef CONFIG_RTC_AUTO_PWRON
	struct rtc_wkalrm   sapa;
	struct alarm        check_poll;
	struct work_struct  check_func;
	struct wake_lock    wakelock;
	int                 lpm_mode;
	unsigned char       triggered;
#endif
};

static int pm8xxx_rtc_read_rtc_data(struct pm8xxx_rtc *rtc_dd, unsigned long *rtc_data)
{
	int rc;
	u8 value[NUM_8_BIT_RTC_REGS];
	unsigned int reg;
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;

	rc = regmap_bulk_read(rtc_dd->regmap, regs->read, value, sizeof(value));
	if (rc)
		return rc;

	/*
	 * Read the LSB again and check if there has been a carry over.
	 * If there is, redo the read operation.
	 */
	rc = regmap_read(rtc_dd->regmap, regs->read, &reg);
	if (rc < 0)
		return rc;

	if (unlikely(reg < value[0])) {
		rc = regmap_bulk_read(rtc_dd->regmap, regs->read,
				      value, sizeof(value));
		if (rc)
			return rc;
	}

	*rtc_data = get_unaligned_le32(value);

	return 0;
}

static int pm8xxx_rtc_read_alarm_data(struct pm8xxx_rtc *rtc_dd, unsigned long *alarm_data)
{
	int rc;
	u8 value[NUM_8_BIT_RTC_REGS];

	rc = regmap_bulk_read(rtc_dd->regmap, rtc_dd->regs->alarm_rw, value,
			      sizeof(value));
	if (rc)
		return rc;

	*alarm_data = get_unaligned_le32(value);

	return 0;
}

/*
 * Steps to write the RTC registers.
 * 1. Disable alarm if enabled.
 * 2. Disable rtc if enabled.
 * 3. Write 0x00 to LSB.
 * 4. Write Byte[1], Byte[2], Byte[3] then Byte[0].
 * 5. Enable rtc if disabled in step 2.
 * 6. Enable alarm if disabled in step 1.
 */
static int pm8xxx_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;
	u8 value[NUM_8_BIT_RTC_REGS];
	bool alarm_enabled;
	unsigned long secs;
	int rc;

	if (!rtc_dd->allow_set_time)
		return -EACCES;

	secs = rtc_tm_to_time64(tm);
	put_unaligned_le32(secs, value);

	dev_dbg(dev, "Seconds value to be written to RTC = %lu\n", secs);

	rc = regmap_update_bits_check(rtc_dd->regmap, regs->alarm_ctrl,
				      regs->alarm_en, 0, &alarm_enabled);
	if (rc)
		return rc;

	/* Disable RTC H/w before writing on RTC register */
	rc = regmap_update_bits(rtc_dd->regmap, regs->ctrl, PM8xxx_RTC_ENABLE, 0);
	if (rc)
		return rc;

	/* Write 0 to Byte[0] */
	rc = regmap_write(rtc_dd->regmap, regs->write, 0);
	if (rc)
		return rc;

	/* Write Byte[1], Byte[2], Byte[3] */
	rc = regmap_bulk_write(rtc_dd->regmap, regs->write + 1,
			       &value[1], sizeof(value) - 1);
	if (rc)
		return rc;

	/* Write Byte[0] */
	rc = regmap_write(rtc_dd->regmap, regs->write, value[0]);
	if (rc)
		return rc;

	/* Enable RTC H/w after writing on RTC register */
	rc = regmap_update_bits(rtc_dd->regmap, regs->ctrl, PM8xxx_RTC_ENABLE,
				PM8xxx_RTC_ENABLE);
	if (rc)
		return rc;

	if (alarm_enabled) {
		rc = regmap_update_bits(rtc_dd->regmap, regs->alarm_ctrl,
					regs->alarm_en, regs->alarm_en);
		if (rc)
			return rc;
	}

#ifdef CONFIG_RTC_AUTO_PWRON
	pr_info("%s : secs = %lu, h:m:s == %d:%d:%d, d/m/y = %d/%d/%d\n", __func__,
			secs, tm->tm_hour, tm->tm_min, tm->tm_sec,
			tm->tm_mday, tm->tm_mon, tm->tm_year);
#endif
	
	return 0;
}

static int pm8xxx_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	int rc;
	unsigned long secs;
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);

	rc = pm8xxx_rtc_read_rtc_data(rtc_dd, &secs);
	if (rc)
		return rc;

	rtc_time64_to_tm(secs, tm);

	dev_dbg(dev, "secs = %lu, h:m:s == %ptRt, y-m-d = %ptRdr\n", secs, tm, tm);

	return 0;
}

static int pm8xxx_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	u8 value[NUM_8_BIT_RTC_REGS];
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;
	unsigned long secs;
	int rc;
	secs = rtc_tm_to_time64(&alarm->time);

	put_unaligned_le32(secs, value);

	rc = regmap_update_bits(rtc_dd->regmap, regs->alarm_ctrl,
				regs->alarm_en, 0);
	if (rc)
		return rc;

	rc = regmap_bulk_write(rtc_dd->regmap, regs->alarm_rw, value,
			       sizeof(value));
	if (rc)
		return rc;

	if (alarm->enabled) {
		rc = regmap_update_bits(rtc_dd->regmap, regs->alarm_ctrl,
					regs->alarm_en, regs->alarm_en);
		if (rc)
			return rc;
	}

	dev_dbg(dev, "Alarm Set for h:m:s=%ptRt, y-m-d=%ptRdr\n",
		&alarm->time, &alarm->time);

	return 0;
}

static int pm8xxx_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	int rc;
	unsigned int ctrl_reg;
	unsigned long secs;
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;

	rc = pm8xxx_rtc_read_alarm_data(rtc_dd, &secs);
	if (rc)
		return rc;

	rtc_time64_to_tm(secs, &alarm->time);

	rc = regmap_read(rtc_dd->regmap, regs->alarm_ctrl, &ctrl_reg);
	if (rc)
		return rc;

	alarm->enabled = !!(ctrl_reg & PM8xxx_RTC_ALARM_ENABLE);

	dev_dbg(dev, "Alarm set for - h:m:s=%ptRt, y-m-d=%ptRdr\n",
		&alarm->time, &alarm->time);

	return 0;
}

static int pm8xxx_rtc_alarm_irq_enable(struct device *dev, unsigned int enable)
{
	int rc;
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;
	u8 value[NUM_8_BIT_RTC_REGS] = {0};
	unsigned int val;

#ifdef CONFIG_RTC_AUTO_PWRON
	pr_info("[SAPA] Alarm irq=%d\n", enable);
#endif
	
	if (enable)
		val = regs->alarm_en;
	else
		val = 0;

	rc = regmap_update_bits(rtc_dd->regmap, regs->alarm_ctrl,
				regs->alarm_en, val);
	if (rc)
		return rc;

	/* Clear Alarm register */
	if (!enable) {
		rc = regmap_bulk_write(rtc_dd->regmap, regs->alarm_rw, value,
					sizeof(value));
		if (rc)
			return rc;
	}

	return 0;
}

#ifdef CONFIG_RTC_AUTO_PWRON
static void
sapa_normalize_alarm(struct rtc_wkalrm *alarm)
{
	if (!alarm->enabled) {
		/* 50 years after RTC reset = 1580518864 = 0x5e34cdd0 */
		alarm->time.tm_year = 70 + 50;
		alarm->time.tm_mon = 1;
		alarm->time.tm_mday = 1;
		alarm->time.tm_hour = 1;
		alarm->time.tm_min = 1;
		alarm->time.tm_sec = 4;
	}
}

#ifdef CONFIG_RTC_AUTO_PWRON_PARAM
static void
sapa_save_kparam(struct pm8xxx_rtc *rtc_dd)
{
	unsigned long secs_pwron;
	unsigned int sapa[3];
	int rc;

	sapa_normalize_alarm(&rtc_dd->sapa);
	rtc_tm_to_time(&rtc_dd->sapa.time, &secs_pwron);
	sapa[0] = SAPA_KPARAM_MAGIC;
	sapa[1] = (unsigned int)rtc_dd->sapa.enabled;
	sapa[2] = (unsigned int)secs_pwron;

	rc = sec_set_param(param_index_sapa, sapa);
	pr_info("sapa: %s rc=%d, enabled=%d, alarm=%u\n",
		__func__, rc, sapa[1], sapa[2]);
}

#endif

static int
sapa_rtc_getalarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);

	alarm->enabled = rtc_dd->triggered;
	return 1;
}

static int
sapa_rtc_setalarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);

	memcpy(&rtc_dd->sapa, alarm, sizeof(struct rtc_wkalrm));
#ifdef CONFIG_RTC_AUTO_PWRON_PARAM
	sapa_save_kparam(rtc_dd);
#endif

	return 0;
}

static int
sapa_check_state(struct pm8xxx_rtc *rtc_dd, unsigned long *data)
{
	unsigned long rtc_secs;
	unsigned long secs_pwron;
	u8 value[NUM_8_BIT_RTC_REGS];
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;
	int rc;
	int res = SAPA_NEAR;

	rc = regmap_bulk_read(rtc_dd->regmap, regs->read, value, sizeof(value));
	if (rc)
		pr_err("%s: rtc read failed.\n", __func__);
	rtc_secs = TO_SECS(value);

	rtc_tm_to_time(&rtc_dd->sapa.time, &secs_pwron);

	if (rtc_secs < secs_pwron) {
		if (secs_pwron - rtc_secs > SAPA_POLL_TIME)
			res = SAPA_DISTANT;
		if (data)
			*data = secs_pwron - rtc_secs;
	} else if (rtc_secs <= secs_pwron+SAPA_BOOTING_TIME) {
		res = SAPA_EXPIRED;
		if (data)
			*data = rtc_secs + 10;
	} else
		res = SAPA_OVER;

	pr_info("%s: rtc:%lu, alrm:%lu[%d]\n", __func__, rtc_secs, secs_pwron, res);
	return res;
}

static void
sapa_check_func(struct work_struct *work)
{
	struct pm8xxx_rtc *rtc_dd = container_of(work, struct pm8xxx_rtc, check_func);
	int res;
	unsigned long remain;

	res = sapa_check_state(rtc_dd, &remain);
	if (res <=  SAPA_NEAR) {
		ktime_t kt;

		if (res == SAPA_DISTANT)
			remain = SAPA_POLL_TIME;
		kt = ns_to_ktime((u64)remain * NSEC_PER_SEC);
		alarm_start_relative(&rtc_dd->check_poll, kt);
		pr_info("%s: next %lu s\n", __func__, remain);
	} else if (res == SAPA_EXPIRED) {
		wake_lock(&rtc_dd->wakelock);
		rtc_dd->triggered = 1;
	}
}

static enum alarmtimer_restart
sapa_check_callback(struct alarm *alarm, ktime_t now)
{
	struct pm8xxx_rtc *rtc_dd = container_of(alarm, struct pm8xxx_rtc, check_poll);

	schedule_work(&rtc_dd->check_func);
	return ALARMTIMER_NORESTART;
}

static void
sapa_load_alarm(struct pm8xxx_rtc *rtc_dd, u8 ctrl_reg)
{
	unsigned long alarm_secs;
	u8 value[NUM_8_BIT_RTC_REGS];
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;
	int rc;

	rc = regmap_bulk_read(rtc_dd->regmap, regs->alarm_ctrl, value, sizeof(value));
	if (rc) {
		pr_err("%s: alarm read failed\n", __func__);
		return;
	}
	alarm_secs = TO_SECS(value);

#ifdef CONFIG_RTC_AUTO_PWRON_PARAM
	pr_info("%s: param=%u\n", __func__, sapa_param_time);
	rtc_time_to_tm(sapa_param_time, &rtc_dd->sapa.time);
	rtc_dd->sapa.enabled = (sapa_param_time) ? 1 : 0;
#else
	rtc_time_to_tm(alarm_secs, &rtc_dd->sapa.time);
	rtc_dd->sapa.enabled = (ctrl_reg & BIT_RTC_ALARM_ENABLE) ? 1 : 0;
#endif

	pr_info("%s: alarm_reg=%02x, pmic=%lu\n", __func__, ctrl_reg, alarm_secs);
}

static void
sapa_init(struct pm8xxx_rtc *rtc_dd)
{
	ktime_t kt;

	rtc_dd->lpm_mode = lpcharge;
	rtc_dd->triggered = 0;
	
	if (rtc_dd->lpm_mode && rtc_dd->sapa.enabled) {
		wake_lock_init(&rtc_dd->wakelock, WAKE_LOCK_SUSPEND, "SAPA");

		alarm_init(&rtc_dd->check_poll, ALARM_REALTIME, sapa_check_callback);
		INIT_WORK(&rtc_dd->check_func, sapa_check_func);

		kt = ns_to_ktime(SAPA_START_POLL_TIME);
		alarm_start_relative(&rtc_dd->check_poll, kt);
	}
}

#endif /*CONFIG_RTC_AUTO_PWRON*/

static const struct rtc_class_ops pm8xxx_rtc_ops = {
	.read_time	= pm8xxx_rtc_read_time,
	.set_time	= pm8xxx_rtc_set_time,
	.set_alarm	= pm8xxx_rtc_set_alarm,
	.read_alarm	= pm8xxx_rtc_read_alarm,
#ifdef CONFIG_RTC_AUTO_PWRON
	.read_bootalarm = sapa_rtc_getalarm,
	.set_bootalarm  = sapa_rtc_setalarm,
#endif /*CONFIG_RTC_AUTO_PWRON*/
	.alarm_irq_enable = pm8xxx_rtc_alarm_irq_enable,
};

static irqreturn_t pm8xxx_alarm_trigger(int irq, void *dev_id)
{
	struct pm8xxx_rtc *rtc_dd = dev_id;
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;
	int rc;

	rtc_update_irq(rtc_dd->rtc, 1, RTC_IRQF | RTC_AF);

	/* Clear the alarm enable bit */
	rc = regmap_update_bits(rtc_dd->regmap, regs->alarm_ctrl,
				regs->alarm_en, 0);
	if (rc)
		return IRQ_NONE;

	/* Clear RTC alarm register */
	rc = regmap_update_bits(rtc_dd->regmap, regs->alarm_ctrl2,
				PM8xxx_RTC_ALARM_CLEAR, 0);
	if (rc)
		return IRQ_NONE;

	return IRQ_HANDLED;
}

/*
 * Trigger the alarm event and clear the alarm settings
 * if the alarm data has been behind the RTC data which
 * means the alarm has been triggered before the driver
 * is probed.
 */
static int pm8xxx_rtc_init_alarm(struct pm8xxx_rtc *rtc_dd)
{
	int rc;
	unsigned long rtc_data, alarm_data;
	unsigned int ctrl_reg, alarm_en;
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;

	rc = pm8xxx_rtc_read_rtc_data(rtc_dd, &rtc_data);
	if (rc)
		return rc;

	rc = pm8xxx_rtc_read_alarm_data(rtc_dd, &alarm_data);
	if (rc)
		return rc;

	rc = regmap_read(rtc_dd->regmap, regs->alarm_ctrl, &ctrl_reg);
	if (rc)
		return rc;

	alarm_en = !!(ctrl_reg & PM8xxx_RTC_ALARM_ENABLE);

	if (alarm_en && rtc_data >= alarm_data)
		pm8xxx_alarm_trigger(0, rtc_dd);

	return 0;
}

static int pm8xxx_rtc_enable(struct pm8xxx_rtc *rtc_dd)
{
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;

	return regmap_update_bits(rtc_dd->regmap, regs->ctrl, PM8xxx_RTC_ENABLE,
				  PM8xxx_RTC_ENABLE);
}

static const struct pm8xxx_rtc_regs pm8921_regs = {
	.ctrl		= 0x11d,
	.write		= 0x11f,
	.read		= 0x123,
	.alarm_rw	= 0x127,
	.alarm_ctrl	= 0x11d,
	.alarm_ctrl2	= 0x11e,
	.alarm_en	= BIT(1),
};

static const struct pm8xxx_rtc_regs pm8058_regs = {
	.ctrl		= 0x1e8,
	.write		= 0x1ea,
	.read		= 0x1ee,
	.alarm_rw	= 0x1f2,
	.alarm_ctrl	= 0x1e8,
	.alarm_ctrl2	= 0x1e9,
	.alarm_en	= BIT(1),
};

static const struct pm8xxx_rtc_regs pm8941_regs = {
	.ctrl		= 0x6046,
	.write		= 0x6040,
	.read		= 0x6048,
	.alarm_rw	= 0x6140,
	.alarm_ctrl	= 0x6146,
	.alarm_ctrl2	= 0x6148,
	.alarm_en	= BIT(7),
};

static const struct pm8xxx_rtc_regs pmk8350_regs = {
	.ctrl		= 0x6146,
	.write		= 0x6140,
	.read		= 0x6148,
	.alarm_rw	= 0x6240,
	.alarm_ctrl	= 0x6246,
	.alarm_ctrl2	= 0x6248,
	.alarm_en	= BIT(7),
};

static const struct pm8xxx_rtc_regs pm8916_regs = {
	.ctrl		= 0x6046,
	.write		= 0x6040,
	.read		= 0x6048,
	.alarm_rw	= 0x6140,
	.alarm_ctrl	= 0x6146,
	.alarm_ctrl2	= 0x6148,
	.alarm_en	= BIT(7),
};

/*
 * Hardcoded RTC bases until IORESOURCE_REG mapping is figured out
 */
static const struct of_device_id pm8xxx_id_table[] = {
	{ .compatible = "qcom,pm8921-rtc", .data = &pm8921_regs },
	{ .compatible = "qcom,pm8018-rtc", .data = &pm8921_regs },
	{ .compatible = "qcom,pm8058-rtc", .data = &pm8058_regs },
	{ .compatible = "qcom,pm8941-rtc", .data = &pm8941_regs },
	{ .compatible = "qcom,pmk8350-rtc", .data = &pmk8350_regs },
	{ .compatible = "qcom,pm8916-rtc", .data = &pm8916_regs },
	{ },
};
MODULE_DEVICE_TABLE(of, pm8xxx_id_table);

static int pm8xxx_rtc_probe(struct platform_device *pdev)
{
	int rc;
	struct pm8xxx_rtc *rtc_dd;
	const struct of_device_id *match;

	match = of_match_node(pm8xxx_id_table, pdev->dev.of_node);
	if (!match)
		return -ENXIO;

	rtc_dd = devm_kzalloc(&pdev->dev, sizeof(*rtc_dd), GFP_KERNEL);
	if (rtc_dd == NULL)
		return -ENOMEM;

	rtc_dd->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!rtc_dd->regmap)
		return -ENXIO;

	rtc_dd->rtc_alarm_irq = platform_get_irq(pdev, 0);
	if (rtc_dd->rtc_alarm_irq < 0)
		return -ENXIO;

	rtc_dd->allow_set_time = of_property_read_bool(pdev->dev.of_node,
						      "allow-set-time");

	rtc_dd->regs = match->data;
	rtc_dd->rtc_dev = &pdev->dev;

	rc = pm8xxx_rtc_enable(rtc_dd);
	if (rc)
		return rc;

	platform_set_drvdata(pdev, rtc_dd);

	device_init_wakeup(&pdev->dev, 1);

#ifdef CONFIG_RTC_AUTO_PWRON
	sapa_load_alarm(rtc_dd, rtc_dd->regs->alarm_ctrl);
#endif
	
	/* Register the RTC device */
	rtc_dd->rtc = devm_rtc_allocate_device(&pdev->dev);
	if (IS_ERR(rtc_dd->rtc))
		return PTR_ERR(rtc_dd->rtc);

	rtc_dd->rtc->ops = &pm8xxx_rtc_ops;
	rtc_dd->rtc->range_max = U32_MAX;

	/* Request the alarm IRQ */
	rc = devm_request_any_context_irq(&pdev->dev, rtc_dd->rtc_alarm_irq,
					  pm8xxx_alarm_trigger,
					  IRQF_TRIGGER_RISING,
					  "pm8xxx_rtc_alarm", rtc_dd);
	if (rc < 0)
		return rc;

#ifdef CONFIG_RTC_AUTO_PWRON
	sapa_init(rtc_dd);
#endif
	
	rc =  rtc_register_device(rtc_dd->rtc);
	if (rc < 0)
		return rc;

	return pm8xxx_rtc_init_alarm(rtc_dd);
}

#ifdef CONFIG_PM_SLEEP
static int pm8xxx_rtc_restore(struct device *dev)
{
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);
	int rc;

	/* Request the alarm IRQ */
	rc = devm_request_any_context_irq(rtc_dd->rtc_dev,
					  rtc_dd->rtc_alarm_irq,
					  pm8xxx_alarm_trigger,
					  IRQF_TRIGGER_RISING,
					  "pm8xxx_rtc_alarm", rtc_dd);
	if (rc < 0) {
		return rc;
	}

	return pm8xxx_rtc_enable(rtc_dd);
}

static int pm8xxx_rtc_freeze(struct device *dev)
{
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);

	devm_free_irq(rtc_dd->rtc_dev, rtc_dd->rtc_alarm_irq, rtc_dd);

	return 0;
}
#endif

static int pm8xxx_rtc_resume(struct device *dev)
{
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		disable_irq_wake(rtc_dd->rtc_alarm_irq);

	return 0;
}

static int pm8xxx_rtc_suspend(struct device *dev)
{
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		enable_irq_wake(rtc_dd->rtc_alarm_irq);

	return 0;
}

static const struct dev_pm_ops pm8xxx_rtc_pm_ops = {
	.freeze = pm8xxx_rtc_freeze,
	.restore = pm8xxx_rtc_restore,
	.suspend = pm8xxx_rtc_suspend,
	.resume = pm8xxx_rtc_resume,
};

static void pm8xxx_rtc_shutdown(struct platform_device *pdev)
{
	struct pm8xxx_rtc *rtc_dd = platform_get_drvdata(pdev);

	devm_free_irq(rtc_dd->rtc_dev, rtc_dd->rtc_alarm_irq, rtc_dd);
}

static struct platform_driver pm8xxx_rtc_driver = {
	.probe		= pm8xxx_rtc_probe,
	.driver	= {
		.name		= "rtc-pm8xxx",
		.pm		= &pm8xxx_rtc_pm_ops,
		.of_match_table	= pm8xxx_id_table,
	},
	.shutdown	= pm8xxx_rtc_shutdown,
};

module_platform_driver(pm8xxx_rtc_driver);

MODULE_ALIAS("platform:rtc-pm8xxx");
MODULE_DESCRIPTION("PMIC8xxx RTC driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Anirudh Ghayal <aghayal@codeaurora.org>");
