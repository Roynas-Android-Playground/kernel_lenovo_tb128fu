/*
 * BQ2589x battery charging driver
 *
 * Copyright (C) 2013 Texas Instruments
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/hardware_info.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/of_gpio.h>
#include "bq2589x_reg.h"
#include "bq2589x_charger.h"

static struct bq2589x *g_bq;

static DEFINE_MUTEX(bq2589x_i2c_lock);

static int bq2589x_read_byte(struct bq2589x *bq, u8 *data, u8 reg)
{
	int ret;

	mutex_lock(&bq2589x_i2c_lock);
	ret = i2c_smbus_read_byte_data(bq->client, reg);
	if (ret < 0) {
		dev_err(bq->dev, "failed to read 0x%.2x\n", reg);
		mutex_unlock(&bq2589x_i2c_lock);
		return ret;
	}

	*data = (u8)ret;
	mutex_unlock(&bq2589x_i2c_lock);

	return 0;
}

static int bq2589x_write_byte(struct bq2589x *bq, u8 reg, u8 data)
{
	int ret;
	mutex_lock(&bq2589x_i2c_lock);
	ret = i2c_smbus_write_byte_data(bq->client, reg, data);
	mutex_unlock(&bq2589x_i2c_lock);
	return ret;
}

static int bq2589x_update_bits(struct bq2589x *bq, u8 reg, u8 mask, u8 data)
{
	int ret;
	u8 tmp;

	ret = bq2589x_read_byte(bq, &tmp, reg);

	if (ret)
		return ret;

	tmp &= ~mask;
	tmp |= data & mask;

	return bq2589x_write_byte(bq, reg, tmp);
}


static enum bq2589x_vbus_type bq2589x_get_vbus_type(struct bq2589x *bq)
{
	u8 val = 0;
	int ret;

	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_0B);
	if (ret < 0)
		return 0;
	val &= BQ2589X_VBUS_STAT_MASK;
	val >>= BQ2589X_VBUS_STAT_SHIFT;

	return val;
}
EXPORT_SYMBOL_GPL(bq2589x_get_vbus_type);

static int bq2589x_enable_otg(struct bq2589x *bq)
{
	u8 val = BQ2589X_OTG_ENABLE << BQ2589X_OTG_CONFIG_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_03,
							   BQ2589X_OTG_CONFIG_MASK, val);

}
EXPORT_SYMBOL_GPL(bq2589x_enable_otg);

static int bq2589x_disable_otg(struct bq2589x *bq)
{
	u8 val = BQ2589X_OTG_DISABLE << BQ2589X_OTG_CONFIG_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_03,
							   BQ2589X_OTG_CONFIG_MASK, val);

}
EXPORT_SYMBOL_GPL(bq2589x_disable_otg);

static int bq2589x_set_otg_volt(struct bq2589x *bq, int volt)
{
	u8 val = 0;

	if (volt < BQ2589X_BOOSTV_BASE)
		volt = BQ2589X_BOOSTV_BASE;
	if (volt > BQ2589X_BOOSTV_BASE + (BQ2589X_BOOSTV_MASK >> BQ2589X_BOOSTV_SHIFT) * BQ2589X_BOOSTV_LSB)
		volt = BQ2589X_BOOSTV_BASE + (BQ2589X_BOOSTV_MASK >> BQ2589X_BOOSTV_SHIFT) * BQ2589X_BOOSTV_LSB;

	val = ((volt - BQ2589X_BOOSTV_BASE) / BQ2589X_BOOSTV_LSB) << BQ2589X_BOOSTV_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_0A, BQ2589X_BOOSTV_MASK, val);

}
EXPORT_SYMBOL_GPL(bq2589x_set_otg_volt);

static int bq2589x_set_otg_current(struct bq2589x *bq, int curr)
{
	u8 temp;

	if (curr == 500)
		temp = BQ2589X_BOOST_LIM_500MA;
	else if (curr == 700)
		temp = BQ2589X_BOOST_LIM_700MA;
	else if (curr == 1100)
		temp = BQ2589X_BOOST_LIM_1100MA;
	else if (curr == 1600)
		temp = BQ2589X_BOOST_LIM_1600MA;
	else if (curr == 1800)
		temp = BQ2589X_BOOST_LIM_1800MA;
	else if (curr == 2100)
		temp = BQ2589X_BOOST_LIM_2100MA;
	else if (curr == 2400)
		temp = BQ2589X_BOOST_LIM_2400MA;
	else
		temp = BQ2589X_BOOST_LIM_1300MA;

	return bq2589x_update_bits(bq, BQ2589X_REG_0A, BQ2589X_BOOST_LIM_MASK, temp << BQ2589X_BOOST_LIM_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_set_otg_current);

static int bq2589x_enable_charger(struct bq2589x *bq)
{
	int ret;
	u8 val = BQ2589X_CHG_ENABLE << BQ2589X_CHG_CONFIG_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_03, BQ2589X_CHG_CONFIG_MASK, val);
	if (ret == 0)
		bq->status |= BQ2589X_STATUS_CHARGE_ENABLE;
	return ret;
}
EXPORT_SYMBOL_GPL(bq2589x_enable_charger);

static int bq2589x_disable_charger(struct bq2589x *bq)
{
	int ret;
	u8 val = BQ2589X_CHG_DISABLE << BQ2589X_CHG_CONFIG_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_03, BQ2589X_CHG_CONFIG_MASK, val);
	if (ret == 0)
		bq->status &= ~BQ2589X_STATUS_CHARGE_ENABLE;
	return ret;
}
EXPORT_SYMBOL_GPL(bq2589x_disable_charger);


/* interfaces that can be called by other module */
int bq2589x_adc_start(struct bq2589x *bq, bool oneshot)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_02);
	if (ret < 0) {
		dev_err(bq->dev, "%s failed to read register 0x02:%d\n", __func__, ret);
		return ret;
	}

	if (((val & BQ2589X_CONV_RATE_MASK) >> BQ2589X_CONV_RATE_SHIFT) == BQ2589X_ADC_CONTINUE_ENABLE)
		return 0; /*is doing continuous scan*/
	if (oneshot)
		ret = bq2589x_update_bits(bq, BQ2589X_REG_02, BQ2589X_CONV_START_MASK, BQ2589X_CONV_START << BQ2589X_CONV_START_SHIFT);
	else
		ret = bq2589x_update_bits(bq, BQ2589X_REG_02, BQ2589X_CONV_RATE_MASK,  BQ2589X_ADC_CONTINUE_ENABLE << BQ2589X_CONV_RATE_SHIFT);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2589x_adc_start);

int bq2589x_adc_stop(struct bq2589x *bq)
{
	return bq2589x_update_bits(bq, BQ2589X_REG_02, BQ2589X_CONV_RATE_MASK, BQ2589X_ADC_CONTINUE_DISABLE << BQ2589X_CONV_RATE_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_adc_stop);


int bq2589x_adc_read_battery_volt(struct bq2589x *bq)
{
	uint8_t val;
	int volt;
	int ret;
	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_0E);
	if (ret < 0) {
		dev_err(bq->dev, "read battery voltage failed :%d\n", ret);
		return ret;
	} else{
		volt = BQ2589X_BATV_BASE + ((val & BQ2589X_BATV_MASK) >> BQ2589X_BATV_SHIFT) * BQ2589X_BATV_LSB ;
		return volt;
	}
}
EXPORT_SYMBOL_GPL(bq2589x_adc_read_battery_volt);


int bq2589x_adc_read_sys_volt(struct bq2589x *bq)
{
	uint8_t val;
	int volt;
	int ret;
	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_0F);
	if (ret < 0) {
		dev_err(bq->dev, "read system voltage failed :%d\n", ret);
		return ret;
	} else{
		volt = BQ2589X_SYSV_BASE + ((val & BQ2589X_SYSV_MASK) >> BQ2589X_SYSV_SHIFT) * BQ2589X_SYSV_LSB ;
		return volt;
	}
}
EXPORT_SYMBOL_GPL(bq2589x_adc_read_sys_volt);

int bq2589x_adc_read_vbus_volt(struct bq2589x *bq)
{
	uint8_t val;
	int volt;
	int ret;
	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_11);
	if (ret < 0) {
		dev_err(bq->dev, "read vbus voltage failed :%d\n", ret);
		return ret;
	} else{
		volt = BQ2589X_VBUSV_BASE + ((val & BQ2589X_VBUSV_MASK) >> BQ2589X_VBUSV_SHIFT) * BQ2589X_VBUSV_LSB ;
		return volt;
	}
}
EXPORT_SYMBOL_GPL(bq2589x_adc_read_vbus_volt);

int bq2589x_adc_read_temperature(struct bq2589x *bq)
{
	uint8_t val;
	int temp;
	int ret;
	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_10);
	if (ret < 0) {
		dev_err(bq->dev, "read temperature failed :%d\n", ret);
		return ret;
	} else{
		temp = BQ2589X_TSPCT_BASE + ((val & BQ2589X_TSPCT_MASK) >> BQ2589X_TSPCT_SHIFT) * BQ2589X_TSPCT_LSB ;
		return temp;
	}
}
EXPORT_SYMBOL_GPL(bq2589x_adc_read_temperature);

int bq2589x_adc_read_charge_current(struct bq2589x *bq)
{
	uint8_t val;
	int volt;
	int ret;
	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_12);
	if (ret < 0) {
		dev_err(bq->dev, "read charge current failed :%d\n", ret);
		return ret;
	} else{
		volt = (int)(BQ2589X_ICHGR_BASE + ((val & BQ2589X_ICHGR_MASK) >> BQ2589X_ICHGR_SHIFT) * BQ2589X_ICHGR_LSB) ;
		return volt;
	}
}
EXPORT_SYMBOL_GPL(bq2589x_adc_read_charge_current);

int bq2589x_set_charge_current(struct bq2589x *bq, int curr)
{
	u8 ichg;

	ichg = (curr - BQ2589X_ICHG_BASE)/BQ2589X_ICHG_LSB;
	return bq2589x_update_bits(bq, BQ2589X_REG_04, BQ2589X_ICHG_MASK, ichg << BQ2589X_ICHG_SHIFT);

}
EXPORT_SYMBOL_GPL(bq2589x_set_charge_current);

int bq2589x_set_term_current(struct bq2589x *bq, int curr)
{
	u8 iterm;

	iterm = (curr - BQ2589X_ITERM_BASE) / BQ2589X_ITERM_LSB;

	return bq2589x_update_bits(bq, BQ2589X_REG_05, BQ2589X_ITERM_MASK, iterm << BQ2589X_ITERM_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_set_term_current);


int bq2589x_set_prechg_current(struct bq2589x *bq, int curr)
{
	u8 iprechg;

	iprechg = (curr - BQ2589X_IPRECHG_BASE) / BQ2589X_IPRECHG_LSB;

	return bq2589x_update_bits(bq, BQ2589X_REG_05, BQ2589X_IPRECHG_MASK, iprechg << BQ2589X_IPRECHG_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_set_prechg_current);

int bq2589x_set_chargevoltage(struct bq2589x *bq, int volt)
{
	u8 val;

	val = (volt - BQ2589X_VREG_BASE)/BQ2589X_VREG_LSB;
	return bq2589x_update_bits(bq, BQ2589X_REG_06, BQ2589X_VREG_MASK, val << BQ2589X_VREG_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_set_chargevoltage);


int bq2589x_set_input_volt_limit(struct bq2589x *bq, int volt)
{
	u8 val;
	val = (volt - BQ2589X_VINDPM_BASE) / BQ2589X_VINDPM_LSB;
	return bq2589x_update_bits(bq, BQ2589X_REG_0D, BQ2589X_VINDPM_MASK, val << BQ2589X_VINDPM_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_set_input_volt_limit);

int bq2589x_set_input_current_limit(struct bq2589x *bq, int curr)
{
	u8 val;

	val = (curr - BQ2589X_IINLIM_BASE) / BQ2589X_IINLIM_LSB;
	return bq2589x_update_bits(bq, BQ2589X_REG_00, BQ2589X_IINLIM_MASK, val << BQ2589X_IINLIM_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_set_input_current_limit);


int bq2589x_set_vindpm_offset(struct bq2589x *bq, int offset)
{
	u8 val;

	val = (offset - BQ2589X_VINDPMOS_BASE)/BQ2589X_VINDPMOS_LSB;
	return bq2589x_update_bits(bq, BQ2589X_REG_01, BQ2589X_VINDPMOS_MASK, val << BQ2589X_VINDPMOS_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_set_vindpm_offset);

int bq2589x_get_charging_status(struct bq2589x *bq)
{
	u8 val = 0;
	int ret;

	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_0B);
	if (ret < 0) {
		dev_err(bq->dev, "%s Failed to read register 0x0b:%d\n", __func__, ret);
		return ret;
	}
	val &= BQ2589X_CHRG_STAT_MASK;
	val >>= BQ2589X_CHRG_STAT_SHIFT;
	return val;
}
EXPORT_SYMBOL_GPL(bq2589x_get_charging_status);

void bq2589x_set_otg(struct bq2589x *bq, int enable)
{
	int ret;

	if (enable) {
		ret = bq2589x_enable_otg(bq);
		if (ret < 0) {
			dev_err(bq->dev, "%s:Failed to enable otg-%d\n", __func__, ret);
			return;
		}
	} else{
		ret = bq2589x_disable_otg(bq);
		if (ret < 0)
			dev_err(bq->dev, "%s:Failed to disable otg-%d\n", __func__, ret);
	}
}
EXPORT_SYMBOL_GPL(bq2589x_set_otg);

int bq2589x_set_watchdog_timer(struct bq2589x *bq, u8 timeout)
{
	return bq2589x_update_bits(bq, BQ2589X_REG_07, BQ2589X_WDT_MASK, (u8)((timeout - BQ2589X_WDT_BASE) / BQ2589X_WDT_LSB) << BQ2589X_WDT_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_set_watchdog_timer);

int bq2589x_disable_watchdog_timer(struct bq2589x *bq)
{
	u8 val = BQ2589X_WDT_DISABLE << BQ2589X_WDT_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_07, BQ2589X_WDT_MASK, val);
}
EXPORT_SYMBOL_GPL(bq2589x_disable_watchdog_timer);

int bq2589x_reset_watchdog_timer(struct bq2589x *bq)
{
	u8 val = BQ2589X_WDT_RESET << BQ2589X_WDT_RESET_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_03, BQ2589X_WDT_RESET_MASK, val);
}
EXPORT_SYMBOL_GPL(bq2589x_reset_watchdog_timer);

int bq2589x_force_dpdm(struct bq2589x *bq)
{
	int ret;
	u8 val = BQ2589X_FORCE_DPDM << BQ2589X_FORCE_DPDM_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_02, BQ2589X_FORCE_DPDM_MASK, val);
	if (ret)
		return ret;

	msleep(20);/*TODO: how much time needed to finish dpdm detect?*/
	return 0;

}
EXPORT_SYMBOL_GPL(bq2589x_force_dpdm);

int bq2589x_reset_chip(struct bq2589x *bq)
{
	int ret;
	u8 val = BQ2589X_RESET << BQ2589X_RESET_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_14, BQ2589X_RESET_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2589x_reset_chip);

int bq2589x_enter_ship_mode(struct bq2589x *bq)
{
	int ret;
	u8 val = BQ2589X_BATFET_OFF << BQ2589X_BATFET_DIS_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_09, BQ2589X_BATFET_DIS_MASK, val);
	return ret;

}
EXPORT_SYMBOL_GPL(bq2589x_enter_ship_mode);

int bq2589x_enter_hiz_mode(struct bq2589x *bq)
{
	u8 val = BQ2589X_HIZ_ENABLE << BQ2589X_ENHIZ_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_00, BQ2589X_ENHIZ_MASK, val);

}
EXPORT_SYMBOL_GPL(bq2589x_enter_hiz_mode);

int bq2589x_exit_hiz_mode(struct bq2589x *bq)
{

	u8 val = BQ2589X_HIZ_DISABLE << BQ2589X_ENHIZ_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_00, BQ2589X_ENHIZ_MASK, val);

}
EXPORT_SYMBOL_GPL(bq2589x_exit_hiz_mode);

int bq2589x_get_hiz_mode(struct bq2589x *bq, u8 *state)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_00);
	if (ret)
		return ret;
	*state = (val & BQ2589X_ENHIZ_MASK) >> BQ2589X_ENHIZ_SHIFT;

	return 0;
}
EXPORT_SYMBOL_GPL(bq2589x_get_hiz_mode);


int bq2589x_pumpx_enable(struct bq2589x *bq, int enable)
{
	u8 val;
	int ret;

	if (enable)
		val = BQ2589X_PUMPX_ENABLE << BQ2589X_EN_PUMPX_SHIFT;
	else
		val = BQ2589X_PUMPX_DISABLE << BQ2589X_EN_PUMPX_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_04, BQ2589X_EN_PUMPX_MASK, val);

	return ret;
}
EXPORT_SYMBOL_GPL(bq2589x_pumpx_enable);

int bq2589x_pumpx_increase_volt(struct bq2589x *bq)
{
	u8 val;
	int ret;

	val = BQ2589X_PUMPX_UP << BQ2589X_PUMPX_UP_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_09, BQ2589X_PUMPX_UP_MASK, val);

	return ret;

}
EXPORT_SYMBOL_GPL(bq2589x_pumpx_increase_volt);

int bq2589x_pumpx_increase_volt_done(struct bq2589x *bq)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_09);
	if (ret)
		return ret;

	if (val & BQ2589X_PUMPX_UP_MASK)
		return 1;   /* not finished*/
	else
		return 0;   /* pumpx up finished*/

}
EXPORT_SYMBOL_GPL(bq2589x_pumpx_increase_volt_done);

int bq2589x_pumpx_decrease_volt(struct bq2589x *bq)
{
	u8 val;
	int ret;

	val = BQ2589X_PUMPX_DOWN << BQ2589X_PUMPX_DOWN_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_09, BQ2589X_PUMPX_DOWN_MASK, val);

	return ret;

}
EXPORT_SYMBOL_GPL(bq2589x_pumpx_decrease_volt);

int bq2589x_pumpx_decrease_volt_done(struct bq2589x *bq)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_09);
	if (ret)
		return ret;

	if (val & BQ2589X_PUMPX_DOWN_MASK)
		return 1;   /* not finished*/
	else
		return 0;   /* pumpx down finished*/

}
EXPORT_SYMBOL_GPL(bq2589x_pumpx_decrease_volt_done);

static int bq2589x_force_ico(struct bq2589x *bq)
{
	u8 val;
	int ret;

	val = BQ2589X_FORCE_ICO << BQ2589X_FORCE_ICO_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_09, BQ2589X_FORCE_ICO_MASK, val);

	return ret;
}
EXPORT_SYMBOL_GPL(bq2589x_force_ico);

static int bq2589x_check_force_ico_done(struct bq2589x *bq)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_14);
	if (ret)
		return ret;

	if (val & BQ2589X_ICO_OPTIMIZED_MASK)
		return 1;  /*finished*/
	else
		return 0;   /* in progress*/
}
EXPORT_SYMBOL_GPL(bq2589x_check_force_ico_done);

static int bq2589x_enable_term(struct bq2589x* bq, bool enable)
{
	u8 val;
	int ret;

	if (enable)
		val = BQ2589X_TERM_ENABLE << BQ2589X_EN_TERM_SHIFT;
	else
		val = BQ2589X_TERM_DISABLE << BQ2589X_EN_TERM_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_07, BQ2589X_EN_TERM_MASK, val);

	return ret;
}
EXPORT_SYMBOL_GPL(bq2589x_enable_term);

static int bq2589x_enable_auto_dpdm(struct bq2589x* bq, bool enable)
{
	u8 val;
	int ret;

	if (enable)
		val = BQ2589X_AUTO_DPDM_ENABLE << BQ2589X_AUTO_DPDM_EN_SHIFT;
	else
		val = BQ2589X_AUTO_DPDM_DISABLE << BQ2589X_AUTO_DPDM_EN_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_02, BQ2589X_AUTO_DPDM_EN_MASK, val);

	return ret;

}
EXPORT_SYMBOL_GPL(bq2589x_enable_auto_dpdm);

static int bq2589x_use_absolute_vindpm(struct bq2589x* bq, bool enable)
{
	u8 val;
	int ret;

	if (enable)
		val = BQ2589X_FORCE_VINDPM_ENABLE << BQ2589X_FORCE_VINDPM_SHIFT;
	else
		val = BQ2589X_FORCE_VINDPM_DISABLE << BQ2589X_FORCE_VINDPM_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_0D, BQ2589X_FORCE_VINDPM_MASK, val);

	return ret;

}
EXPORT_SYMBOL_GPL(bq2589x_use_absolute_vindpm);

static int bq2589x_enable_ico(struct bq2589x* bq, bool enable)
{
	u8 val;
	int ret;

	if (enable)
		val = BQ2589X_ICO_ENABLE << BQ2589X_ICOEN_SHIFT;
	else
		val = BQ2589X_ICO_DISABLE << BQ2589X_ICOEN_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_02, BQ2589X_ICOEN_MASK, val);

	return ret;

}
EXPORT_SYMBOL_GPL(bq2589x_enable_ico);


static int bq2589x_read_idpm_limit(struct bq2589x *bq)
{
	uint8_t val;
	int curr;
	int ret;

	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_13);
	if (ret < 0) {
		dev_err(bq->dev, "read vbus voltage failed :%d\n", ret);
		return ret;
	} else{
		curr = BQ2589X_IDPM_LIM_BASE + ((val & BQ2589X_IDPM_LIM_MASK) >> BQ2589X_IDPM_LIM_SHIFT) * BQ2589X_IDPM_LIM_LSB ;
		return curr;
	}
}
EXPORT_SYMBOL_GPL(bq2589x_read_idpm_limit);

static bool bq2589x_is_charge_done(struct bq2589x *bq)
{
	int ret;
	u8 val;

	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_0B);
	if (ret < 0) {
		dev_err(bq->dev, "%s:read REG0B failed :%d\n", __func__, ret);
		return false;
	}
	val &= BQ2589X_CHRG_STAT_MASK;
	val >>= BQ2589X_CHRG_STAT_SHIFT;

	return (val == BQ2589X_CHRG_STAT_CHGDONE);
}
EXPORT_SYMBOL_GPL(bq2589x_is_charge_done);

static void bq2589x_dump_regs(struct bq2589x *bq)
{
	int addr, ret;
	u8 val;

	pr_err("bq2589x_dump_regs:");
	for (addr = 0x0; addr <= 0x14; addr++) {
		ret = bq2589x_read_byte(bq, &val, addr);
		if (ret == 0)
			pr_err("Reg[%.2x] = 0x%.2x ", addr, val);
	}
	pr_err("\n");

}
EXPORT_SYMBOL_GPL(bq2589x_dump_regs);

static int bq2589x_init_device(struct bq2589x *bq)
{
	int ret;

	/*common initialization*/
	bq2589x_disable_watchdog_timer(bq);

	bq2589x_enable_auto_dpdm(bq, bq->cfg.enable_auto_dpdm);
	bq2589x_enable_term(bq, bq->cfg.enable_term);
	bq2589x_enable_ico(bq, bq->cfg.enable_ico);
	/*force use absolute vindpm if auto_dpdm not enabled*/
	if (!bq->cfg.enable_auto_dpdm)
		bq->cfg.use_absolute_vindpm = true;

	bq2589x_use_absolute_vindpm(bq, bq->cfg.use_absolute_vindpm);

	ret = bq2589x_set_vindpm_offset(bq, 600);
	if (ret < 0) {
		dev_err(bq->dev, "%s:Failed to set vindpm offset:%d\n", __func__, ret);
		return ret;
	}

	ret = bq2589x_set_term_current(bq, bq->cfg.term_current);
	if (ret < 0) {
		dev_err(bq->dev, "%s:Failed to set termination current:%d\n", __func__, ret);
		return ret;
	}

	ret = bq2589x_set_chargevoltage(bq, bq->cfg.charge_voltage);
	if (ret < 0) {
		dev_err(bq->dev, "%s:Failed to set charge voltage:%d\n", __func__, ret);
		return ret;
	}

	ret = bq2589x_set_charge_current(bq, bq->cfg.charge_current);
	if (ret < 0) {
		dev_err(bq->dev, "%s:Failed to set charge current:%d\n", __func__, ret);
		return ret;
	}

	ret = bq2589x_enable_charger(bq);
	if (ret < 0) {
		dev_err(bq->dev, "%s:Failed to enable charger:%d\n", __func__, ret);
		return ret;
	}

	bq2589x_adc_start(bq, false);
	bq2589x_update_bits(bq, BQ2589X_REG_00, BQ2589X_ENILIM_MASK,
		BQ2589X_ENILIM_DISABLE << BQ2589X_ENILIM_SHIFT);
	bq2589x_update_bits(bq, BQ2589X_REG_02, 0x8, 1 << 3);
	if (bq->is_bq25890h)
		bq2589x_update_bits(bq, BQ2589X_REG_01, 0x2, 0 << 1);
	else
		bq2589x_update_bits(bq, BQ2589X_REG_02, 0x4, 0 << 2);
	return ret;
}


static int bq2589x_charge_status(struct bq2589x *bq)
{
	u8 val = 0;

	bq2589x_read_byte(bq, &val, BQ2589X_REG_0B);
	val &= BQ2589X_CHRG_STAT_MASK;
	val >>= BQ2589X_CHRG_STAT_SHIFT;
	switch (val) {
	case BQ2589X_CHRG_STAT_FASTCHG:
		return POWER_SUPPLY_CHARGE_TYPE_FAST;
	case BQ2589X_CHRG_STAT_PRECHG:
		return POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
	case BQ2589X_CHRG_STAT_CHGDONE:
	case BQ2589X_CHRG_STAT_IDLE:
		return POWER_SUPPLY_CHARGE_TYPE_NONE;
	default:
		return POWER_SUPPLY_CHARGE_TYPE_UNKNOWN;
	}
}
EXPORT_SYMBOL_GPL(bq2589x_charge_status);

static ssize_t bq2589x_show_registers(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	u8 addr;
	u8 val;
	u8 tmpbuf[300];
	int len;
	int idx = 0;
	int ret ;

	idx = snprintf(buf, PAGE_SIZE, "%s:\n", "Charger 1");
	for (addr = 0x0; addr <= 0x14; addr++) {
		ret = bq2589x_read_byte(g_bq, &val, addr);
		if (ret == 0) {
			len = snprintf(tmpbuf, PAGE_SIZE - idx,"Reg[0x%.2x] = 0x%.2x\n", addr, val);
			memcpy(&buf[idx], tmpbuf, len);
			idx += len;
		}
	}

	return idx;
}

static DEVICE_ATTR(registers, S_IRUGO, bq2589x_show_registers, NULL);

static struct attribute *bq2589x_attributes[] = {
	&dev_attr_registers.attr,
	NULL,
};

static const struct attribute_group bq2589x_attr_group = {
	.attrs = bq2589x_attributes,
};


static int bq2589x_parse_dt(struct device *dev, struct bq2589x *bq)
{
	int ret;
	struct device_node *np = dev->of_node;

	bq->cfg.enable_auto_dpdm = of_property_read_bool(np, "ti,bq2589x,enable-auto-dpdm");
	bq->cfg.enable_term = of_property_read_bool(np, "ti,bq2589x,enable-termination");
	bq->cfg.enable_ico = of_property_read_bool(np, "ti,bq2589x,enable-ico");
	bq->cfg.use_absolute_vindpm = of_property_read_bool(np, "ti,bq2589x,use-absolute-vindpm");

	ret = of_property_read_u32(np, "ti,bq2589x,charge-voltage",&bq->cfg.charge_voltage);
	if (ret) {
		bq->cfg.charge_voltage = DEFAULT_BATT_CV;
	} else {
		pr_err("charge_voltage: %d\n", bq->cfg.charge_voltage);
	}

	ret = of_property_read_u32(np, "ti,bq2589x,charge-current",&bq->cfg.charge_current);
	if (ret) {
		bq->cfg.charge_current = DEFAULT_CHG_CURR;
	} else {
		pr_err("charge_current: %d\n", bq->cfg.charge_current);
	}

	ret = of_property_read_u32(np, "ti,bq2589x,term-current",&bq->cfg.term_current);
	if (ret) {
		bq->cfg.term_current = DEFAULT_TERM_CURR;
	} else {
		pr_err("charge_current: %d\n", bq->cfg.term_current);
	}

	ret = of_property_read_u32(np, "ti,bq2589x,,prechg_current", &bq->cfg.prechg_current);
	if (ret) {
		bq->cfg.prechg_current = DEFAULT_PRECHG_CURR;
	} else {
		pr_err("prechg_current: %d\n", bq->cfg.prechg_current);
	}

	bq->otg_gpio = of_get_named_gpio(np, "otg-gpio", 0);
        if (ret < 0) {
                pr_err("%s no otg_gpio info\n", __func__);
	};

	bq->irq_gpio = of_get_named_gpio(np, "intr-gpio", 0);
        if (ret < 0) {
                pr_err("%s get intr_gpio fail !\n", __func__);
        } else {
                pr_err("%s intr_gpio info %d\n", __func__, bq->irq_gpio);
	}

	bq->usb_switch1 = of_get_named_gpio(np, "usb-switch1", 0);
	if (ret < 0) {
		pr_err("%s get usb-switch1 fail !\n", __func__);
	} else {
		pr_err("%s usb_switch1 info %d\n", __func__, bq->usb_switch1);
	}

	return 0;
}

static int bq2589x_usb_switch(struct bq2589x *bq, bool en)
{
	gpio_direction_output(bq->usb_switch1, en);
	msleep(5);

	return 0;
}


static int bq2589x_detect_device(struct bq2589x *bq)
{
	int ret;
	u8 data;

	ret = bq2589x_read_byte(bq, &data, BQ2589X_REG_14);
	if (ret == 0) {
		bq->part_no = (data & BQ2589X_PN_MASK) >> BQ2589X_PN_SHIFT;
		bq->revision = (data & BQ2589X_DEV_REV_MASK) >> BQ2589X_DEV_REV_SHIFT;
	}

	return ret;
}

static int bq2589x_get_chg_status(struct bq2589x *bq)
{
	int ret;
	u8 status = 0;
	u8 charge_status = 0;

	/* Read STATUS registers */
	ret = bq2589x_read_byte(bq, &status, BQ2589X_REG_0B);
	if (ret) {
		dev_err(bq->dev, "%s: read regs:0x0b fail !\n", __func__);
		return bq->chg_status;
	}

	charge_status = (status & BQ2589X_CHRG_STAT_MASK) >> BQ2589X_CHRG_STAT_SHIFT;
	switch (charge_status) {
	case BQ2589X_NOT_CHARGING:
		bq->chg_status = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case BQ2589X_PRE_CHARGE:
	case BQ2589X_FAST_CHARGING:
		bq->chg_status = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case BQ2589X_CHARGE_DONE:
		bq->chg_status = POWER_SUPPLY_STATUS_FULL;
		break;
	default:
		bq->chg_status = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	if (bq->usb_online && bq->charging_enable
			&& (bq->chg_status == POWER_SUPPLY_STATUS_DISCHARGING)) {
		bq->chg_status = POWER_SUPPLY_STATUS_CHARGING;
	}

	return bq->chg_status;
}

static void bq2589x_adjust_absolute_vindpm(struct bq2589x *bq)
{
	u16 vbus_volt;
	u16 vindpm_volt;
	int ret;

	ret = bq2589x_disable_charger(bq);
	if (ret < 0) {
		dev_err(bq->dev,"%s:failed to disable charger\n",__func__);
		/*return;*/
	}
	/* wait for new adc data */
	msleep(1000);
	vbus_volt = bq2589x_adc_read_vbus_volt(bq);
	ret = bq2589x_enable_charger(bq);
	if (ret < 0) {
		dev_err(bq->dev, "%s:failed to enable charger\n",__func__);
		return;
	}
//+ ExtB oak-1971, tankaikun@wt, add 20220121, fix 'vindpm occurred' error
	//if (vbus_volt < 6000)
	//	vindpm_volt = vbus_volt - 600;
	//else
	//	vindpm_volt = vbus_volt - 1200;
	vindpm_volt = 4600;
//- ExtB oak-1971, tankaikun@wt, add 20220121, fix 'vindpm occurred' error
	ret = bq2589x_set_input_volt_limit(bq, vindpm_volt);
	if (ret < 0)
		dev_err(bq->dev, "%s:Set absolute vindpm threshold %d Failed:%d\n", __func__, vindpm_volt, ret);
	else
		dev_info(bq->dev, "%s:Set absolute vindpm threshold %d successfully\n", __func__, vindpm_volt);

}

static void bq2589x_adapter_in_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, adapter_in_work);
	int ret;

	if (bq->vbus_type == BQ2589X_VBUS_MAXC) {
		dev_info(bq->dev, "%s:HVDCP or Maxcharge adapter plugged in\n", __func__);
		ret = bq2589x_set_input_current_limit(bq, 500);
		if (ret < 0)
			dev_err(bq->dev, "%s:Failed to set charge current:%d\n", __func__, ret);
		else
			dev_info(bq->dev, "%s: Set inputcurrent to %dmA successfully\n",__func__,bq->cfg.charge_current);
		schedule_delayed_work(&bq->ico_work, 0);
	} else if (bq->vbus_type == BQ2589X_VBUS_USB_DCP) {/* DCP, let's check if it is PE adapter*/
		ret = bq2589x_set_input_current_limit(bq, 500);
		dev_info(bq->dev, "%s:usb dcp adapter plugged in\n", __func__);
		ret = bq2589x_set_charge_current(bq, bq->cfg.charge_current);
		if (ret < 0)
			dev_err(bq->dev, "%s:Failed to set charge current:%d\n", __func__, ret);
		else
			dev_info(bq->dev, "%s: Set input current to %dmA successfully\n",__func__,bq->cfg.charge_current);
	} else if (bq->vbus_type == BQ2589X_VBUS_USB_SDP || bq->vbus_type == BQ2589X_VBUS_UNKNOWN) {
		if (bq->vbus_type == BQ2589X_VBUS_USB_SDP)
			dev_info(bq->dev, "%s:host SDP plugged in\n", __func__);
		else
			dev_info(bq->dev, "%s:unknown adapter plugged in\n", __func__);

		ret = bq2589x_set_input_current_limit(bq, 500);
		if (ret < 0)
			dev_err(bq->dev, "%s:Failed to set charge current:%d\n", __func__, ret);
		else
			dev_info(bq->dev, "%s: Set input current to %dmA successfully\n",__func__,500);
	} else {
		dev_info(bq->dev, "%s:other adapter plugged in,vbus_type is %d\n", __func__, bq->vbus_type);
		ret = bq2589x_set_input_current_limit(bq, 500);
		if (ret < 0)
			dev_err(bq->dev, "%s:Failed to set charge current:%d\n", __func__, ret);
		else
			dev_info(bq->dev, "%s: Set input current to %dmA successfully\n",__func__,1000);
		schedule_delayed_work(&bq->ico_work, 0);
	}

	ret = bq2589x_set_charge_current(bq, 500);
	if (ret < 0) {
		dev_err(bq->dev, "%s:Failed to set charge current:%d\n", __func__, ret);
	}

	if (bq->cfg.use_absolute_vindpm)
		bq2589x_adjust_absolute_vindpm(bq);

}

static void bq2589x_adapter_out_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, adapter_out_work);
	int ret;

	bq2589x_set_charge_current(bq, 500);
	bq2589x_set_input_current_limit(bq, 500);
	ret = bq2589x_set_input_volt_limit(bq, 4400);
	if (ret < 0)
		dev_err(bq->dev,"%s:reset vindpm threshold to 4400 failed:%d\n",__func__,ret);
	else
		dev_info(bq->dev,"%s:reset vindpm threshold to 4400 successfully\n",__func__);

	cancel_delayed_work_sync(&bq->monitor_work);
}

static void bq2589x_ico_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, ico_work.work);
	int ret;
	int idpm;
	u8 status;
	static bool ico_issued;

	if (!ico_issued) {
		ret = bq2589x_force_ico(bq);
		if (ret < 0) {
			schedule_delayed_work(&bq->ico_work, HZ); /* retry 1 second later*/
			dev_info(bq->dev, "%s:ICO command issued failed:%d\n", __func__, ret);
		} else {
			ico_issued = true;
			schedule_delayed_work(&bq->ico_work, 3 * HZ);
			dev_info(bq->dev, "%s:ICO command issued successfully\n", __func__);
		}
	} else {
		ico_issued = false;
		ret = bq2589x_check_force_ico_done(bq);
		if (ret) {/*ico done*/
			ret = bq2589x_read_byte(bq, &status, BQ2589X_REG_13);
			if (ret == 0) {
				idpm = ((status & BQ2589X_IDPM_LIM_MASK) >> BQ2589X_IDPM_LIM_SHIFT) * BQ2589X_IDPM_LIM_LSB + BQ2589X_IDPM_LIM_BASE;
				dev_info(bq->dev, "%s:ICO done, result is:%d mA\n", __func__, idpm);
			}
		}
	}
}

static void bq2589x_monitor_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, monitor_work.work);
	int ret;
	u8 status = 0;
	u8 charge_status = 0;
	int chg_current;

	//bq2589x_dump_regs(bq);
	//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	if(!bq->resume_completed){
		pr_err("bq2589x_monitor_workfunc suspend cannot work \n");
		schedule_delayed_work(&bq->monitor_work, msecs_to_jiffies(10000));
		return ;
	}
	//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	bq->vbus_volt = bq2589x_adc_read_vbus_volt(bq);
	bq->vbat_volt = bq2589x_adc_read_battery_volt(bq);
	chg_current = bq2589x_adc_read_charge_current(bq);

	bq2589x_read_byte(bq, &status, BQ2589X_REG_0B);
	charge_status = (status & BQ2589X_CHRG_STAT_MASK) >> BQ2589X_CHRG_STAT_SHIFT;
	switch (charge_status) {
	case BQ2589X_NOT_CHARGING:
		bq->chg_status = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case BQ2589X_PRE_CHARGE:
	case BQ2589X_FAST_CHARGING:
		bq->chg_status = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case BQ2589X_CHARGE_DONE:
		bq->chg_status = POWER_SUPPLY_STATUS_FULL;
		break;
	default:
		bq->chg_status = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	dev_err(bq->dev, "%s:vbus_type: %d, vbus volt:%d,vbat volt:%d,charge current:%d, usb-switch1 %d, usb_switch_flag %d\n",
			__func__, bq->charge_type, bq->vbus_volt, bq->vbat_volt, chg_current, bq->usb_switch1, bq->usb_switch_flag);

	ret = bq2589x_read_byte(bq, &status, BQ2589X_REG_13);
	if (ret == 0 && (status & BQ2589X_VDPM_STAT_MASK))
		dev_info(bq->dev, "%s:VINDPM occurred\n", __func__);
	if (ret == 0 && (status & BQ2589X_IDPM_STAT_MASK))
		dev_info(bq->dev, "%s:IINDPM occurred\n", __func__);

	schedule_delayed_work(&bq->monitor_work, msecs_to_jiffies(10000));
}

void bq2589x_set_wtchg_update_work(struct bq2589x *bq)
{
	int ret;
	union power_supply_propval val = {0,};

	bq->usb_psy = power_supply_get_by_name("usb");

	if(bq->usb_psy) {
		//+ ExtB OAK-3916, tankaikun@wt, add 20220211, add battery protect mode
		val.intval = bq->usb_online;
		ret = power_supply_set_property(bq->usb_psy,
					POWER_SUPPLY_PROP_ONLINE, &val);
		if (ret < 0) {
			dev_err(bq->dev,
				"Couldn't set POWER_SUPPLY_PROP_ONLINE: ret = %d\n",  ret);
		}
		//- ExtB OAK-3916, tankaikun@wt, add 20220211, add battery protect mode

		ret = power_supply_set_property(bq->usb_psy,
					POWER_SUPPLY_PROP_DP_DM, &val);
		if (ret < 0) {
			dev_err(bq->dev,
				"Couldn't set POWER_SUPPLY_PROP_DP_DM: ret = %d\n",  ret);
		}
	}else {
		dev_err(bq->dev,
				"bq2589x_set_wtchg_update_work Couldn't get usb psy \n");
	}
}
EXPORT_SYMBOL_GPL(bq2589x_set_wtchg_update_work);

void bq2589x_set_usb_type(struct bq2589x *bq)
{
	int ret;
	union power_supply_propval val = {0,};

	val.intval = bq->charge_type;
	bq->usb_psy = power_supply_get_by_name("usb");
	if(bq->usb_psy) {
		ret = power_supply_set_property(bq->usb_psy,
						POWER_SUPPLY_PROP_REAL_TYPE, &val);
		if (ret < 0) {
			dev_err(bq->dev,
				"Couldn't set power_supply_prop_usb_type: ret = %d\n",  ret);
		}
	}
}
EXPORT_SYMBOL_GPL(bq2589x_set_usb_type);

void bq2589x_set_usb_online(struct bq2589x *bq)
{
	int ret;
	union power_supply_propval val = {0,};

	val.intval = bq->usb_online;
	bq->usb_psy = power_supply_get_by_name("usb");
	if(bq->usb_psy) {
		ret = power_supply_set_property(bq->usb_psy,
						POWER_SUPPLY_PROP_ONLINE, &val);
		if (ret < 0) {
			dev_err(bq->dev,
				"Couldn't set power_supply_prop_online: ret = %d\n",  ret);
		}
	}
}
EXPORT_SYMBOL_GPL(bq2589x_set_usb_online);

void bq2589x_set_charger_work(struct bq2589x *bq)
{
	int ret,pd_active=0;
	union power_supply_propval val = {0,};

	bq->wt_hvdcp_psy = power_supply_get_by_name("wt_hvdcp");
	if (bq->wt_hvdcp_psy) {
		ret = power_supply_get_property(bq->wt_hvdcp_psy,
							POWER_SUPPLY_PROP_TYPE, &val);
		if (ret < 0) {
			dev_err(bq->dev,
				"Couldn't get POWER_SUPPLY_PROP_TYPE, ret=%d\n", ret);
		}

		bq->charge_type = val.intval;
		dev_err(bq->dev, "%s:wt_hvdcp = %d\n", __func__, bq->charge_type);
	}

	bq->usb_psy = power_supply_get_by_name("usb");
	if(bq->usb_psy) {
		ret = power_supply_get_property(bq->usb_psy,
						POWER_SUPPLY_PROP_PD_ACTIVE, &val);
		pd_active = val.intval;
		if (ret < 0) {
			dev_err(bq->dev,
				"Couldn't set power_supply_prop_online: ret = %d\n",  ret);
		}
	}

	// ExtB oak-3798, tankaikun@wt, add 20220126, fix plug in charger slowly cause identify float type
	if ((bq->usb_switch_flag == false) && ( !bq->firmware_flag)
		&& (bq->charge_type == POWER_SUPPLY_TYPE_USB
			|| bq->charge_type == POWER_SUPPLY_TYPE_USB_CDP
			|| pd_active == POWER_SUPPLY_PD_ACTIVE
			|| pd_active == POWER_SUPPLY_PD_PPS_ACTIVE)) {
			// || bq->charge_type == POWER_SUPPLY_TYPE_USB_FLOAT
		bq2589x_usb_switch(bq, false);
	 }

	cancel_delayed_work_sync(&bq->monitor_work);
	schedule_delayed_work(&bq->monitor_work, 0);
}
EXPORT_SYMBOL_GPL(bq2589x_set_charger_work);

void bq2589x_set_otg_status(struct bq2589x *bq, bool enable)
{
	int ret;
	union power_supply_propval val = {0,};

	if(enable) {
		bq2589x_usb_switch(bq, !enable);
		bq2589x_exit_hiz_mode(bq);
		bq2589x_disable_charger(bq);
		bq2589x_enable_otg(bq);
		bq->batt_psy = power_supply_get_by_name("battery");
		if(bq->batt_psy) {
			power_supply_get_property(bq->batt_psy,
						POWER_SUPPLY_PROP_CAPACITY, &val);
			if (ret < 0) {
				dev_err(bq->dev,
					"Couldn't set power_supply_prop_status: ret = %d\n",  ret);
			}

			if(val.intval > BATT_LOW_VOLT) {
				bq2589x_set_otg_current(bq, 1100); // ExtB oak-2873, tankaikun@wt, modify 20220118, modify otg current to 1.2A
			}
			else {
				bq2589x_set_otg_current(bq, 500);
			}
		}
	} else {
		bq2589x_disable_otg(bq);
	}
}
EXPORT_SYMBOL_GPL(bq2589x_set_otg_status);

static void bq2589x_charger_irq_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, irq_work.work);
	u8 status = 0;
	u8 fault = 0;
	u8 charge_status = 0;
	int ret;
	union power_supply_propval val = {0,};

	/* Read STATUS and FAULT registers */
	msleep(5);
	//dev_err(bq->dev, "%s:  irq work start, revision:%d\n", __func__, bq->revision);
	ret = bq2589x_read_byte(bq, &status, BQ2589X_REG_0B);
	if (ret) {
		dev_err(bq->dev, "%s: read regs:0x0b fail !\n", __func__);
		return;
	}

	ret = bq2589x_read_byte(bq, &fault, BQ2589X_REG_0C);
	if (ret) {
		dev_err(bq->dev, "%s: read regs:0x0c fail !\n", __func__);
		return;
	}

	bq->vbus_type = (status & BQ2589X_VBUS_STAT_MASK) >> BQ2589X_VBUS_STAT_SHIFT;
	bq->usb_online = (status & BQ2589X_PG_STAT_MASK) >> BQ2589X_PG_STAT_SHIFT;
	charge_status = (status & BQ2589X_CHRG_STAT_MASK) >> BQ2589X_CHRG_STAT_SHIFT;
	switch (charge_status) {
	case BQ2589X_NOT_CHARGING:
		bq->chg_status = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case BQ2589X_PRE_CHARGE:
	case BQ2589X_FAST_CHARGING:
		bq->chg_status = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case BQ2589X_CHARGE_DONE:
		bq->chg_status = POWER_SUPPLY_STATUS_FULL;
		break;
	default:
		bq->chg_status = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	if ((bq->usb_switch_flag && bq->usb_online) && ( !bq->firmware_flag)) {
		bq2589x_usb_switch(bq, true);
		bq->usb_switch_flag = false;

		bq->wt_hvdcp_psy = power_supply_get_by_name("wt_hvdcp");
		if (bq->wt_hvdcp_psy) {
			ret = power_supply_set_property(bq->wt_hvdcp_psy,
								POWER_SUPPLY_PROP_APSD_RERUN, &val);
			if (ret < 0) {
				dev_err(bq->dev,
					"Couldn't set POWER_SUPPLY_PROP_APSD_RERUN, ret=%d\n", ret);

				bq->charge_type = POWER_SUPPLY_TYPE_USB;
				bq2589x_set_usb_type(bq);
				bq2589x_usb_switch(bq, false);
				cancel_delayed_work_sync(&bq->monitor_work);
				schedule_delayed_work(&bq->monitor_work, msecs_to_jiffies(300));
			}
		}

		schedule_work(&bq->adapter_in_work);
		msleep(20);
		bq2589x_set_wtchg_update_work(bq);
		dev_err(bq->dev, "%s:adapter plugged in\n", __func__);
	} else if ((!bq->usb_online) && ( !bq->firmware_flag)) {
		bq2589x_usb_switch(bq, false);
		bq->usb_switch_flag = true;

		bq->charge_type = POWER_SUPPLY_TYPE_UNKNOWN;
		bq2589x_set_usb_type(bq);
		schedule_work(&bq->adapter_out_work);
		msleep(20);
		bq2589x_set_wtchg_update_work(bq);
		dev_err(bq->dev, "%s:adapter removed\n", __func__);
	}

	if (bq->usb_online && bq->chg_status == POWER_SUPPLY_STATUS_FULL) {
		if (bq->chg_error_count++ >= CHG_FULL_ERROR_MAX) {
			bq->chg_error_count = 0;
			msleep(100);
			bq2589x_set_wtchg_update_work(bq);
		}
	} else {
		bq->chg_error_count = 0;
	}
	dev_err(bq->dev, "%s:usb_online: %d, chg_status: %d, chg_fault: 0x%x\n",
							__func__, bq->usb_online, bq->chg_status, fault);
}

static irqreturn_t bq2589x_charger_interrupt(int irq, void *data)
{
	struct bq2589x *bq = data;

	schedule_delayed_work(&bq->irq_work, 0);
	return IRQ_HANDLED;
}

/************************
 * USB PSY REGISTRATION *
 ************************/
static enum power_supply_property bq2589x_usb_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VBUS_VOLTAGE,
	POWER_SUPPLY_PROP_TYPE,
	POWER_SUPPLY_PROP_REAL_TYPE,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_APSD_RERUN,
	POWER_SUPPLY_PROP_DP_DM,
	POWER_SUPPLY_PROP_INPUT_SUSPEND,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CHARGE_ENABLED,
	POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_UPDATE_NOW,
	POWER_SUPPLY_PROP_USB_OTG,
	POWER_SUPPLY_PROP_USB_SWITCH,
	POWER_SUPPLY_PROP_SET_SHIP_MODE,
};

static int bq2589x_usb_get_prop(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct bq2589x *bq = power_supply_get_drvdata(psy);
	int ret = 0;
	val->intval = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = bq2589x_get_chg_status(bq);
		break;

	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = bq->usb_online;
		break;

	case POWER_SUPPLY_PROP_VBUS_VOLTAGE:
		val->intval = bq2589x_adc_read_vbus_volt(bq);
		break;

	case POWER_SUPPLY_PROP_TYPE:
	case POWER_SUPPLY_PROP_REAL_TYPE:
		val->intval = bq->vbus_type;
		break;

	case POWER_SUPPLY_PROP_UPDATE_NOW:
		val->intval =  (int)bq->firmware_flag;
		break;

	case POWER_SUPPLY_PROP_USB_OTG:
		val->intval =  (int)bq->otg_status;
		break;
	//+ ExtB oak-4542, tankaikun@wt, add 20220206, fix usb do not have diag port
	case POWER_SUPPLY_PROP_USB_SWITCH:
		val->intval = (int)bq->usb_switch_flag;
		break;
	//- ExtB oak-4542, tankaikun@wt, add 20220206, fix usb do not have diag port
	case POWER_SUPPLY_PROP_INPUT_SUSPEND:
		val->intval = bq->usb_suspend;
		break;
	default:
		pr_err("get prop %d is not supported in usb\n", psp);
		ret = -EINVAL;
		break;
	}

	if (ret < 0) {
		dev_err(bq->dev, "Couldn't get prop %d rc = %d\n", psp, ret);
		return -ENODATA;
	}

	return 0;
}


static int bq2589x_usb_set_prop(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct bq2589x *bq = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_DP_DM:
		bq2589x_set_charger_work(bq);
		break;

	case POWER_SUPPLY_PROP_SET_SHIP_MODE:
		bq2589x_enter_ship_mode(bq);
		break;

	case POWER_SUPPLY_PROP_INPUT_SUSPEND:
		bq->usb_suspend = (bool)val->intval;
		dev_err(bq->dev,
				"set usb suspend: %d\n", bq->usb_suspend);
		if (bq->usb_suspend) {
			bq2589x_enter_hiz_mode(bq);
		} else {
			bq2589x_exit_hiz_mode(bq);
		}
		break;

	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		dev_err(bq->dev, 
				"set input current: %d\n", val->intval);
		bq2589x_set_input_current_limit(bq, val->intval);
		break;

	case POWER_SUPPLY_PROP_CHARGE_ENABLED:
		bq->charging_enable = (bool)val->intval;
		dev_err(bq->dev,
				"set charging enable: %d\n", bq->charging_enable);
		if (bq->charging_enable) {
			bq2589x_enable_charger(bq);
		} else {
			bq2589x_disable_charger(bq);
		}
		break;

	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		dev_err(bq->dev, 
				"set term current: %d\n", val->intval);
		bq2589x_set_term_current(bq, val->intval);
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		dev_err(bq->dev, 
				"set charge voltage: %d\n", val->intval);
		bq2589x_set_chargevoltage(bq, val->intval);
		break;

	case POWER_SUPPLY_PROP_CURRENT_NOW:
		dev_err(bq->dev, 
				"set charge current: %d\n", val->intval);
		bq2589x_set_charge_current(bq, val->intval);
		break;
			
	case POWER_SUPPLY_PROP_UPDATE_NOW:
		bq->firmware_flag = (bool)val->intval;
		bq2589x_usb_switch(bq, bq->firmware_flag);
		break;

	case POWER_SUPPLY_PROP_USB_OTG:
		bq->otg_status = (bool)val->intval;
		bq2589x_set_otg_status(bq, bq->otg_status);
		dev_err(bq->dev, 
				"set otg_status: %d\n", bq->otg_status);
		break;
	//+ ExtB oak-4542, tankaikun@wt, add 20220206, fix usb do not have diag port
	case POWER_SUPPLY_PROP_USB_SWITCH:
		pr_err("main chg set usb switch %d \n", val->intval);
		bq->usb_switch_flag = (bool)val->intval;
		break;
	//- ExtB oak-4542, tankaikun@wt, add 20220206, fix usb do not have diag port
	default:
		dev_err(bq->dev, "set prop: %d is not supported\n", psp);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int bq2589x_usb_prop_is_writeable(struct power_supply *psy,
		enum power_supply_property psp)
{

	switch (psp) {
	case POWER_SUPPLY_PROP_APSD_RERUN:
	case POWER_SUPPLY_PROP_DP_DM:
	case POWER_SUPPLY_PROP_INPUT_SUSPEND:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
	case POWER_SUPPLY_PROP_CHARGE_ENABLED:
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
	case POWER_SUPPLY_PROP_UPDATE_NOW:
	case POWER_SUPPLY_PROP_USB_OTG:
	case POWER_SUPPLY_PROP_SET_SHIP_MODE:
	case POWER_SUPPLY_PROP_USB_SWITCH:
		return 1;

	default:
		break;
	}

	return 0;
}

static const struct power_supply_desc usb_psy_desc = {
	.name = "main_chg",
	.type = POWER_SUPPLY_TYPE_MAIN,
	.properties = bq2589x_usb_props,
	.num_properties = ARRAY_SIZE(bq2589x_usb_props),
	.get_property = bq2589x_usb_get_prop,
	.set_property = bq2589x_usb_set_prop,
	.property_is_writeable = bq2589x_usb_prop_is_writeable,
};

static int bq2589x_init_usb_psy(struct bq2589x *bq)
{
	struct power_supply_config wall_cfg = {};

	wall_cfg.drv_data = bq;
	wall_cfg.of_node = bq->dev->of_node;
	bq->wall_psy = devm_power_supply_register(bq->dev,
						  &usb_psy_desc,
						  &wall_cfg);
	if (IS_ERR(bq->wall_psy)) {
		pr_err("Couldn't register main chg power supply\n");
		return PTR_ERR(bq->wall_psy);
	}

	return 0;
}

static int bq2589x_charger_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct bq2589x *bq;
	int irqn;

	int ret;

	printk("bq2589x driver probe start \n");
	bq = devm_kzalloc(&client->dev, sizeof(struct bq2589x), GFP_KERNEL);
	if (!bq) {
		dev_err(&client->dev, "%s: out of memory\n", __func__);
		return -ENOMEM;
	}

	bq->dev = &client->dev;
	bq->client = client;
	i2c_set_clientdata(client, bq);

	//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend 
	bq->resume_completed = true;
	mutex_init(&bq->resume_complete_lock);
	//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend 

	bq->usb_switch_flag = false; // ExtB oak-4542, tankaikun@wt, add 20220206, fix usb do not have diag port
	bq->firmware_flag = false;
	ret = bq2589x_detect_device(bq);
	if (!ret && bq->part_no == BQ25890) {
		bq->status |= BQ2589X_STATUS_EXIST;
		bq->is_bq25890h = true;
		dev_err(bq->dev, "%s: charger device bq25890 detected, revision:%d\n", __func__, bq->revision);
	} else if (!ret && bq->part_no == SYV690) {
		bq->status |= BQ2589X_STATUS_EXIST;
		bq->is_bq25890h = false;
		dev_err(bq->dev, "%s: charger device SYV690 detected, revision:%d\n", __func__, bq->revision);
	}else {
		dev_err(bq->dev, "%s: no bq25890 charger device found:%d\n", __func__, ret);
		return -ENODEV;
	}

	bq->batt_psy = power_supply_get_by_name("battery");

	g_bq = bq;
	ret = bq2589x_init_usb_psy(bq);
	if (ret < 0) {
		pr_err("Couldn't initialize usb psy rc=%d\n", ret);
		goto err_1;
	}

	if (client->dev.of_node)
		bq2589x_parse_dt(&client->dev, bq);

	ret = gpio_request(bq->usb_switch1, "usb_switch1_gpio");
	if (ret) {
		dev_err(bq->dev, "%s: %d usb_switch1_gpio request failed\n",
					__func__, bq->usb_switch1);
		goto err_0;
	}

	ret = bq2589x_init_device(bq);
	if (ret) {
		dev_err(bq->dev, "device init failure: %d\n", ret);
		goto err_0;
	}
	ret = gpio_request(bq->irq_gpio, "bq2589x irq pin");
	if (ret) {
		dev_err(bq->dev, "%s: %d gpio request failed\n", __func__, bq->irq_gpio);
		goto err_0;
	}

	irqn = gpio_to_irq(bq->irq_gpio);
	if (irqn < 0) {
		dev_err(bq->dev, "%s:%d gpio_to_irq failed\n", __func__, irqn);
		ret = irqn;
		goto err_0;
	}
	client->irq = irqn;

	INIT_WORK(&bq->adapter_in_work, bq2589x_adapter_in_workfunc);
	INIT_WORK(&bq->adapter_out_work, bq2589x_adapter_out_workfunc);
	INIT_DELAYED_WORK(&bq->irq_work, bq2589x_charger_irq_workfunc);
	INIT_DELAYED_WORK(&bq->monitor_work, bq2589x_monitor_workfunc);
	INIT_DELAYED_WORK(&bq->ico_work, bq2589x_ico_workfunc);

	ret = sysfs_create_group(&bq->dev->kobj, &bq2589x_attr_group);
	if (ret) {
		dev_err(bq->dev, "failed to register sysfs. err: %d\n", ret);
		goto err_irq;
	}

	ret = request_irq(client->irq, bq2589x_charger_interrupt,
						IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
						"bq2589x_charger1_irq", bq);
	if (ret) {
		dev_err(bq->dev, "%s:Request IRQ %d failed: %d\n", __func__, client->irq, ret);
		goto err_irq;
	} else {
		dev_err(bq->dev, "%s:irq = %d\n", __func__, client->irq);
	}

	enable_irq_wake(irqn);

	if (bq->is_bq25890h)
		hardwareinfo_set_prop(HARDWARE_CHARGER_IC, "BQ25890H_CHARGER");
	else
		hardwareinfo_set_prop(HARDWARE_CHARGER_IC, "SYV690_CHARGER");

	printk("bq2589x driver probe succeed !!! \n");
	schedule_delayed_work(&bq->irq_work, msecs_to_jiffies(1500));

	return 0;

err_irq:
	cancel_work_sync(&bq->adapter_in_work);
	cancel_work_sync(&bq->adapter_out_work);
	cancel_delayed_work_sync(&bq->irq_work);
	cancel_delayed_work_sync(&bq->monitor_work);
	cancel_delayed_work_sync(&bq->ico_work);
err_1:
err_0:
	// ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	mutex_destroy(&bq->resume_complete_lock);

	devm_kfree(&client->dev, bq);
	bq = NULL;
	g_bq = NULL;
	return ret;
}

static void bq2589x_charger_shutdown(struct i2c_client *client)
{
	struct bq2589x *bq = i2c_get_clientdata(client);

	dev_info(bq->dev, "%s: shutdown\n", __func__);

	// ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	mutex_destroy(&bq->resume_complete_lock);

	bq2589x_adc_stop(bq);
	sysfs_remove_group(&bq->dev->kobj, &bq2589x_attr_group);
	cancel_work_sync(&bq->adapter_in_work);
	cancel_work_sync(&bq->adapter_out_work);
	cancel_delayed_work_sync(&bq->irq_work);
	cancel_delayed_work_sync(&bq->monitor_work);
	cancel_delayed_work_sync(&bq->ico_work);

	free_irq(bq->client->irq, NULL);
	g_bq = NULL;
}

//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
static int bq2589x_charger_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq2589x *bq = i2c_get_clientdata(client);

	dev_info(bq->dev,"bq2589x_charger_resume \n");
	mutex_lock(&bq->resume_complete_lock);
	bq->resume_completed = true;
	mutex_unlock(&bq->resume_complete_lock);

	return 0;
}

static int bq2589x_charger_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq2589x *bq = i2c_get_clientdata(client);

	dev_info(bq->dev,"bq2589x_charger_suspend \n");
	mutex_lock(&bq->resume_complete_lock);
	bq->resume_completed = false;
	mutex_unlock(&bq->resume_complete_lock);

	return 0;
}
//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend

static struct of_device_id bq2589x_charger_match_table[] = {
	{.compatible = "ti,bq2589x-1",},
	{},
};


static const struct i2c_device_id bq2589x_charger_id[] = {
	{ "bq2589x-1", BQ25890 },
	{},
};

//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
static const struct dev_pm_ops bq2589x_charger_pm_ops = {
	.resume		= bq2589x_charger_resume,
	.suspend	= bq2589x_charger_suspend,
};
//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend

MODULE_DEVICE_TABLE(i2c, bq2589x_charger_id);

static struct i2c_driver bq2589x_charger_driver = {
	.driver		= {
		.name	= "bq2589x-1",
		.of_match_table = bq2589x_charger_match_table,
		.pm		= &bq2589x_charger_pm_ops,
	},
	.id_table	= bq2589x_charger_id,

	.probe		= bq2589x_charger_probe,
	.shutdown   = bq2589x_charger_shutdown,
};

module_i2c_driver(bq2589x_charger_driver);

MODULE_DESCRIPTION("TI BQ2589x Charger Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Texas Instruments");
