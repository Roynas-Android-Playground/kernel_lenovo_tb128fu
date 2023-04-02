/*
 * wingtch charger manage driver
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/power_supply.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/pm_wakeup.h>
#include <linux/iio/iio.h>
#include <linux/iio/consumer.h>
#include <linux/ktime.h>
#include "wt_chg.h"

static int thermal_mitigation[] = {
	4000000, 3500000, 3000000, 2500000,
	2000000, 1500000, 1000000, 500000,
};

static struct hvdcp30_profile_t  hvdcp30_profile[] = {
	{3400, 10000},
	{3500, 10000},
	{3600, 10000},
	{3700, 10000},
	{3800, 10000},
	{3900, 10000},
	{4000, 10000},
	{4100, 10000},
	{4200, 10000},
	{4300, 10000},
};

static int get_iio_channel(struct wt_chg *chg, const char *propname,
                                        struct iio_channel **chan)
{
        int ret = 0;

        ret = of_property_match_string(chg->dev->of_node,
                                        "io-channel-names", propname);
        if (ret < 0)
                return 0;

        *chan = iio_channel_get(chg->dev, propname);
        if (IS_ERR(*chan)) {
                ret = PTR_ERR(*chan);
                if (ret != -EPROBE_DEFER)
                        pr_err("%s channel unavailable, %d\n", propname, ret);
                *chan = NULL;
        }

        return ret;
}

//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
static inline bool is_device_suspended(struct wt_chg *chg)
{
	return !chg->resume_completed;
}
//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend

static int wtchg_get_usb_online(struct wt_chg *chg)
{
	if (chg->vbus_online &&
		(chg->real_type == POWER_SUPPLY_TYPE_USB
			|| chg->real_type == POWER_SUPPLY_TYPE_USB_CDP
			|| chg->real_type == POWER_SUPPLY_TYPE_UNKNOWN)) {
		chg->usb_online = true;
	} else {
		chg->usb_online = false;
	}

	return chg->usb_online;
}

//+ ExtB oak-3916, tankaikun@wt, add 20220209, modify battery protect mode
static void wtchg_set_usb_online(struct wt_chg *chg, int online)
{
	pr_err("wtchg_set_usb_online capacity=%d protect=%d batt_protected_mode=%d\n", 
				chg->batt_capacity, chg->batt_protected_mode);
	mutex_lock(&chg->battery_protect_lock);
	if(chg->batt_protected_mode == true){
		if ((chg->batt_capacity >= BATT_PROTECT_SOC_MIN)
				&& (chg->batt_capacity <= BATT_PROTECT_SOC_MAX)){
			chg->batt_protected_mode_disable_charge = true;
			chg->batt_protected_mode_disable_suspend = false;
		}else if((chg->batt_capacity < BATT_PROTECT_SOC_MIN)){
			chg->batt_protected_mode_disable_charge = false;
			chg->batt_protected_mode_disable_suspend = false;
		}else if((chg->batt_capacity > BATT_PROTECT_SOC_MAX)){
			chg->batt_protected_mode_disable_charge = true;
			chg->batt_protected_mode_disable_suspend = true;
		}
	}else{
		chg->batt_protected_mode_disable_suspend = false;
		chg->batt_protected_mode_disable_charge = false;
	}
	mutex_unlock(&chg->battery_protect_lock);
}
//- ExtB oak-3916, tankaikun@wt, add 20220209, modify battery protect mode

static int wtchg_get_ac_online(struct wt_chg *chg)
{
	if (chg->vbus_online &&
		(chg->real_type == POWER_SUPPLY_TYPE_USB_DCP
			|| chg->real_type == POWER_SUPPLY_TYPE_USB_FLOAT
			|| chg->real_type == POWER_SUPPLY_TYPE_USB_PD
			|| chg->real_type == POWER_SUPPLY_TYPE_USB_PD_DRP
			|| chg->real_type == POWER_SUPPLY_TYPE_USB_HVDCP
			|| chg->real_type == POWER_SUPPLY_TYPE_USB_HVDCP_3
			|| chg->real_type == POWER_SUPPLY_TYPE_USB_HVDCP_3P5)) {
		chg->ac_online = true;
	} else {
		chg->ac_online = false;
	}

	return chg->ac_online;
}

static int wtchg_get_usb_real_type(struct wt_chg *chg)
{
	return chg->real_type;
}

static void wtchg_pd_usb_switch(struct wt_chg *chg)
{
	int ret;
	union power_supply_propval val = {0,};
	struct power_supply *main_chg_psy;
	main_chg_psy = power_supply_get_by_name("main_chg");
	if(main_chg_psy) {
		val.intval = chg->real_type;
		ret = power_supply_set_property(main_chg_psy,
								POWER_SUPPLY_PROP_DP_DM, &val);
		if (ret < 0) {
			dev_err(chg->dev,
					"Couldn't set POWER_SUPPLY_PROP_DP_DM: ret = %d\n",  ret);
		}
	}
}

static void wtchg_set_usb_real_type(struct wt_chg *chg, int type)
{
	 chg->real_type = type;

	 if (chg->pd_active == POWER_SUPPLY_PD_ACTIVE
		|| chg->pd_active == POWER_SUPPLY_PD_PPS_ACTIVE) {
			chg->real_type = POWER_SUPPLY_TYPE_USB_PD;
	}
}

static void wtchg_fast_charger(struct wt_chg *chg)
{
	if(chg->vbus_online){
		switch (chg->real_type) {
		case POWER_SUPPLY_TYPE_USB_PD:
		case POWER_SUPPLY_TYPE_USB_HVDCP:
		case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		case POWER_SUPPLY_TYPE_USB_HVDCP_3P5:
			chg->fast_charger=1;
			break;
		default:
			chg->fast_charger=0;
			break;
		}
	}else{
		chg->fast_charger=0;
	}
}


static int wtchg_get_vbus_online(struct wt_chg *chg)
{
	int ret;
	union power_supply_propval val = {0,};

	//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	if(is_device_suspended(chg)){
		pr_err(" is_device_suspended cannot get vbus online \n");
		return chg->vbus_online; 
	}
	//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend

	chg->main_psy = power_supply_get_by_name("main_chg");
	if (chg->main_psy) {
		ret = power_supply_get_property(chg->main_psy,
							POWER_SUPPLY_PROP_ONLINE, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't get main: POWER_SUPPLY_PROP_ONLINE, ret=%d\n", ret);
		}

		chg->vbus_online = val.intval;
	}

	return chg->vbus_online; 
}

static void wtchg_set_slave_chg_ship_mode(struct wt_chg *chg)
{
	int ret;
	union power_supply_propval val = {0,};

	val.intval = chg->ship_mode;
	chg->slave_psy = power_supply_get_by_name("slave_chg");
	if (chg->slave_psy) {
		ret = power_supply_set_property(chg->slave_psy,
							POWER_SUPPLY_PROP_SET_SHIP_MODE, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set main: POWER_SUPPLY_PROP_SET_SHIP_MODE, ret=%d\n", ret);
		}
	}
}

static void wtchg_set_main_chg_ship_mode(struct wt_chg *chg)
{
	int ret;
	union power_supply_propval val = {0,};

	val.intval = chg->ship_mode;
	chg->main_psy = power_supply_get_by_name("main_chg");
	if (chg->main_psy) {
		ret = power_supply_set_property(chg->main_psy,
							POWER_SUPPLY_PROP_SET_SHIP_MODE, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set main: POWER_SUPPLY_PROP_SET_SHIP_MODE, ret=%d\n", ret);
		}
	}
}

static void wtchg_set_main_chg_otg(struct wt_chg *chg, int enable)
{
	int ret;
	union power_supply_propval val = {0,};

	val.intval = enable;
	chg->main_psy = power_supply_get_by_name("main_chg");
	if (chg->main_psy) {
		ret = power_supply_set_property(chg->main_psy,
							POWER_SUPPLY_PROP_USB_OTG, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set main: POWER_SUPPLY_PROP_USB_OTG, ret=%d\n", ret);
		}
	}
}

static void wtchg_set_main_chg_volt(struct wt_chg *chg, int volt)
{
	int ret;
	union power_supply_propval val = {0,};

	val.intval = volt;
	chg->main_psy = power_supply_get_by_name("main_chg");
	if (chg->main_psy) {
		ret = power_supply_set_property(chg->main_psy,
							POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set main: POWER_SUPPLY_PROP_VOLTAGE_NOW, ret=%d\n", ret);
		}
	}
}

static void wtchg_set_main_chg_current(struct wt_chg *chg, int curr)
{
	int ret;
	union power_supply_propval val = {0,};

	val.intval = curr;
	chg->main_psy = power_supply_get_by_name("main_chg");
	if (chg->main_psy) {
		ret = power_supply_set_property(chg->main_psy,
							POWER_SUPPLY_PROP_CURRENT_NOW, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set main: POWER_SUPPLY_PROP_CURRENT_NOW, ret=%d\n", ret);
		}
	}
}

static int  wtchg_set_main_input_current(struct wt_chg *chg, int curr)
{
	int ret = 0;
	union power_supply_propval val = {0,};

	val.intval = curr;
	chg->main_psy = power_supply_get_by_name("main_chg");
	if (chg->main_psy) {
		ret = power_supply_set_property(chg->main_psy,
							POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set main: POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, ret=%d\n", ret);
		}
	}

	return ret;
}

static void wtchg_set_main_term_current(struct wt_chg *chg, int curr)
{
	int ret;
	union power_supply_propval val = {0,};

	val.intval = curr;
	chg->main_psy = power_supply_get_by_name("main_chg");
	if (chg->main_psy) {
		ret = power_supply_set_property(chg->main_psy,
							POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set main: POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT, ret=%d\n", ret);
		}
	}
}

static void wtchg_set_main_chg_enable(struct wt_chg *chg, int enable)
{
	int ret;
	union power_supply_propval val = {0,};

	val.intval = enable;
	chg->main_psy = power_supply_get_by_name("main_chg");
	if (chg->main_psy) {
		ret = power_supply_set_property(chg->main_psy,
							POWER_SUPPLY_PROP_CHARGE_ENABLED, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set main: POWER_SUPPLY_PROP_CHARGE_ENABLED, ret=%d\n", ret);
		}
	}
}

static void wtchg_set_main_input_suspend(struct wt_chg *chg, int enable)
{
	int ret;
	union power_supply_propval val = {0,};

	val.intval = enable;
	chg->main_psy = power_supply_get_by_name("main_chg");
	if (chg->main_psy) {
		ret = power_supply_set_property(chg->main_psy,
							POWER_SUPPLY_PROP_INPUT_SUSPEND, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set main: POWER_SUPPLY_PROP_INPUT_SUSPEND, ret=%d\n", ret);
		}
	}
}

static int wtchg_get_main_input_suspend(struct wt_chg *chg)
{
	int ret;
	union power_supply_propval val = {0,};

	chg->main_psy = power_supply_get_by_name("main_chg");
	if (chg->main_psy) {
		ret = power_supply_get_property(chg->main_psy,
							POWER_SUPPLY_PROP_INPUT_SUSPEND, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't get main: POWER_SUPPLY_PROP_INPUT_SUSPEND, ret=%d\n", ret);
		}
	}

	return val.intval;
}

//+ ExtB oak-4542, tankaikun@wt, add 20220206, fix usb do not have diag port 
static void wtchg_set_main_chg_usb_switch(struct wt_chg *chg, int enable)
{
	int ret;
	union power_supply_propval val = {0,};

	val.intval = enable;
	chg->main_psy = power_supply_get_by_name("main_chg");
	if (chg->main_psy) {
		ret = power_supply_set_property(chg->main_psy,
							POWER_SUPPLY_PROP_USB_SWITCH, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set main: POWER_SUPPLY_PROP_USB_SWITCH, ret=%d\n", ret);
		}
	}
}
//- ExtB oak-4542, tankaikun@wt, add 20220206, fix usb do not have diag port

static void wtchg_set_slave_chg_volt(struct wt_chg *chg, int volt)
{
	int ret;
	union power_supply_propval val = {0,};

	val.intval = volt;
	chg->slave_psy = power_supply_get_by_name("slave_chg");
	if (chg->slave_psy) {
		ret = power_supply_set_property(chg->slave_psy,
							POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set slave: POWER_SUPPLY_PROP_VOLTAGE_NOW, ret=%d\n", ret);
		}
	}
}

static void wtchg_set_slave_chg_current(struct wt_chg *chg, int curr)
{
	int ret;
	union power_supply_propval val = {0,};

	val.intval = curr;
	chg->slave_psy = power_supply_get_by_name("slave_chg");
	if (chg->slave_psy) {
		ret = power_supply_set_property(chg->slave_psy,
							POWER_SUPPLY_PROP_CURRENT_NOW, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set slave: POWER_SUPPLY_PROP_CURRENT_NOW, ret=%d\n", ret);
		}
	}
}

static void wtchg_set_slave_input_current(struct wt_chg *chg, int curr)
{
	int ret;
	union power_supply_propval val = {0,};

	val.intval = curr;
	chg->slave_psy = power_supply_get_by_name("slave_chg");
	if (chg->slave_psy) {
		ret = power_supply_set_property(chg->slave_psy,
							POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set slave:POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, ret=%d\n", ret);
		}
	}
}

static void wtchg_set_slave_chg_enable(struct wt_chg *chg, int enable)
{
	int ret;
	union power_supply_propval val = {0,};

	val.intval = enable;
	chg->slave_psy = power_supply_get_by_name("slave_chg");
	if (chg->slave_psy) {
		ret = power_supply_set_property(chg->slave_psy,
							POWER_SUPPLY_PROP_CHARGE_ENABLED, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set slave: POWER_SUPPLY_PROP_CHARGE_ENABLED, ret=%d\n", ret);
		}
	}
}

static void wtchg_set_slave_input_suspend(struct wt_chg *chg, int enable)
{
	int ret;
	union power_supply_propval val = {0,};

	val.intval = enable;
	chg->slave_psy = power_supply_get_by_name("slave_chg");
	if (chg->slave_psy) {
		ret = power_supply_set_property(chg->slave_psy,
							POWER_SUPPLY_PROP_INPUT_SUSPEND, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set slave: POWER_SUPPLY_PROP_CHARGE_ENABLED, ret=%d\n", ret);
		}
	}
}

static void wtchg_set_slave_chg_monitor(struct wt_chg *chg, int enable)
{
	int ret;
	union power_supply_propval val = {0,};

	val.intval = enable;
	chg->slave_psy = power_supply_get_by_name("slave_chg");
	if (chg->slave_psy) {
		ret = power_supply_set_property(chg->slave_psy,
							POWER_SUPPLY_PROP_DP_DM, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set slave: POWER_SUPPLY_PROP_DP_DM, ret=%d\n", ret);
		}
	}
}

static int wtchg_set_adapter_voltage(struct wt_chg *chg, int volt)
{
	int ret = 0;
	union power_supply_propval val = {0,};

	val.intval = volt;
	chg->hvdcp_psy = power_supply_get_by_name("wt_hvdcp");
	if (chg->hvdcp_psy) {
		ret = power_supply_set_property(chg->hvdcp_psy,
							POWER_SUPPLY_PROP_INPUT_VOLTAGE_SETTLED, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set POWER_SUPPLY_PROP_INPUT_VOLTAGE_SETTLED, ret=%d\n", ret);
		}
	}

	return ret;
}

static void wtchg_set_hvdcp_voltage(struct wt_chg *chg, int volt)
{
	int ret = 0;
	union power_supply_propval val = {0,};

	val.intval = volt;
	chg->hvdcp_psy = power_supply_get_by_name("wt_hvdcp");
	if (chg->hvdcp_psy) {
		ret = power_supply_set_property(chg->hvdcp_psy,
							POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set POWER_SUPPLY_PROP_VOLTAGE_NOW, ret=%d\n", ret);
		}
	}
}

static int wtchg_set_chg_input_current(struct wt_chg *chg)
{
	int ret = 0;

	if (chg->slave_chg_enable) {
		chg->main_chg_input_curr = chg->chg_type_input_curr /2 + 350;
		chg->slave_chg_input_curr = chg->chg_type_input_curr /2 - 300;
	} else {
		chg->main_chg_input_curr = chg->chg_type_input_curr;
		chg->slave_chg_input_curr = FCC_0_MA;
	}

	//set charge input current value
	//if (chg->chg_type_input_curr  != chg->chg_type_input_curr_pre) {
		chg->chg_type_input_curr_pre = chg->chg_type_input_curr;
		chg->batt_protected_mode_disable_suspend_pre = chg->batt_protected_mode_disable_suspend;
		wtchg_set_main_input_current(chg, chg->main_chg_input_curr);

		if(chg->batt_protected_mode_disable_suspend)
			wtchg_set_main_input_suspend(chg, true);
		else
			wtchg_set_main_input_suspend(chg, false);

		if (chg->slave_chg_enable && !chg->batt_protected_mode_disable_suspend) {
			wtchg_set_slave_input_suspend(chg, false);
			wtchg_set_slave_input_current(chg, chg->slave_chg_input_curr);
		} else {
			wtchg_set_slave_input_suspend(chg, true);
		}
	//}

	return ret;
}

static int wtchg_set_chg_ibat_current(struct wt_chg *chg)
{
	int ret = 0;

	if (chg->slave_chg_enable) {
		chg->main_chg_ibatt = chg->chg_ibatt /2 + 600;
		chg->slave_chg_ibatt = chg->chg_ibatt /2 - 600;
	} else {
		chg->main_chg_ibatt = chg->chg_ibatt;
		chg->slave_chg_ibatt = FCC_0_MA;
	}

	dev_err(chg->dev,  "type_ibatt=%d, jeita_ibatt=%d, main_chg_ibatt=%d, slave_chg_ibatt=%d, chg_ibatt=%d,chg_ibatt_pre=%d\n",
		chg->chg_type_ibatt, chg->jeita_ibatt, chg->main_chg_ibatt,
		chg->slave_chg_ibatt, chg->chg_ibatt, chg->chg_ibatt_pre);

	//set charge ibat current value
	if ((chg->chg_ibatt  != chg->chg_ibatt_pre) ||
		(chg->batt_protected_mode_disable_charge != chg->batt_protected_mode_disable_charge_pre)){
		chg->chg_ibatt_pre = chg->chg_ibatt;
		chg->batt_protected_mode_disable_charge_pre = chg->batt_protected_mode_disable_charge;

		wtchg_set_main_chg_current(chg, chg->main_chg_ibatt);
		wtchg_set_main_term_current(chg, chg->chg_iterm);
		if (chg->main_chg_ibatt == FCC_0_MA || chg->batt_protected_mode_disable_charge) {
			wtchg_set_main_chg_enable(chg, false);
		} else {
			wtchg_set_main_chg_enable(chg, true);
		}

		if (chg->slave_chg_enable && !chg->batt_protected_mode_disable_charge) {
			wtchg_set_slave_chg_current(chg, chg->slave_chg_ibatt);
			wtchg_set_slave_chg_enable(chg, true);
			wtchg_set_slave_chg_monitor(chg, true);
		} else {
			wtchg_set_slave_chg_enable(chg, false);
			wtchg_set_slave_chg_monitor(chg, false);
		}
	}

	return ret;
}

static int wtchg_get_vbus_voltage(struct wt_chg *chg)
{
	int ret;
	int volt;
	union power_supply_propval val = {0,};

	//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	if(is_device_suspended(chg)){
		pr_err("wtchg_get_vbus_voltage is_device_suspended \n");
		return chg->vbus_volt;
	}
	//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend

	volt = chg->vbus_volt;
	chg->main_psy = power_supply_get_by_name("main_chg");
	if (chg->main_psy) {
		ret = power_supply_get_property(chg->main_psy,
							POWER_SUPPLY_PROP_VBUS_VOLTAGE, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't get POWER_SUPPLY_PROP_VBUS_VOLTAGE, ret=%d\n", ret);
			return ret;
		}

		volt = val.intval;
	}

	if (volt >= VBUS_VOLT_MIN) {
		chg->vbus_volt_zero_count = 0;
		if (volt == VBUS_VOLT_MIN) {
			chg->vbus_volt = 0;
		} else {
			chg->vbus_volt = volt;
		}
	} else {
		if ((chg->vbus_volt_zero_count++) >= VBUS_VOLT_ZERO_MAX) {
			chg->vbus_volt_zero_count = 0;
			chg->vbus_volt = volt;
		}
	}

	return chg->vbus_volt;
}

static int wtchg_get_main_chg_temp(struct wt_chg *chg,  int *val)
{
	int ret, temp;

	if (chg->main_chg_therm) {
		ret = iio_read_channel_processed(chg->main_chg_therm,
				&temp);
		if (ret < 0) {
			dev_err(chg->dev,
				"read main_chg_therm channel fail, ret=%d\n", ret);
			return ret;
		}
		*val = temp / 100;
	} else {
		ret = get_iio_channel(chg, "main_chg_therm", &chg->main_chg_therm);
		if (ret < 0) {
			pr_err("get main_chg_therm fail: %d\n", ret);
			return -ENODATA;
		}
		if (chg->main_chg_therm) {
			ret = iio_read_channel_processed(chg->main_chg_therm,&temp);
			if (ret < 0) {
				dev_err(chg->dev,"read main_chg_therm channel fail, ret=%d\n", ret);
				return ret;
			}
			*val = temp / 100;
		} else {
			return -ENODATA;
		}
	}

	return ret;
}

static int wtchg_get_usb_port_temp(struct wt_chg *chg,  int *val)
{
	int ret, temp;

	if (chg->usb_port_therm) {
		ret = iio_read_channel_processed(chg->usb_port_therm,
				&temp);
		if (ret < 0) {
			dev_err(chg->dev,
				"read usb_port_therm channel fail, ret=%d\n", ret);
			return ret;
		}
		*val = temp / 100;
	} else {
		ret = get_iio_channel(chg, "usb_port_therm", &chg->usb_port_therm);
		if (ret < 0) {
			pr_err("get usb_port_therm fail: %d\n", ret);
			return -ENODATA;
		}
		if (chg->usb_port_therm) {
			ret = iio_read_channel_processed(chg->usb_port_therm,&temp);
			if (ret < 0) {
				dev_err(chg->dev,"read usb_port_therm channel fail, ret=%d\n", ret);
				return ret;
			}
			*val = temp / 100;
		} else {
			return -ENODATA;
		}
	}

	if (chg->usb_temp_debug_flag == true) {
		*val = chg->usb_debug_temp;
	}

	return ret;
}

static int wtchg_get_board_pcb_temp(struct wt_chg *chg,  int *val)
{
	int ret, temp;

	if (chg->board_pcb_therm) {
		ret = iio_read_channel_processed(chg->board_pcb_therm,
				&temp);
		if (ret < 0) {
			dev_err(chg->dev,
				"read board_pcb_therm channel fail, ret=%d\n", ret);
			return ret;
		}
		*val = temp / 100;
	} else {
		ret = get_iio_channel(chg, "quiet_therm", &chg->board_pcb_therm);
		if (ret < 0) {
			pr_err("get board_pcb_therm fail: %d\n", ret);
			return -ENODATA;
		}
		if (chg->board_pcb_therm) {
			ret = iio_read_channel_processed(chg->board_pcb_therm,&temp);
			if (ret < 0) {
				dev_err(chg->dev,"read board_pcb_therm channel fail, ret=%d\n", ret);
				return ret;
			}
			*val = temp / 100;
		} else {
			return -ENODATA;
		}
	}

	return ret;
}

static int wtchg_get_batt_volt_max(struct wt_chg *chg)
{
	return chg->batt_cv_max;
}

static int wtchg_get_batt_current_max(struct wt_chg *chg)
{
	return chg->batt_fcc_max;
}

static int wtchg_get_batt_iterm_max(struct wt_chg *chg)
{
	return chg->batt_iterm;
}

//+ ExtB oak-2480, tankaikun@wt, add 20220121, fix power supply system report error maxChargingMicAmp cause mErrorShowDialog
static int wtchg_get_usb_max_current(struct wt_chg *chg)
{
	int input_curr_max = 0;

	if(chg->vbus_online){
		switch (chg->real_type) {
		case POWER_SUPPLY_TYPE_USB_PD:
		case POWER_SUPPLY_TYPE_USB_HVDCP:
		case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		case POWER_SUPPLY_TYPE_USB_HVDCP_3P5:
			input_curr_max = FCC_2000_MA;
			break;
		case POWER_SUPPLY_TYPE_USB_CDP:
			input_curr_max = FCC_1500_MA;
			break;
		case POWER_SUPPLY_TYPE_USB:
		case POWER_SUPPLY_TYPE_USB_FLOAT:
			input_curr_max = FCC_500_MA;
			break;
		default:
			input_curr_max = FCC_500_MA;
			break;
		}
	}else{
		input_curr_max = FCC_0_MA;
	}

	chg->chg_type_input_curr_max = input_curr_max * 1000; // ua

	return chg->chg_type_input_curr_max;
}

static int wtchg_get_usb_max_voltage(struct wt_chg *chg)
{
	int input_voltage_max = 0;

	if(chg->vbus_online){
		switch (chg->real_type) {
		case POWER_SUPPLY_TYPE_USB_PD:
			input_voltage_max = DEFAULT_PD_USB_VOLT_DESIGN;
			break;			
		case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		case POWER_SUPPLY_TYPE_USB_HVDCP_3P5:
			input_voltage_max = DEFAULT_HVDCP3_USB_VOLT_DESIGN;
			break;
		case POWER_SUPPLY_TYPE_USB_HVDCP:
			input_voltage_max = DEFAULT_HVDCP_USB_VOLT_DESIGN;
			break;
		case POWER_SUPPLY_TYPE_USB_CDP:
		case POWER_SUPPLY_TYPE_USB:
		case POWER_SUPPLY_TYPE_USB_FLOAT:
			input_voltage_max = DEFAULT_USB_VOLT_DESIGN;
			break;
		default:
			input_voltage_max = DEFAULT_USB_VOLT_DESIGN;
			break;
		}
	}else{
		input_voltage_max = 0;
	}
	chg->chg_type_input_voltage_max = input_voltage_max * 1000; // uv

	return chg->chg_type_input_voltage_max;
}
//- ExtB oak-2480, tankaikun@wt, add 20220121, fix power supply system report error maxChargingMicAmp cause mErrorShowDialog

static int wtchg_get_batt_status(struct wt_chg *chg)
{
	int ret;
	union power_supply_propval val = {0,};

	//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	if(is_device_suspended(chg)){
		pr_err("wtchg_get_batt_status is_device_suspended\n");
		return chg->chg_status;
	}
	//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend

#if 1
	chg->main_psy = power_supply_get_by_name("main_chg");
	if (chg->main_psy) {
		ret = power_supply_get_property(chg->main_psy,
							POWER_SUPPLY_PROP_STATUS, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't get POWER_SUPPLY_PROP_STATUS, ret=%d\n", ret);
		}

		chg->chg_status = val.intval;
	}
#else
	chg->bms_psy = power_supply_get_by_name("bms");
	if (chg->bms_psy) {
		ret = power_supply_get_property(chg->bms_psy,
							POWER_SUPPLY_PROP_STATUS, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't get POWER_SUPPLY_PROP_STATUS, ret=%d\n", ret);
		}

		chg->chg_status = val.intval;
	}
#endif

	return chg->chg_status;
}

static const char * wtchg_get_batt_id_info(struct wt_chg *chg)
{
	int ret;
	union power_supply_propval val = {0,};

	chg->bms_psy = power_supply_get_by_name("bms");
	if (chg->bms_psy) {
		ret = power_supply_get_property(chg->bms_psy,
							POWER_SUPPLY_PROP_MANUFACTURER, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't get POWER_SUPPLY_PROP_MANUFACTURER, ret=%d\n", ret);
		}

		chg->batt_id_string = val.strval;
	}

	return chg->batt_id_string;
}

static int wtchg_get_batt_volt(struct wt_chg *chg)
{
	int ret;
	int volt;
	union power_supply_propval val = {0,};

	//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	if(is_device_suspended(chg)){
		pr_err("wtchg_get_batt_volt is_device_suspended \n");
		return chg->batt_volt;
	}
	//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend

	volt = chg->batt_volt;
	chg->bms_psy = power_supply_get_by_name("bms");
	if (chg->bms_psy) {
		ret = power_supply_get_property(chg->bms_psy,
							POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't get POWER_SUPPLY_PROP_VOLTAGE_NOW, ret=%d\n", ret);
		}

		volt = val.intval;
	}

	if (volt >= BATT_VOLT_MAX) {
		volt = BATT_VOLT_MAX;
	}

	if (volt  <= BATT_VOLT_MIN && chg->vbus_online) {
		if ((chg->volt_zero_count++) >= VOLT_ZERO_MAX) {
			chg->volt_zero_count = 0;
			chg->batt_volt = volt;
		}
	} else if (volt  > BATT_VOLT_MIN) {
		chg->volt_zero_count = 0;
		chg->batt_volt = volt;
	}

	return chg->batt_volt;
}

static int wtchg_get_batt_current(struct wt_chg *chg)
{
	int ret;
	int curr;
	union power_supply_propval val = {0,};

	//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	if(is_device_suspended(chg)){
		pr_err("wtchg_get_batt_current is_device_suspended \n");
		return chg->chg_current;
	}
	//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend

	curr = chg->chg_current;
	chg->bms_psy = power_supply_get_by_name("bms");
	if (chg->bms_psy) {
		ret = power_supply_get_property(chg->bms_psy,
							POWER_SUPPLY_PROP_CURRENT_NOW, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't get POWER_SUPPLY_PROP_CURRENT_NOW, ret=%d\n", ret);
		}

		curr = val.intval;
	}

	if (curr != FCC_0_MA) {
		chg->current_zero_count = 0;
		chg->chg_current = curr;
	} else {
		if ((chg->current_zero_count++) >= CURRENT_ZERO_MAX) {
			chg->current_zero_count = 0;
			chg->chg_current = curr;
		}
	}

	return chg->chg_current;
}

static int wtchg_get_batt_temp(struct wt_chg *chg)
{
	int ret;
	int temp;
	union power_supply_propval val = {0,};

	//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	if(is_device_suspended(chg)){
		pr_err("wtchg_get_batt_temp is_device_suspended \n");
		return chg->batt_temp;
	}
	//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend

	temp = chg->batt_temp;
	chg->bms_psy = power_supply_get_by_name("bms");
	if (chg->bms_psy) {
		ret = power_supply_get_property(chg->bms_psy,
							POWER_SUPPLY_PROP_TEMP, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't get POWER_SUPPLY_PROP_TEMP, ret=%d\n", ret);
		}

		temp = val.intval;
		chg->batt_temp_true = temp;
	}

	if (temp <= BATT_TEMP_MIN) {
#ifdef WT_COMPILE_FACTORY_VERSION
		temp = BATT_TEMP_DEFAULT;
#else
		temp = BATT_TEMP_MIN;
#endif
	} else if (temp >= BATT_TEMP_MAX) {
		temp = BATT_TEMP_MAX;
	}

	if (temp != BATT_TEMP_0) {
		chg->temp_zero_count = 0;
		chg->batt_temp = temp;
	} else {
		if ((chg->temp_zero_count++) >= TEMP_ZERO_MAX) {
			chg->temp_zero_count = 0;
			chg->batt_temp = temp;
		}
	}

	if (chg->batt_temp_debug_flag) {
		chg->batt_temp = chg->batt_debug_temp;
		dev_err(chg->dev,"lsw batt_temp_debug_flag=%d use batt_debug_temp=%d\n",
								chg->batt_temp_debug_flag, chg->batt_debug_temp);
	}

	return chg->batt_temp;
}

static int wtchg_get_batt_capacity(struct wt_chg *chg)
{
	int ret;
	int capacity;
	union power_supply_propval val = {0,};

	//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	if(is_device_suspended(chg)){
		pr_err("wtchg_get_batt_capacity is_device_suspended \n");
		return chg->batt_capacity;
	}
	//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend

	capacity = chg->batt_capacity;
	chg->bms_psy = power_supply_get_by_name("bms");
	if (chg->bms_psy) {
		ret = power_supply_get_property(chg->bms_psy,
							POWER_SUPPLY_PROP_CAPACITY, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't get POWER_SUPPLY_PROP_CAPACITY, ret=%d\n", ret);
		}

		capacity = val.intval;
	}

	if (capacity  > BATT_SOC_ZERO) {
		chg->soc_zero_count = 0;
		if(capacity > 100)
			chg->batt_capacity = BATT_CAPACITY_DEFAULT_VALUE;
		else
			chg->batt_capacity = capacity;
	} else {
		if ((chg->soc_zero_count++) >= SOC_ZERO_MAX) {
			chg->soc_zero_count = 0;
			chg->batt_capacity = capacity;
		}
	}

#ifdef WT_COMPILE_FACTORY_VERSION
	if (chg->batt_temp_true <= BATT_TEMP_MIN || chg->batt_temp_true > BATT_TEMP_MAX)
		chg->batt_capacity = BATT_CAPACITY_DEFAULT_VALUE;
#endif

	return chg->batt_capacity;
}

static int wtchg_get_batt_capacity_level(struct wt_chg *chg)
{
	if (chg->batt_capacity >= 100) {
		chg->batt_capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
	} else if (chg->batt_capacity >= 80 && chg->batt_capacity < 100) {
		chg->batt_capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
	} else if (chg->batt_capacity >= 20 && chg->batt_capacity < 80) {
		chg->batt_capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
	} else if (chg->batt_capacity > 0 && chg->batt_capacity < 20) {
		chg->batt_capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
	} else if (chg->batt_capacity == 0) {
		if ((chg->usb_online || chg->ac_online)
				&& (chg->batt_volt >= SHUTDOWN_BATT_VOLT)) {
			chg->batt_capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		} else {
			chg->batt_capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		}
	} else {
		chg->batt_capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
	}

	//dev_err(chg->dev,"batt_capacity_level = %d\n", chg->batt_capacity_level);

	return chg->batt_capacity_level;
}

static int wtchg_check_battery_exist(struct wt_chg *chg)
{
	int ret = 0;

	if (chg->chg_status == POWER_SUPPLY_STATUS_FULL
			&& chg->batt_temp <= BATT_TEMP_MIN) {
		chg->battery_exist = false;
	}

	if (!chg->battery_exist) {
		wtchg_set_slave_input_current(chg, FCC_500_MA);
		wtchg_set_slave_chg_enable(chg, false);
		wtchg_set_slave_input_suspend(chg, true);
		wtchg_set_slave_chg_monitor(chg, false);

		wtchg_set_main_chg_current(chg, FCC_500_MA);
		wtchg_set_main_input_current(chg, FCC_500_MA);
		wtchg_set_main_chg_enable(chg, false);
		wtchg_set_main_input_suspend(chg, true);
	}

	return ret;
}

static int wtchg_get_batt_health(struct wt_chg *chg)
{
	int ret = 0;

	chg->batt_temp = wtchg_get_batt_temp(chg);
	if (chg->batt_temp >= BATT_TEMP_MIN_THRESH &&
			chg->batt_temp < BATT_TEMP_0_THRESH) {
		chg->batt_health = POWER_SUPPLY_HEALTH_COLD;
	} else if (chg->batt_temp >= BATT_TEMP_0_THRESH &&
			chg->batt_temp < BATT_TEMP_15_THRESH) {
		chg->batt_health = POWER_SUPPLY_HEALTH_COOL;
	} else if (chg->batt_temp >= BATT_TEMP_15_THRESH &&
			chg->batt_temp < BATT_TEMP_45_THRESH) {
		chg->batt_health = POWER_SUPPLY_HEALTH_GOOD;
	} else if (chg->batt_temp >= BATT_TEMP_45_THRESH &&
			chg->batt_temp < BATT_TEMP_50_THRESH) {
		chg->batt_health = POWER_SUPPLY_HEALTH_WARM;
	} else if (chg->batt_temp >= BATT_TEMP_50_THRESH) {
		chg->batt_health = POWER_SUPPLY_HEALTH_HOT;
	} else {
		// out of range
		dev_err(chg->dev, "batt temp: out of range, ret=%d\n", ret);
	}

	return chg->batt_health;
}

static void wtchg_init_chg_parameter(struct wt_chg *chg)
{
	chg->chg_type_ibatt = FCC_0_MA;
	chg->chg_type_input_curr = FCC_0_MA;
	chg->chg_type_input_curr_pre = -1;

	chg->chg_ibatt = FCC_0_MA;
	chg->chg_ibatt_pre = -1;

	chg->jeita_ibatt = FCC_0_MA;
	chg->jeita_batt_cv = BATT_NORMAL_CV;
	chg->jeita_batt_cv_pre = FCC_0_MA;

	chg->pre_vbus = DEFAULT_HVDCP_VOLT;

	if (chg->chg_init_flag == false) {
		chg->chg_init_flag = true;
		wtchg_set_slave_input_current(chg, FCC_500_MA);
		wtchg_set_slave_chg_enable(chg, false);
		wtchg_set_slave_input_suspend(chg, true);
		wtchg_set_slave_chg_monitor(chg, false);

		wtchg_set_main_chg_current(chg, FCC_500_MA);
		wtchg_set_main_input_current(chg, FCC_500_MA);
	}
}

static int wtchg_hvdcp30_request_adapter_voltage(struct wt_chg *chg, int chr_volt)
{
	int ret = 0;
	int retry_cnt = 0;
	int retry_cnt_max = 3;
	int vchr_before, vchr_after, vchr_delta;

	do {
		retry_cnt++;
		vchr_before = wtchg_get_vbus_voltage(chg);
		wtchg_set_hvdcp_voltage(chg, chg->pre_vbus);
		ret = wtchg_set_adapter_voltage(chg, chr_volt);
		msleep(3000);
		vchr_after = wtchg_get_vbus_voltage(chg);
		wtchg_get_vbus_online(chg);
		vchr_delta = abs(vchr_after - chr_volt);
		if ((vchr_delta < 1000) && (ret == 0)) {
			chg->pre_vbus = chr_volt;
			dev_err(chg->dev, "%s ok !: vchr = (%d, %d), vchr_target:%d\n",
						__func__, vchr_before, vchr_after, chr_volt);	
			return ret;
		}

		dev_err(chg->dev, "%s: retry_cnt = (%d, %d), vchr_before:%d, vchr_after:%d\n",
						__func__, retry_cnt, retry_cnt_max, vchr_before, vchr_after);
	} while (chg->vbus_online && (retry_cnt < retry_cnt_max));

	return ret;
}

static void wtchg_hvdcp30_start_algorithm(struct wt_chg *chg)
{
	int idx, size;
	int vbus;
	int vbat;

	vbat = wtchg_get_batt_volt(chg) /1000;
	size = ARRAY_SIZE(hvdcp30_profile);
	for (idx = 0; idx < size; idx++) {
		if (vbat > (hvdcp30_profile[idx].vbat + 100))
			continue;

		vbus = hvdcp30_profile[idx].vchr;
		wtchg_hvdcp30_request_adapter_voltage(chg, vbus);
		break;
	}

	wtchg_set_chg_input_current(chg);
	wtchg_set_chg_ibat_current(chg);

}

static int wtchg_set_hvdcp20_mode(struct wt_chg *chg, int mode)
{
	int ret = 0;
	union power_supply_propval val = {0,};

	val.intval = mode;
	chg->hvdcp_psy = power_supply_get_by_name("wt_hvdcp");
	if (chg->hvdcp_psy) {
		ret = power_supply_set_property(chg->hvdcp_psy,
							POWER_SUPPLY_PROP_HVDCP_OPTI_ALLOWED, &val);
		if (ret < 0) {
			dev_err(chg->dev,
				"Couldn't set POWER_SUPPLY_PROP_HVDCP_OPTI_ALLOWED, ret=%d\n", ret);
		}
	}

	return ret;
}

static void wtchg_hvdcp20_start_algorithm(struct wt_chg *chg)
{
	int retry_cnt = 0;
	int retry_cnt_max = 3;
	int vchr_before, vchr_after;

	wtchg_get_vbus_voltage(chg);
	wtchg_get_batt_volt(chg);
	if (chg->vbus_volt <= HVDCP20_VOLT_6V) {
		do {
			retry_cnt++;
			vchr_before = wtchg_get_vbus_voltage(chg);
			wtchg_set_hvdcp20_mode(chg, WT_HVDCP20_9V);
			msleep(500);
			vchr_after = wtchg_get_vbus_voltage(chg);
			wtchg_get_vbus_online(chg);

			dev_err(chg->dev, "hvdcp20: retry_cnt = (%d, %d), vchr_before:%d, vchr_after:%d\n",
							retry_cnt, retry_cnt_max, vchr_before, vchr_after);
		} while (chg->vbus_online && (retry_cnt < retry_cnt_max)
						&& (vchr_after < HVDCP20_VOLT_6V));
	}

	wtchg_set_chg_input_current(chg);
	wtchg_set_chg_ibat_current(chg);
}

static void wtchg_sw_jeita_state_machine(struct wt_chg *chg)
{
	int batt_jeita_stat = BATT_TEMP_NORMAL;
	int batt_prev_low_temp = 0;
	int batt_prev_high_temp = 0;
	int temp_low_hysteresis = 0;
	int temp_high_hysteresis = 0;

	//check battery thermal status
	chg->batt_temp = wtchg_get_batt_temp(chg);
	if (chg->batt_temp > BATT_TEMP_MIN_THRESH &&
		chg->batt_temp < BATT_TEMP_0_THRESH) {
		batt_jeita_stat = BATT_TEMP_COLD;
	} else if (chg->batt_temp >= BATT_TEMP_0_THRESH &&
		chg->batt_temp < BATT_TEMP_15_THRESH) {
		batt_jeita_stat = BATT_TEMP_COOL;
	} else if (chg->batt_temp >= BATT_TEMP_15_THRESH &&
		chg->batt_temp < BATT_TEMP_45_THRESH) {
		batt_jeita_stat = BATT_TEMP_NORMAL;
	} else if (chg->batt_temp >= BATT_TEMP_45_THRESH &&
		chg->batt_temp < BATT_TEMP_50_THRESH) {
		batt_jeita_stat = BATT_TEMP_WARM;
	} else if (chg->batt_temp >= BATT_TEMP_50_THRESH) {
		batt_jeita_stat = BATT_TEMP_HOT;
	} else {
		// out of range
	}

	//check and set battery thermal hysteresis
	if (batt_jeita_stat != chg->batt_jeita_stat_prev) {
		switch (chg->batt_jeita_stat_prev) {
		case BATT_TEMP_COLD:
			batt_prev_low_temp = BATT_TEMP_MIN_THRESH;
			batt_prev_high_temp = BATT_TEMP_0_THRESH;
			temp_low_hysteresis = 0;
			temp_high_hysteresis = (temp_high_hysteresis + BATT_TEMP_HYSTERESIS);
			break;
		case BATT_TEMP_COOL:
			batt_prev_low_temp = BATT_TEMP_0_THRESH;
			batt_prev_high_temp = BATT_TEMP_15_THRESH;
			temp_low_hysteresis = 0;
			temp_high_hysteresis = (temp_high_hysteresis + BATT_TEMP_HYSTERESIS);
			break;
		case BATT_TEMP_NORMAL:
			batt_prev_low_temp = BATT_TEMP_15_THRESH;
			batt_prev_high_temp = BATT_TEMP_45_THRESH;
			temp_low_hysteresis = 0;
			temp_high_hysteresis = 0;
			break;
		case BATT_TEMP_WARM:
			batt_prev_low_temp = BATT_TEMP_45_THRESH;
			batt_prev_high_temp = BATT_TEMP_50_THRESH;
			temp_low_hysteresis = (temp_low_hysteresis - BATT_TEMP_HYSTERESIS);
			temp_high_hysteresis = 0;
			break;
		case BATT_TEMP_HOT:
			batt_prev_low_temp = BATT_TEMP_50_THRESH;
			batt_prev_high_temp = BATT_TEMP_MAX_THRESH;
			temp_low_hysteresis = (temp_low_hysteresis - BATT_TEMP_HYSTERESIS);
			temp_high_hysteresis = 0;
			break;
		default:
			dev_err(chg->dev, "batt_jeita_stat_prev error !\n");
		}

		dev_err(chg->dev, "batt_jeita_stat_prev=%d, batt_jeita_stat=%d\n",
								chg->batt_jeita_stat_prev, batt_jeita_stat);
		if ((chg->batt_temp > (batt_prev_low_temp + temp_low_hysteresis)) &&
			(chg->batt_temp < (batt_prev_high_temp + temp_high_hysteresis))) {
			batt_jeita_stat = chg->batt_jeita_stat_prev;
		} else {
			chg->batt_jeita_stat_prev = batt_jeita_stat;
		}
	}

	wtchg_fast_charger(chg);

	if (batt_jeita_stat == BATT_TEMP_COLD) {
		chg->jeita_ibatt = FCC_0_MA;
		chg->jeita_batt_cv = BATT_NORMAL_CV;
	} else if (batt_jeita_stat == BATT_TEMP_COOL) {

		if(chg->fast_charger && chg->lcd_on) {
				chg->jeita_ibatt = FCC_2000_MA;
				chg->jeita_batt_cv = BATT_NORMAL_CV;
		} else{
			chg->jeita_ibatt = FCC_1500_MA;
			chg->jeita_batt_cv = BATT_NORMAL_CV;
		}

	} else if (batt_jeita_stat == BATT_TEMP_NORMAL) {
		chg->jeita_batt_cv = BATT_NORMAL_CV;
#if defined(DISABLE_BOARD_TEMP_CONTROL)
		chg->jeita_ibatt = FCC_4000_MA;
#else
		if (chg->board_temp < BOARD_TEMP_41_THRESH) {
			if(chg->fast_charger && chg->lcd_on) {
				if(chg->batt_temp < BATT_TEMP_29_THRESH ){
					chg->jeita_ibatt = FCC_3000_MA;
				} else if(BATT_TEMP_29_THRESH =< chg->batt_temp < BATT_TEMP_33_THRESH){
					chg->jeita_ibatt = FCC_2000_MA;
				} else if(BATT_TEMP_33_THRESH =< chg->batt_temp < BATT_TEMP_35_THRESH){
					chg->jeita_ibatt = FCC_1100_MA;
				} else {
					chg->jeita_ibatt = FCC_700_MA;
				}
			} else{
				if (chg->lcd_on) {
					chg->jeita_ibatt = FCC_2200_MA;
				} else {
					chg->jeita_ibatt = FCC_3600_MA;
				}
			}
		} else if ((chg->board_temp >= BOARD_TEMP_41_THRESH)
					&& (chg->board_temp < BOARD_TEMP_43_THRESH)) {
			if(chg->fast_charger && chg->lcd_on) {
				if(chg->batt_temp < BATT_TEMP_29_THRESH ){
					chg->jeita_ibatt = FCC_3000_MA;
				} else if(BATT_TEMP_29_THRESH =< chg->batt_temp < BATT_TEMP_33_THRESH){
					chg->jeita_ibatt = FCC_2000_MA;
				} else if(BATT_TEMP_33_THRESH =< chg->batt_temp < BATT_TEMP_35_THRESH){
					chg->jeita_ibatt = FCC_1100_MA;
				} else {
					chg->jeita_ibatt = FCC_700_MA;
				}
			} else{
				if (chg->lcd_on) {
					chg->jeita_ibatt = FCC_1800_MA;
				} else {
					chg->jeita_ibatt = FCC_2600_MA;
				}
			}
		} else if ((chg->board_temp >= BOARD_TEMP_43_THRESH)
					&& (chg->board_temp < BOARD_TEMP_45_THRESH)) {
			if(chg->fast_charger && chg->lcd_on) {
				if(chg->batt_temp < BATT_TEMP_29_THRESH ){
					chg->jeita_ibatt = FCC_3000_MA;
				} else if(BATT_TEMP_29_THRESH =< chg->batt_temp < BATT_TEMP_33_THRESH){
					chg->jeita_ibatt = FCC_2000_MA;
				} else if(BATT_TEMP_33_THRESH =< chg->batt_temp < BATT_TEMP_35_THRESH){
					chg->jeita_ibatt = FCC_1100_MA;
				} else {
					chg->jeita_ibatt = FCC_700_MA;
				}
			} else{
				if (chg->lcd_on) {
					chg->jeita_ibatt = FCC_1200_MA;
				} else {
					chg->jeita_ibatt = FCC_2200_MA;
				}
			}
		} else if ((chg->board_temp >= BOARD_TEMP_45_THRESH)
					&& (chg->board_temp < BOARD_TEMP_47_THRESH)) {
			if(chg->fast_charger && chg->lcd_on) {
				if(chg->batt_temp < BATT_TEMP_29_THRESH ){
					chg->jeita_ibatt = FCC_3000_MA;
				} else if(BATT_TEMP_29_THRESH =< chg->batt_temp < BATT_TEMP_33_THRESH){
					chg->jeita_ibatt = FCC_2000_MA;
				} else if(BATT_TEMP_33_THRESH =< chg->batt_temp < BATT_TEMP_35_THRESH){
					chg->jeita_ibatt = FCC_1100_MA;
				} else {
					chg->jeita_ibatt = FCC_700_MA;
				}
			} else{
				if (chg->lcd_on) {
					chg->jeita_ibatt = FCC_800_MA;
				} else {
					chg->jeita_ibatt = FCC_1600_MA;
				}
			}
		} else if (chg->board_temp >= BOARD_TEMP_47_THRESH) {
			if(chg->fast_charger && chg->lcd_on) {
				if(chg->batt_temp < BATT_TEMP_29_THRESH ){
					chg->jeita_ibatt = FCC_3000_MA;
				} else if(BATT_TEMP_29_THRESH =< chg->batt_temp < BATT_TEMP_33_THRESH){
					chg->jeita_ibatt = FCC_2000_MA;
				} else if(BATT_TEMP_33_THRESH =< chg->batt_temp < BATT_TEMP_35_THRESH){
					chg->jeita_ibatt = FCC_1100_MA;
				} else {
					chg->jeita_ibatt = FCC_700_MA;
				}
			} else{
				if (chg->lcd_on) {
					chg->jeita_ibatt = FCC_500_MA;
				} else {
					chg->jeita_ibatt = FCC_1000_MA;
				}
			}
		}
#endif
	} else if (batt_jeita_stat == BATT_TEMP_WARM) {
		chg->jeita_batt_cv = BATT_HIGH_TEMP_CV;
#if defined(DISABLE_BOARD_TEMP_CONTROL)
		chg->jeita_ibatt = FCC_1500_MA;
#else
		if (chg->board_temp < BOARD_TEMP_47_THRESH) {
			if(chg->fast_charger && chg->lcd_on){
				chg->jeita_ibatt = FCC_0_MA;
			} else{
				if (chg->lcd_on) {
					chg->jeita_ibatt = FCC_500_MA;
				} else {
					chg->jeita_ibatt = FCC_1000_MA;
				}
			}
		} else if ((chg->board_temp >= BOARD_TEMP_47_THRESH)
					&& (chg->board_temp < BOARD_TEMP_58_THRESH)) {
			if(chg->fast_charger && chg->lcd_on){
				chg->jeita_ibatt = FCC_0_MA;
			} else{
				chg->jeita_ibatt = FCC_500_MA;
			}
		} else if (chg->board_temp >= BOARD_TEMP_58_THRESH) {
			chg->jeita_ibatt = FCC_0_MA;
		}
#endif
	} else if (batt_jeita_stat == BATT_TEMP_HOT) {
		chg->jeita_ibatt = FCC_0_MA;
		chg->jeita_batt_cv = BATT_HIGH_TEMP_CV;
	}

#ifdef CONFIG_DISABLE_TEMP_PROTECT
	chg->jeita_batt_cv = BATT_HIGH_TEMP_CV;
#endif

	dev_err(chg->dev,  "batt_temp=%d, jeita_status=%d, board_temp=%d,lcd_on=%d\n",
				chg->batt_temp, batt_jeita_stat, chg->board_temp, chg->lcd_on);
}

static void wtchg_charge_strategy_machine(struct wt_chg *chg)
{

	//check and set battery thermal hysteresis
	wtchg_sw_jeita_state_machine(chg);

	switch (chg->real_type) {
	case POWER_SUPPLY_TYPE_USB_PD:
		chg->chg_type_ibatt = FCC_4000_MA;
		chg->chg_type_input_curr = FCC_2200_MA;
		break;
	case POWER_SUPPLY_TYPE_USB_HVDCP:
		chg->chg_type_ibatt = FCC_4000_MA;
		chg->chg_type_input_curr = FCC_2000_MA;
		break;
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
	case POWER_SUPPLY_TYPE_USB_HVDCP_3P5:
		chg->chg_type_ibatt = FCC_4000_MA;
		chg->chg_type_input_curr = FCC_2000_MA;
		break;
	case POWER_SUPPLY_TYPE_USB_DCP:
		chg->chg_type_ibatt = FCC_2000_MA;
		chg->chg_type_input_curr = FCC_2000_MA;
		break;
	case POWER_SUPPLY_TYPE_USB_CDP:
		chg->chg_type_ibatt = FCC_1500_MA;
		chg->chg_type_input_curr = FCC_1500_MA;
		break;
	case POWER_SUPPLY_TYPE_USB:
		chg->chg_type_ibatt = FCC_500_MA;
		chg->chg_type_input_curr = FCC_500_MA;
		break;
	case POWER_SUPPLY_TYPE_USB_FLOAT:
		chg->chg_type_ibatt = FCC_500_MA;
		chg->chg_type_input_curr = FCC_500_MA;
		break;
	default:
		chg->chg_type_ibatt = FCC_500_MA;
		chg->chg_type_input_curr = FCC_500_MA;
		break;
	}
	if(chg->typec_cc_orientation == 0 && chg->vbus_online)
	{
		chg->chg_type_ibatt = FCC_650_MA;
	}
	//choose smaller charging current
	if (chg->chg_type_ibatt <= chg->jeita_ibatt) {
		chg->chg_ibatt = chg->chg_type_ibatt;
	} else {
		chg->chg_ibatt = chg->jeita_ibatt;
	}

	if (chg->chg_ibatt > CHG_CURRENT_BYPASS_VALUE
		&& chg->batt_capacity < SLAVE_CHG_START_SOC_MAX
		&& chg->ato_soc_control == false) {
		chg->slave_chg_enable = true;
	} else {
		chg->slave_chg_enable = false;
	}

	dev_err(chg->dev,  "jeita_batt_cv=%d, jeita_batt_cv_pre=%d, input_curr=%d, input_curr_pre=%d, slave_enable=%d\n",
				chg->jeita_batt_cv, chg->jeita_batt_cv_pre,
				chg->chg_type_input_curr, chg->chg_type_input_curr_pre,
				chg->slave_chg_enable);

	//set charge cv value
	if (chg->jeita_batt_cv  != chg->jeita_batt_cv_pre) {
		chg->jeita_batt_cv_pre = chg->jeita_batt_cv;
		wtchg_set_main_chg_volt(chg, chg->jeita_batt_cv);
		wtchg_set_slave_chg_volt(chg, chg->jeita_batt_cv);
	} 

	switch (chg->real_type) {
	case POWER_SUPPLY_TYPE_USB_HVDCP:
		wtchg_hvdcp20_start_algorithm(chg);
		break;
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
	case POWER_SUPPLY_TYPE_USB_HVDCP_3P5:
		wtchg_hvdcp30_start_algorithm(chg);
		break;
	case POWER_SUPPLY_TYPE_USB:
	case POWER_SUPPLY_TYPE_USB_DCP:
	case POWER_SUPPLY_TYPE_USB_CDP:
	case POWER_SUPPLY_TYPE_USB_FLOAT:
		wtchg_set_chg_input_current(chg);
		wtchg_set_chg_ibat_current(chg);
		break;
	case POWER_SUPPLY_TYPE_USB_PD:
		wtchg_set_chg_input_current(chg);
		wtchg_set_chg_ibat_current(chg);
		break;

	default:
		break;
	}
}

static void wtchg_set_charge_work(struct wt_chg *chg)
{
	dev_err(chg->dev, "%s: start !!\n", __func__);
	//+ ExtB oak-3798, tankaikun@wt, add 20220126, fix plug in charger slowly cause identify float type
	if(chg->vbus_online && chg->float_recheck_cnt > 0 && chg->real_type == POWER_SUPPLY_TYPE_USB_FLOAT) {
		return;
	}
	//- ExtB oak-3798, tankaikun@wt, add 20220126, fix plug in charger slowly cause identify float type 

	// ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	alarm_start_relative(&chg->chg_main_alarm, ms_to_ktime(WT_CHARGER_ALARM_TIME));

	cancel_delayed_work_sync(&chg->wt_chg_work);
	schedule_delayed_work(&chg->wt_chg_work, 0);
}

static int wtchg_batt_init_config(struct wt_chg *chg)
{
	int ret = 0;

	chg->usb_online = false;
	chg->ac_online = false;

	chg->batt_health = POWER_SUPPLY_HEALTH_GOOD;
	chg->batt_temp = DEFAULT_BATT_TEMP;
	chg->batt_temp_true = DEFAULT_BATT_TEMP;
	chg->batt_capacity = 1;
	chg->batt_volt = 3800000;
	chg->jeita_batt_cv = chg->batt_cv_max;
	chg->chg_iterm = 350;

	chg->chg_init_flag = true;
	chg->maintain_soc_control = false;
	chg->protected_soc_control = false;
	chg->ato_soc_control = false;
	chg->lcd_on = false;

	chg->batt_temp_debug_flag = false;
	chg->board_temp_debug_flag = false;
	chg->usb_temp_debug_flag = false;

	chg->pre_vbus = DEFAULT_HVDCP_VOLT;
	chg->ship_mode = 0;
	chg->battery_exist = true;

	//+ ExtB oak-3798, tankaikun@wt, add 20220126, fix plug in charger slowly cause identify float type
	chg->float_recheck_cnt = 0;

	// ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	chg->resume_completed = true;

	//+ ExtB oak-3916, tankaikun@wt, add 20220209, modify battery protect mode
	chg->batt_protected_mode_disable_charge = false;
	chg->batt_protected_mode_disable_suspend = false;

	chg->typec_pr_role = POWER_SUPPLY_TYPEC_PR_NONE;
	//- ExtB oak-3916, tankaikun@wt, add 20220209, modify battery protect mode

	return ret;
}

static int wtchg_batt_set_system_temp_level(struct wt_chg *chg,
				const union power_supply_propval *val)
{
	int cur;

	if (val->intval < 0)
		return -EINVAL;

	if (chg->thermal_levels <= 0)
		return -EINVAL;

	if (val->intval > chg->thermal_levels)
		return -EINVAL;

	chg->system_temp_level = val->intval;
	cur = thermal_mitigation[val->intval] / 1000;

	pr_err("batt_set_prop_system_temp_levels %d, cur = %d\n", chg->system_temp_level, cur);

	return 0;
}

static int wtchg_ato_charge_manage(struct wt_chg *chg)
{
	int ret = 0;

#ifdef WT_COMPILE_FACTORY_VERSION
	if (chg->batt_capacity >= ATO_BATT_SOC_MAX && !chg->otg_enable) {
		chg->ato_soc_control = true;
		wtchg_set_main_chg_enable(chg, false);
		wtchg_set_main_input_suspend(chg, true);

		wtchg_set_slave_chg_enable(chg, false);
		wtchg_set_slave_input_suspend(chg, true);
		dev_err(chg->dev, "ATO version capacity: %d, ato_soc_flag: %d\n",
								chg->batt_capacity, chg->ato_soc_control);
	} else if (chg->batt_capacity <= ATO_BATT_SOC_MIN && chg->ato_soc_control) {
		chg->ato_soc_control = false;
		wtchg_set_main_chg_enable(chg, true);
		wtchg_set_main_input_suspend(chg, false);

		if (chg->slave_chg_enable) {
			wtchg_set_slave_chg_enable(chg, true);
			wtchg_set_slave_input_suspend(chg, false);
		}
		dev_err(chg->dev, "ATO version capacity: %d, ato_soc_flag: %d\n",
								chg->batt_capacity, chg->ato_soc_control);
	}
#endif

	return ret;
}

static int wtchg_battery_maintain_charge_manage(struct wt_chg *chg)
{
	int ret = 0;

	if ((chg->batt_maintain_mode == true)
			&& (chg->batt_capacity >= BATT_MAINTAIN_SOC_MAX)) {
		chg->maintain_soc_control = true;
		wtchg_set_main_chg_enable(chg, false);

		wtchg_set_slave_chg_enable(chg, false);
		wtchg_set_slave_input_suspend(chg, true);
		dev_err(chg->dev, "Maintain mode capacity: %d, maintain_soc_control: %d\n",
								chg->batt_capacity, chg->maintain_soc_control);
	} else if ((chg->batt_maintain_mode == true)
			&& (chg->batt_capacity <= BATT_MAINTAIN_SOC_MIN)
			&& (chg->maintain_soc_control)) {
		chg->maintain_soc_control = false;
		wtchg_set_main_chg_enable(chg, true);
		wtchg_set_main_input_suspend(chg, false);

		if (chg->slave_chg_enable) {
			wtchg_set_slave_chg_enable(chg, true);
			wtchg_set_slave_input_suspend(chg, false);
		}
		dev_err(chg->dev, "Maintain mode capacity: %d, maintain_soc_control: %d\n",
								chg->batt_capacity, chg->maintain_soc_control);
	}

	return ret;
}

static int wtchg_battery_protect_charge_manage(struct wt_chg *chg)
{
	int ret = 0;

//+ ExtB OAK-3916, tankaikun@wt, add 20220209, modify battery protect mode
	if(chg->batt_protected_mode == true){
		if ((chg->batt_capacity >= BATT_PROTECT_SOC_MIN) 
				&& (chg->batt_capacity < BATT_PROTECT_SOC_MAX)){
			chg->batt_protected_mode_disable_suspend = false;
		}else if(chg->batt_capacity < BATT_PROTECT_SOC_MIN){
			chg->batt_protected_mode_disable_charge = false;
			chg->batt_protected_mode_disable_suspend = false;
		}else if(chg->batt_capacity > BATT_PROTECT_SOC_MAX){
			chg->batt_protected_mode_disable_charge = true;
			chg->batt_protected_mode_disable_suspend = true;
		}else if(chg->batt_capacity == BATT_PROTECT_SOC_MAX){
			chg->batt_protected_mode_disable_charge = true;
			chg->batt_protected_mode_disable_suspend = false;
		}

		if(!(chg->usb_online ||chg->ac_online) && (chg->typec_pr_role == POWER_SUPPLY_TYPEC_PR_SINK)
			&& chg->batt_capacity <= BATT_PROTECT_SOC_MAX){
				dev_err(chg->dev, "vbus online and disable charge input suspend \n");
				wtchg_set_main_input_suspend(chg, false);
		}
	} else{
		chg->batt_protected_mode_disable_charge = false;
		chg->batt_protected_mode_disable_suspend = false;
	}

	dev_err(chg->dev, "batt_protected_mode=%d batt_protected_mode_disable_charge=%d batt_protected_mode_disable_suspend=%d \n",
					chg->batt_protected_mode, chg->batt_protected_mode_disable_charge, chg->batt_protected_mode_disable_suspend);

#if 0
	if ((chg->batt_protected_mode == true)
			&& (chg->batt_capacity >= BATT_PROTECT_SOC_MAX)) {
		chg->protected_soc_control = true;
		wtchg_set_main_chg_enable(chg, false);
		wtchg_set_main_input_suspend(chg, true);

		wtchg_set_slave_chg_enable(chg, false);
		wtchg_set_slave_input_suspend(chg, true);
		dev_err(chg->dev, "Protect mode: capacity: %d, protected_soc_control: %d\n",
								chg->batt_capacity, chg->protected_soc_control);
	} else if ((chg->batt_protected_mode == true)
			&& (chg->batt_capacity <= BATT_PROTECT_SOC_MIN)
			&& (chg->protected_soc_control)) {
		chg->protected_soc_control = false;
		wtchg_set_main_chg_enable(chg, true);
		wtchg_set_main_input_suspend(chg, false);

		if (chg->slave_chg_enable) {
			wtchg_set_slave_chg_enable(chg, true);
			wtchg_set_slave_input_suspend(chg, false);
		}
		dev_err(chg->dev, "Protect mode: capacity: %d,  protected_soc_control: %d\n",
								chg->batt_capacity, chg->protected_soc_control);
	}
#endif
//- ExtB OAK-3916, tankaikun@wt, add 20220209, modify battery protect mode

	return ret;
}

//+ ExtB oak-3798, tankaikun@wt, add 20220126, fix plug in charger slowly cause identify float type
static int wtchg_float_charge_type_check(struct wt_chg *chg)
{
	int ret = 0;
	union power_supply_propval val = {0,};

	dev_err(chg->dev, "wtchg_float_charge_type_check vbus_online=%d real_type=%d float_recheck_cnt=%d\n", 
				chg->vbus_online , chg->real_type, chg->float_recheck_cnt);

	if(chg->vbus_online == 1 && chg->real_type == 16){
		pr_err("wtchg_float_charge_type_check recheck \n");
		chg->float_recheck_cnt++;
		chg->hvdcp_psy = power_supply_get_by_name("wt_hvdcp");
		if (chg->hvdcp_psy) {
			ret = power_supply_set_property(chg->hvdcp_psy,
								POWER_SUPPLY_PROP_APSD_RERUN, &val);
			if (ret < 0) {
				dev_err(chg->dev,
					"Couldn't set POWER_SUPPLY_PROP_APSD_RERUN, ret=%d\n", ret);
			}
		}
	}else if(chg->vbus_online == 0){
		chg->float_recheck_cnt = 0;
	}

	return ret;
}
//- ExtB oak-3798, tankaikun@wt, add 20220126, fix plug in charger slowly cause identify float type

//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
static enum alarmtimer_restart chg_main_alarm_cb(struct alarm *alarm,
								ktime_t now)
{
	struct wt_chg *chg = container_of(alarm, struct wt_chg,
							chg_main_alarm);

	dev_err(chg->dev, "chg_main_alarm_cb wt chg alarm triggered %lld\n", ktime_to_ms(now));

	if(chg->vbus_online || (chg->typec_pr_role == POWER_SUPPLY_TYPEC_PR_SINK))
		alarm_start_relative(&chg->chg_main_alarm, ms_to_ktime(WT_CHARGER_ALARM_TIME));

	return ALARMTIMER_NORESTART;
}
//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend

static void wt_chg_main(struct work_struct *work)
{
	struct wt_chg *chg = container_of(work,
							struct wt_chg, wt_chg_work.work);
	unsigned long flags;
	ktime_t now, add, resume_time;

	//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	spin_lock_irqsave(&chg->ws_lock, flags);
	if (chg->main_chg_ws && !atomic_read(&chg->pm_awake_active)){
		__pm_stay_awake(chg->main_chg_ws);
		atomic_set(&chg->pm_awake_active, 1);
	}
	spin_unlock_irqrestore(&chg->ws_lock, flags);

	now = ktime_get_boottime();
	add = ktime_set(5000 / MSEC_PER_SEC,
				(5000 % MSEC_PER_SEC) * NSEC_PER_MSEC);
	resume_time = ktime_add(now, add);
	chg->resume_main_work_time = resume_time;
	dev_err(chg->dev, "wt_chg_main now = %lld resume_time = %lld \n", ktime_to_ms(now), ktime_to_ms(resume_time));

	if(is_device_suspended(chg)){
		pr_err("wt_chg_main device is suspended schedule next work \n");
		schedule_delayed_work(&chg->wt_chg_work, msecs_to_jiffies(chg->interval_time));

		spin_lock_irqsave(&chg->ws_lock, flags);
		if (chg->main_chg_ws && atomic_read(&chg->pm_awake_active))
			__pm_relax(chg->main_chg_ws);
		spin_unlock_irqrestore(&chg->ws_lock, flags);
		return ;
	}
	//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend

	wtchg_get_vbus_online(chg);
	wtchg_get_usb_online(chg);
	wtchg_get_ac_online(chg);
	wtchg_get_batt_status(chg);
	wtchg_get_batt_volt(chg);
	wtchg_get_batt_current(chg);
	wtchg_get_batt_temp(chg);
	wtchg_get_batt_health(chg);
	wtchg_get_batt_capacity(chg);
	wtchg_get_vbus_voltage(chg);
	wtchg_float_charge_type_check(chg); // ExtB oak-3798, tankaikun@wt, add 20220126, fix plug in charger slowly cause identify float type

	if (chg->usb_online ||chg->ac_online) {
		wtchg_check_battery_exist(chg);
		wtchg_get_main_chg_temp(chg, &chg->main_chg_temp);
		wtchg_get_usb_port_temp(chg, &chg->usb_port_temp);
		wtchg_get_board_pcb_temp(chg, &chg->board_temp);
		wtchg_battery_protect_charge_manage(chg);
		wtchg_charge_strategy_machine(chg);
		wtchg_battery_maintain_charge_manage(chg);
		wtchg_ato_charge_manage(chg);

		chg->chg_init_flag = false;
		chg->interval_time = CHARGE_ONLINE_INTERVAL_MS;
	} else {
		wtchg_init_chg_parameter(chg);
		chg->interval_time = CHARGE_ONLINE_INTERVAL_MS;
		wtchg_battery_protect_charge_manage(chg);
		wtchg_ato_charge_manage(chg);
	}

	wtchg_get_batt_status(chg);
	pr_err("chg_type: %d, vbus_online: %d, vbus_volt: %d, chg_status: %d, batt_volt: %d, batt_current: %d, temp: %d, health: %d, capacity: %d\n",
			chg->real_type, chg->vbus_online, chg->vbus_volt,
			chg->chg_status, chg->batt_volt, chg->chg_current,
			chg->batt_temp, chg->batt_health, chg->batt_capacity);

	pr_err("chg_temp: %d, usb_temp: %d, board_temp: %d, usb_online: %d, ac_online: %d, pd_active:%d typec_pr_role:%d \n",
			chg->main_chg_temp, chg->usb_port_temp, chg->board_temp,
			chg->usb_online, chg->ac_online, chg->pd_active, chg->typec_pr_role);

	/* Update userspace */
	if (chg->batt_psy)
		power_supply_changed(chg->batt_psy);

	if (chg->usb_psy)
		power_supply_changed(chg->usb_psy);

	if (chg->ac_psy)
		power_supply_changed(chg->ac_psy);

	schedule_delayed_work(&chg->wt_chg_work, msecs_to_jiffies(chg->interval_time));

	//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	spin_lock_irqsave(&chg->ws_lock, flags);
	if (chg->main_chg_ws && atomic_read(&chg->pm_awake_active))
		__pm_relax(chg->main_chg_ws);
	spin_unlock_irqrestore(&chg->ws_lock, flags);
	//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend 

}

static int wtchg_batt_parse_dt(struct wt_chg *chg)
{
	struct device_node *node = chg->dev->of_node;
	int ret = 0;

	if (!node) {
		pr_err("device tree node missing\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(node,
				"qcom,fv-max-uv", &chg->batt_cv_max);
	if (ret < 0)
		chg->batt_cv_max = 4432000;
	else
		pr_err("fv-max-uv: %d\n",chg->batt_cv_max);

	ret = of_property_read_u32(node,
				"qcom,fcc-max-ua", &chg->batt_fcc_max);
	if (ret < 0)
		chg->batt_fcc_max = 4000000;
	else
		pr_err("fcc-max-ua: %d\n",chg->batt_fcc_max);

	ret = of_property_read_u32(node,
				"qcom,batt_iterm", &chg->batt_iterm);
	if (ret < 0)
		chg->batt_iterm = 300000;
	else
		pr_err("batt_iterm: %d\n",chg->batt_iterm);

	ret = get_iio_channel(chg, "main_chg_therm", &chg->main_chg_therm);
	if (ret < 0) {
		pr_err("get main_chg_therm fail: %d\n", ret);
		//return ret;
	}

	ret = get_iio_channel(chg, "usb_port_therm", &chg->usb_port_therm);
	if (ret < 0) {
		pr_err("get usb_port_therm fail: %d\n", ret);
		//return ret;
	}

	ret = get_iio_channel(chg, "quiet_therm", &chg->board_pcb_therm);
	if (ret < 0) {
		pr_err("get board_pcb_therm fail: %d\n", ret);
		//return ret;
	}

	return 0;
};

static ssize_t show_startcharging_test(struct device *dev,
											struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	chg->start_charging_test = true;
	wtchg_set_main_chg_enable(chg, chg->start_charging_test);
	wtchg_set_main_input_suspend(chg, !chg->start_charging_test);
	dev_err(chg->dev, "set: startcharging_test = %d\n", chg->start_charging_test);

	return sprintf(buf, "startcharging_test = %d\n", chg->start_charging_test);
}

static ssize_t store_startcharging_test(struct device *dev,
											struct device_attribute *attr,
											const char *buf, size_t size)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	dev_err(chg->dev,  "Couldn't support write: startcharging_test !\n");

	return size;
}
static DEVICE_ATTR(StartCharging_Test, 0664, show_startcharging_test, store_startcharging_test);

static ssize_t show_stopcharging_test(struct device *dev,
											struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	chg->start_charging_test = false;
	wtchg_set_main_chg_enable(chg, chg->start_charging_test);
	wtchg_set_main_input_suspend(chg, !chg->start_charging_test);
	dev_err(chg->dev, "set: stopcharging_test = %d\n", chg->start_charging_test);

	return sprintf(buf, "stopcharging_test = %d\n", chg->start_charging_test);
}

static ssize_t store_stopcharging_test(struct device *dev,
											struct device_attribute *attr,
											const char *buf, size_t size)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	dev_err(chg->dev,  "Couldn't support write: stopcharging_test !\n");

	return size;
}
static DEVICE_ATTR(StopCharging_Test, 0664, show_stopcharging_test, store_stopcharging_test);


#define BATT_TEMP_LIMIT_H	700
#define BATT_TEMP_LIMIT_L	(-200)
static ssize_t show_batt_temp_test(struct device *dev,
											struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	return sprintf(buf, "real: batt_debug_temp = %d\n", chg->batt_debug_temp);
}

static ssize_t store_batt_temp_test(struct device *dev,
											struct device_attribute *attr,
											const char *buf, size_t size)
{
	int temp = 250;
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	if (kstrtoint(buf, 10, &temp) == 0) {
		if ((temp >= BATT_TEMP_LIMIT_L) && (temp <= BATT_TEMP_LIMIT_H)) {
			chg->batt_temp_debug_flag = true;
			chg->batt_debug_temp = temp;
		}
	}

	return size;
}
static DEVICE_ATTR(batt_temp_test, 0664, show_batt_temp_test, store_batt_temp_test);


#define BOARD_TEMP_LIMIT_H	700
#define BOARD_TEMP_LIMIT_L	(-200)
static ssize_t show_board_temp_test(struct device *dev,
											struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	return sprintf(buf, "board_debug_temp = %d\n", chg->board_debug_temp);
}

static ssize_t store_board_temp_test(struct device *dev,
											struct device_attribute *attr,
											const char *buf, size_t size)
{
	int temp = 250;
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	if (kstrtoint(buf, 10, &temp) == 0) {
		if ((temp >= BOARD_TEMP_LIMIT_L) && (temp <= BOARD_TEMP_LIMIT_H)) {
			chg->board_temp_debug_flag = true;
			chg->board_debug_temp = temp;
		}
	}

	return size;
}
static DEVICE_ATTR(board_temp_test, 0664, show_board_temp_test, store_board_temp_test);

static ssize_t show_board_temp(struct device *dev,
											struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	return sprintf(buf, "board_temp = %d\n", chg->board_temp);
}

static ssize_t store_board_temp(struct device *dev,
											struct device_attribute *attr,
											const char *buf, size_t size)
{
	return size;
}

static DEVICE_ATTR(board_temp, 0664, show_board_temp, store_board_temp);

static ssize_t show_main_chg_temp(struct device *dev,
											struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	return sprintf(buf, "main_chg_temp = %d\n", chg->main_chg_temp);
}

static ssize_t store_main_chg_temp(struct device *dev,
											struct device_attribute *attr,
											const char *buf, size_t size)
{
	return size;
}

static DEVICE_ATTR(main_chg_temp, 0664, show_main_chg_temp, store_main_chg_temp);

#define USB_TEMP_LIMIT_H	700
#define USB_TEMP_LIMIT_L	(-200)
static ssize_t show_usb_temp_test(struct device *dev,
											struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	return sprintf(buf, "usb_debug_temp = %d\n", chg->usb_debug_temp);
}

static ssize_t store_usb_temp_test(struct device *dev,
											struct device_attribute *attr,
											const char *buf, size_t size)
{
	int temp;
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	pr_debug("usb_debug_temp = %d\n", temp);
	if (kstrtoint(buf, 10, &temp) == 0) {
		if ((temp >= USB_TEMP_LIMIT_L) && (temp <= USB_TEMP_LIMIT_H)) {
			chg->usb_temp_debug_flag = true;
			chg->usb_debug_temp = temp;
		}
	}

	return size;
}
static DEVICE_ATTR(usb_temp_test, 0664, show_usb_temp_test, store_usb_temp_test);

#define HVDCP_FASTCHARGING_CURRENT	2000000
static ssize_t show_afc_flag(struct device *dev,
											struct device_attribute *attr, char *buf)
{
	int value;
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	value = wtchg_get_batt_current(chg);

	dev_err(chg->dev,  "batt current_now : %d\n", value);
	if (value >= HVDCP_FASTCHARGING_CURRENT){
		return sprintf(buf, "1\n");
	} else{
		return sprintf(buf, "0\n");
	}
}

static ssize_t store_afc_flag(struct device *dev,
											struct device_attribute *attr,
											const char *buf, size_t size)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	dev_err(chg->dev,  "store_afc_flag !\n");
	return 0;
}
static DEVICE_ATTR(afc_flag, 0664, show_afc_flag, store_afc_flag);

static ssize_t show_battery_maintain(struct device *dev,
											struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	return sprintf(buf, "%d\n", chg->batt_maintain_mode);
}

static ssize_t store_battery_maintain(struct device *dev,
											struct device_attribute *attr,
											const char *buf, size_t size)
{
	int maintain;
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	pr_debug("batt_maintain_mode = %d\n", maintain);
	if (kstrtoint(buf, 10, &maintain) == 0) {
		if (maintain == 0) {
			chg->batt_maintain_mode = false;
		} else if  (maintain == 1) {
			chg->batt_maintain_mode = true;
		}
	}

	return size;
}
static DEVICE_ATTR(battery_maintain, 0664, show_battery_maintain, store_battery_maintain);

static ssize_t show_battery_protected(struct device *dev,
											struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	return sprintf(buf, "%d\n", chg->batt_protected_mode);
}

static ssize_t store_battery_protected(struct device *dev,
											struct device_attribute *attr,
											const char *buf, size_t size)
{
	int protected;
	struct power_supply *psy = dev_get_drvdata(dev);
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	pr_debug("batt_protected_mode = %d\n", protected);
	if (kstrtoint(buf, 10, &protected) == 0) {
		if (protected == 0) {
			chg->batt_protected_mode = false;

			//+ ExtB OAK-3916, tankaikun@wt, add 20220209, add battery protect mode
			chg->batt_protected_mode_disable_charge = false;
			chg->batt_protected_mode_disable_suspend = false;

			if(wtchg_get_main_input_suspend(chg))
				wtchg_set_main_input_suspend(chg, false);
			//- ExtB OAK-3916, tankaikun@wt, add 20220209, add battery protect mode
		} else if  (protected == 1) {
			chg->batt_protected_mode = true;

			//+ ExtB OAK-3916, tankaikun@wt, add 20220209, add battery protect mode
			if ((chg->batt_protected_mode == true)
					&& (chg->batt_capacity >= BATT_PROTECT_SOC_MIN)
					&& (chg->batt_capacity <= BATT_PROTECT_SOC_MAX)){
				chg->batt_protected_mode_disable_charge = true;
				chg->batt_protected_mode_disable_suspend = false;
			}else if((chg->batt_protected_mode == true)
					&& (chg->batt_capacity < BATT_PROTECT_SOC_MIN)){
				chg->batt_protected_mode_disable_charge = false;
				chg->batt_protected_mode_disable_suspend = false;
			}else if((chg->batt_protected_mode == true)
					&& (chg->batt_capacity > BATT_PROTECT_SOC_MAX)){
				chg->batt_protected_mode_disable_charge = true;
				chg->batt_protected_mode_disable_suspend = true;
			}
			//- ExtB OAK-3916, tankaikun@wt, add 20220209, add battery protect mode
		}
	}

	return size;
}
static DEVICE_ATTR(battery_protected, 0664, show_battery_protected, store_battery_protected);


static enum power_supply_property wt_batt_props[] = {
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_SET_SHIP_MODE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
};

static int wt_batt_get_prop(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct wt_chg *chg = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = chg->chg_status;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = chg->batt_health;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		val->intval = 2000*1000;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = chg->batt_capacity;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = wtchg_get_batt_capacity_level(chg);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = chg->batt_volt;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = wtchg_get_batt_volt_max(chg);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = wtchg_get_batt_current(chg);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = wtchg_get_batt_current_max(chg);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		val->intval = wtchg_get_batt_iterm_max(chg);
		break;
	case POWER_SUPPLY_PROP_TEMP:
	#ifndef CONFIG_DISABLE_TEMP_PROTECT
		val->intval = chg->batt_temp;
	#else
		val->intval = DEFAULT_BATT_TEMP;
	#endif
		break;
	case POWER_SUPPLY_PROP_SET_SHIP_MODE:
		val->intval = chg->ship_mode;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = wtchg_get_batt_id_info(chg);
		break;
//+ExtB oak-2734, tankaikun@wt, add 20220113, vts getHealthInfo/0_default FAIL
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		val->intval = chg->batt_capacity * 7700 * 10; //(capacity / 100 * battery_mah * 1000)uah
		break;
//-ExtB oak-2734, tankaikun@wt, add 20220113, vts getHealthInfo/0_default FAIL
	default:
		pr_err("batt power supply prop %d not supported\n", psp);
		return -EINVAL;
	}

	if (ret < 0) {
		pr_debug("Couldn't get prop %d rc = %d\n", psp, ret);
		return -ENODATA;
	}

	return 0;
}

static int wt_batt_set_prop(struct power_supply *psy,
		enum power_supply_property prop,
		const union power_supply_propval *val)
{
	int ret = 0;
	struct wt_chg *chg = power_supply_get_drvdata(psy);

	switch (prop) {
	case POWER_SUPPLY_PROP_SET_SHIP_MODE:
		chg->ship_mode = val->intval;
		wtchg_set_slave_chg_ship_mode(chg);
		wtchg_set_main_chg_ship_mode(chg);
		break;

	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		//rc = smblib_set_prop_input_suspend(chg, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		ret = wtchg_batt_set_system_temp_level(chg, val);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		//rc = smblib_set_prop_batt_capacity(chg, val);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		//chg->batt_profile_fv_uv = val->intval;
		//vote(chg->fv_votable, BATT_PROFILE_VOTER, true, val->intval);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
	//	chg->batt_profile_fcc_ua = val->intval;
		//vote(chg->fcc_votable, BATT_PROFILE_VOTER, true, val->intval);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int wt_batt_prop_is_writeable(struct power_supply *psy,
		enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
	case POWER_SUPPLY_PROP_SET_SHIP_MODE:
		return 1;
	default:
		break;
	}

	return 0;
}

static const struct power_supply_desc batt_psy_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = wt_batt_props,
	.num_properties = ARRAY_SIZE(wt_batt_props),
	.get_property = wt_batt_get_prop,
	.set_property = wt_batt_set_prop,
	.property_is_writeable = wt_batt_prop_is_writeable,
};

static int wt_init_batt_psy(struct wt_chg *chg)
{
	struct power_supply_config batt_cfg = {};
	int ret = 0;

	if(!chg) {
		pr_err("chg is NULL\n");
		return ret;
	}

	batt_cfg.drv_data = chg;
	batt_cfg.of_node = chg->dev->of_node;
	chg->batt_psy = devm_power_supply_register(chg->dev,
					   &batt_psy_desc,
					   &batt_cfg);
	if (IS_ERR(chg->batt_psy)) {
		pr_err("Couldn't register battery power supply\n");
		return PTR_ERR(chg->batt_psy);
	}

	if (device_create_file(&chg->batt_psy->dev, &dev_attr_StartCharging_Test)) {
		pr_err("Couldn't create file: batt_temp_test !\n");
	}

	if (device_create_file(&chg->batt_psy->dev, &dev_attr_StopCharging_Test)) {
		pr_err("Couldn't create file: board_temp_test !\n");
	}

	if (device_create_file(&chg->batt_psy->dev, &dev_attr_batt_temp_test)) {
		pr_err("Couldn't create file: batt_temp_test !\n");
	}

	if (device_create_file(&chg->batt_psy->dev, &dev_attr_board_temp_test)) {
		pr_err("Couldn't create file: board_temp_test !\n");
	}

	if (device_create_file(&chg->batt_psy->dev, &dev_attr_board_temp)) {
		pr_err("Couldn't create file: board_temp !\n");
	}

	if (device_create_file(&chg->batt_psy->dev, &dev_attr_main_chg_temp)) {
		pr_err("Couldn't create file: main_chg_temp !\n");
	}

	if (device_create_file(&chg->batt_psy->dev, &dev_attr_battery_maintain)) {
		pr_err("Couldn't create file: maintain_mode !\n");
	}

	if (device_create_file(&chg->batt_psy->dev, &dev_attr_battery_protected)) {
		pr_err("Couldn't create file: battery_protected !\n");
	}

	return ret;
}


/************************
 * AC PSY REGISTRATION *
 ************************/
static enum power_supply_property wt_ac_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_TYPE,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
};

static int wt_ac_get_prop(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	int ret = 0;
	struct wt_chg *chg = power_supply_get_drvdata(psy);
	val->intval = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		//if(main_get_charge_type() > 0)
		//	val->intval = 1;
		//else
			val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = wtchg_get_ac_online(chg);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = wtchg_get_usb_max_voltage(chg);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = wtchg_get_usb_max_voltage(chg);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		wtchg_get_vbus_voltage(chg);
		val->intval = chg->vbus_volt * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
//		rc = smblib_get_prop_usb_current_now(chg, val);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = wtchg_get_usb_max_current(chg);
		break;
	case POWER_SUPPLY_PROP_TYPE:
//		val->intval = POWER_SUPPLY_TYPE_USB_PD;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		/* USB uses this to set SDP current */
//		val->intval = get_client_vote(chg->usb_icl_votable,
//					      USB_PSY_VOTER);
		break;
	default:
		pr_err("get prop %d is not supported in usb\n", psp);
		ret = -EINVAL;
		break;
	}

	if (ret < 0) {
		pr_debug("Couldn't get prop %d rc = %d\n", psp, ret);
		return -ENODATA;
	}

	return 0;
}

static int wt_ac_set_prop(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
//	struct smb5 *chip = power_supply_get_drvdata(psy);
//	struct wt_chg *chg = &chip->chg;
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
//		rc = smblib_set_prop_sdp_current_max(chg, val->intval);
		break;
	default:
		pr_err("Set prop %d is not supported in usb psy\n",
				psp);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int wt_ac_prop_is_writeable(struct power_supply *psy,
		enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
	case POWER_SUPPLY_PROP_POWER_NOW:
	case POWER_SUPPLY_PROP_PD_ACTIVE:
	case POWER_SUPPLY_PROP_PD_VOLTAGE_MIN:
	case POWER_SUPPLY_PROP_PD_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_PD_CURRENT_MAX:
		return 1;
	default:
		break;
	}

	return 0;
}

static const struct power_supply_desc ac_psy_desc = {
	.name = "ac",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = wt_ac_props,
	.num_properties = ARRAY_SIZE(wt_ac_props),
	.get_property = wt_ac_get_prop,
	.set_property = wt_ac_set_prop,
	.property_is_writeable = wt_ac_prop_is_writeable,
};

static int wt_init_ac_psy(struct wt_chg *chg)
{
	struct power_supply_config ac_cfg = {};

	ac_cfg.drv_data = chg;
	ac_cfg.of_node = chg->dev->of_node;
	chg->ac_psy = devm_power_supply_register(chg->dev,
						  &ac_psy_desc,
						  &ac_cfg);
	if (IS_ERR(chg->ac_psy)) {
		pr_err("Couldn't register USB power supply\n");
		return PTR_ERR(chg->ac_psy);
	}

	return 0;
}


/************************
 * USB PSY REGISTRATION *
 ************************/
static enum power_supply_property wt_usb_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_TYPE,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_REAL_TYPE,
	POWER_SUPPLY_PROP_PD_ACTIVE,
	POWER_SUPPLY_PROP_PD_VOLTAGE_MIN,
	POWER_SUPPLY_PROP_PD_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_PD_CURRENT_MAX,
	POWER_SUPPLY_PROP_PD_USB_SUSPEND_SUPPORTED,
	POWER_SUPPLY_PROP_PD_IN_HARD_RESET,
//Bug 697289 ,lishuwen.wt,ADD,20211027, add typec cc orientation node
	POWER_SUPPLY_PROP_TYPEC_CC_ORIENTATION,
	POWER_SUPPLY_PROP_USB_OTG,
};

static int wt_usb_get_prop(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	int ret = 0;
	struct wt_chg *chg = power_supply_get_drvdata(psy);
	val->intval = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = wtchg_get_usb_online(chg);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = wtchg_get_usb_max_voltage(chg);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = wtchg_get_usb_max_voltage(chg);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		wtchg_get_vbus_voltage(chg);
		val->intval = chg->vbus_volt * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
//		rc = smblib_get_prop_usb_current_now(chg, val);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = wtchg_get_usb_max_current(chg);
		break;
	case POWER_SUPPLY_PROP_TYPE:
		val->intval =  wtchg_get_usb_real_type(chg);
		break;
	case POWER_SUPPLY_PROP_SCOPE:
//		rc = smblib_get_prop_scope(chg, val);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		/* USB uses this to set SDP current */
//		val->intval = get_client_vote(chg->usb_icl_votable,
//					      USB_PSY_VOTER);
		break;
	case POWER_SUPPLY_PROP_POWER_NOW:
		/* Show power rating for QC3+ adapter. */
	//	if (chg->real_charger_type == QTI_POWER_SUPPLY_TYPE_USB_HVDCP_3P5)
	//		val->intval = chg->qc3p5_detected_mw;
	//	else
	//		rc = -ENODATA;
		break;
	case POWER_SUPPLY_PROP_REAL_TYPE:
		val->intval = wtchg_get_usb_real_type(chg);
		break;
	case POWER_SUPPLY_PROP_PD_ACTIVE:
		val->intval = chg->pd_active;
		break;
	case POWER_SUPPLY_PROP_PD_VOLTAGE_MIN:
		val->intval = chg->pd_min_vol;
		break;
	case POWER_SUPPLY_PROP_PD_VOLTAGE_MAX:
		val->intval = chg->pd_max_vol;
		break;
	case POWER_SUPPLY_PROP_PD_CURRENT_MAX:
		val->intval = chg->pd_cur_max;
		break;
        case POWER_SUPPLY_PROP_PD_USB_SUSPEND_SUPPORTED:
		val->intval = chg->pd_usb_suspend;
		break;
	case POWER_SUPPLY_PROP_PD_IN_HARD_RESET:
		val->intval = chg->pd_in_hard_reset;
		break;
	case POWER_SUPPLY_PROP_USB_OTG:
		val->intval = chg->otg_enable;
		break;
//+Bug 697289 ,lishuwen.wt,ADD,20211027, add typec cc orientation node
	case POWER_SUPPLY_PROP_TYPEC_CC_ORIENTATION:
		val->intval = chg->typec_cc_orientation;
		break;
//-Bug 697289 ,lishuwen.wt,ADD,20211027, add typec cc orientation node
//+ ExtB 4980 ,tankaikun,add,20220219, add battery protect
	case POWER_SUPPLY_PROP_TYPEC_POWER_ROLE:
		val->intval = chg->typec_pr_role;
		break;
//- ExtB 4980 ,tankaikun,add,20220219, add battery protect
	default:
		pr_err("get prop %d is not supported in usb\n", psp);
		ret = -EINVAL;
		break;
	}

	if (ret < 0) {
		pr_debug("Couldn't get prop %d rc = %d\n", psp, ret);
		return -ENODATA;
	}

	return 0;
}

static int wt_usb_set_prop(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct wt_chg *chg = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		wtchg_set_usb_online(chg, val->intval);
		break;
	case POWER_SUPPLY_PROP_DP_DM:
		wtchg_set_charge_work(chg);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
//		rc = smblib_set_prop_sdp_current_max(chg, val->intval);
		break;
	case POWER_SUPPLY_PROP_POWER_NOW:
//			chg->qc3p5_detected_mw = val->intval;
		break;
	case POWER_SUPPLY_PROP_REAL_TYPE:
		wtchg_set_usb_real_type(chg, val->intval);
		break;
	case POWER_SUPPLY_PROP_PD_ACTIVE:
		chg->pd_active = val->intval;
		wtchg_pd_usb_switch(chg);
		break;
	case POWER_SUPPLY_PROP_PD_VOLTAGE_MIN:
		chg->pd_min_vol = val->intval;
		break;
	case POWER_SUPPLY_PROP_PD_VOLTAGE_MAX:
		chg->pd_max_vol = val->intval;
		break;
	case POWER_SUPPLY_PROP_PD_CURRENT_MAX:
		chg->pd_cur_max = val->intval;
		break;
        case POWER_SUPPLY_PROP_PD_USB_SUSPEND_SUPPORTED:
		chg->pd_usb_suspend =  val->intval;
		break;
        case POWER_SUPPLY_PROP_PD_IN_HARD_RESET:
		chg->pd_in_hard_reset =  val->intval;
		break;
	case POWER_SUPPLY_PROP_USB_OTG:
		chg->otg_enable = val->intval;
		wtchg_set_main_chg_otg(chg, val->intval);
		break;
//+Bug 697289 ,lishuwen.wt,ADD,20211027, add typec cc orientation node
	case POWER_SUPPLY_PROP_TYPEC_CC_ORIENTATION:
		chg->typec_cc_orientation = val->intval;
		if(!chg->typec_cc_orientation)
			chg->ato_soc_control = false;
		break;
//-Bug 697289 ,lishuwen.wt,ADD,20211027, add typec cc orientation node
//+ ExtB 4980,tankaikun,add,20220219, add battery protect
	case POWER_SUPPLY_PROP_TYPEC_POWER_ROLE:
		chg->typec_pr_role = val->intval;
		//cancel_delayed_work_sync(&chg->wt_chg_work);
		//schedule_delayed_work(&chg->wt_chg_work, 0);
		break;
//- ExtB 4980,tankaikun,add,20220219, add battery protect
	default:
		pr_err("Set prop %d is not supported in usb psy\n",
				psp);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int wt_usb_prop_is_writeable(struct power_supply *psy,
		enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
	case POWER_SUPPLY_PROP_POWER_NOW:
	case POWER_SUPPLY_PROP_REAL_TYPE:
	case POWER_SUPPLY_PROP_PD_ACTIVE:
	case POWER_SUPPLY_PROP_PD_VOLTAGE_MIN:
	case POWER_SUPPLY_PROP_PD_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_PD_CURRENT_MAX:
	case POWER_SUPPLY_PROP_DP_DM:
	case POWER_SUPPLY_PROP_ONLINE:
	case POWER_SUPPLY_PROP_USB_OTG:
		return 1;
	default:
		break;
	}

	return 0;
}

static const struct power_supply_desc usb_psy_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB, //ExtR OAK-1007, tankaikun@wt, modify 20220106, modify charger type(AC,USB) report error
	.properties = wt_usb_props,
	.num_properties = ARRAY_SIZE(wt_usb_props),
	.get_property = wt_usb_get_prop,
	.set_property = wt_usb_set_prop,
	.property_is_writeable = wt_usb_prop_is_writeable,
};

static int wt_init_usb_psy(struct wt_chg *chg)
{
	struct power_supply_config usb_cfg = {};

	usb_cfg.drv_data = chg;
	usb_cfg.of_node = chg->dev->of_node;
	chg->usb_psy = devm_power_supply_register(chg->dev,
						  &usb_psy_desc,
						  &usb_cfg);
	if (IS_ERR(chg->usb_psy)) {
		pr_err("Couldn't register USB power supply\n");
		return PTR_ERR(chg->usb_psy);
	}

	if (device_create_file(&chg->usb_psy->dev, &dev_attr_usb_temp_test)) {
		pr_err("Couldn't create file: usb_temp_test !\n");
	}

	if (device_create_file(&chg->usb_psy->dev, &dev_attr_afc_flag)) {
		pr_err("Couldn't create file: usb_temp_test !\n");
	}

	return 0;
}

static int wt_chg_probe(struct platform_device *pdev)
{
	struct wt_chg *wt_chg = NULL;
	int ret;

	pr_err("wt_chg probe start\n");
	if (!pdev->dev.of_node)
		return -ENODEV;

	pr_err("wt_chg probe start 1\n");
	if (pdev->dev.of_node) {
		wt_chg = devm_kzalloc(&pdev->dev, sizeof(struct wt_chg), GFP_KERNEL);
		if (!wt_chg) {
			pr_err("Failed to allocate memory\n");
			return -ENOMEM;
		}
	}
	if (!wt_chg) {
		pr_err("No platform data found\n");
		return -EINVAL;
	}

	wt_chg->dev = &pdev->dev;
	wt_chg->pdev = pdev;
	platform_set_drvdata(pdev, wt_chg);

	pr_err("wt_chg probe start 2\n");

	ret = wtchg_batt_parse_dt(wt_chg);
	if (ret < 0) {
		pr_err("Couldn't parse device tree rc=%d\n", ret);
		return ret;
	}

	//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	mutex_init(&wt_chg->resume_complete_lock);
	mutex_init(&wt_chg->battery_protect_lock);
	spin_lock_init(&wt_chg->ws_lock);
	wt_chg->main_chg_ws = wakeup_source_register(wt_chg->dev, "wt-chg");

	if (!wt_chg->main_chg_ws)
		pr_err("Couldn't get main_chg_ws\n");

	if (alarmtimer_get_rtcdev()) {
		alarm_init(&wt_chg->chg_main_alarm, ALARM_BOOTTIME,
					chg_main_alarm_cb);
	} else {
		pr_err("wt_chg_probe Couldn't get rtc device wt_charger probe fail \n");
		ret = -ENODEV;
		goto cleanup;
	}
	//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend

	wtchg_batt_init_config(wt_chg);
	ret = wt_init_batt_psy(wt_chg);
	if (ret < 0) {
		pr_err("Couldn't initialize batt psy rc=%d\n", ret);
		goto cleanup;
	}

	ret = wt_init_ac_psy(wt_chg);
	if (ret < 0) {
		pr_err("Couldn't initialize ac psy rc=%d\n", ret);
		goto cleanup;
	}

	ret = wt_init_usb_psy(wt_chg);
	if (ret < 0) {
		pr_err("Couldn't initialize usb psy rc=%d\n", ret);
		goto cleanup;
	}

	// ExtB oak-4542, tankaikun@wt, add 20220206, fix usb do not have diag port
	wtchg_set_main_chg_usb_switch(wt_chg, 1);

	INIT_DELAYED_WORK(&wt_chg->wt_chg_work, wt_chg_main);
	schedule_delayed_work(&wt_chg->wt_chg_work, 1000);
	pr_err("wt_chg probe succeed !\n");
	return 0;

cleanup:
//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	wakeup_source_unregister(wt_chg->main_chg_ws);
	mutex_destroy(&wt_chg->resume_complete_lock);
	mutex_destroy(&wt_chg->battery_protect_lock);
//- ExtB oak-2314, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend

	pr_err("wt_chg probe fail\n");
	return ret;
}

static int wt_chg_remove(struct platform_device *pdev)
{
	struct wt_chg *wt_chg = platform_get_drvdata(pdev);

//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	if(alarmtimer_get_rtcdev())
		alarm_cancel(&wt_chg->chg_main_alarm);

	wakeup_source_unregister(wt_chg->main_chg_ws);
	mutex_destroy(&wt_chg->resume_complete_lock);
	mutex_destroy(&wt_chg->battery_protect_lock);
//+ ExtB oak-2314, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend

	kfree(wt_chg);
	return 0;
}

//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
static int wt_chg_resume(struct device *dev)
{
	struct wt_chg *wt_chg = dev_get_drvdata(dev);
	unsigned long flags;
	ktime_t now;

	now = ktime_get_boottime();

	if(ktime_compare(now, wt_chg->resume_main_work_time)){
		mutex_lock(&wt_chg->resume_complete_lock);
		wt_chg->resume_completed = true;
		mutex_unlock(&wt_chg->resume_complete_lock);

		spin_lock_irqsave(&wt_chg->ws_lock, flags);
		if(!atomic_read(&wt_chg->pm_awake_active)){
			if (wt_chg->main_chg_ws)
				__pm_stay_awake(wt_chg->main_chg_ws);
			atomic_set(&wt_chg->pm_awake_active, 1);
		}
		spin_unlock_irqrestore(&wt_chg->ws_lock, flags);

		dev_err(wt_chg->dev, "wt_chg_resume enter now = %lld \n", ktime_to_ms(now));

		cancel_delayed_work_sync(&wt_chg->wt_chg_work);
		schedule_delayed_work(&wt_chg->wt_chg_work, 0);
	}

	return 0;
}

static int wt_chg_suspend(struct device *dev)
{
	struct wt_chg *wt_chg = dev_get_drvdata(dev);

	dev_err(wt_chg->dev,"wt_chg_suspend \n");

	mutex_lock(&wt_chg->resume_complete_lock);
	wt_chg->resume_completed = false;
	mutex_unlock(&wt_chg->resume_complete_lock);

	return 0;
}

static const struct dev_pm_ops wt_chg_pm_ops = {
	.resume		= wt_chg_resume,
	.suspend	= wt_chg_suspend,
};
//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend

static const struct of_device_id wt_chg_dt_match[] = {
	{.compatible = "qcom,wt_chg"},
	{},
};

static struct platform_driver wt_chg_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "wt_chg",
		.of_match_table = wt_chg_dt_match,
		.pm		= &wt_chg_pm_ops,
	},
	.probe = wt_chg_probe,
	.remove = wt_chg_remove,
};

static int __init wt_chg_init(void)
{
    platform_driver_register(&wt_chg_driver);
	pr_err("wt_chg init end\n");
    return 0;
}

static void __exit wt_chg_exit(void)
{
	pr_err("wt_chg exit\n");
	platform_driver_unregister(&wt_chg_driver);
}

module_init(wt_chg_init);
module_exit(wt_chg_exit);

MODULE_AUTHOR("WingTech Inc.");
MODULE_DESCRIPTION("battery driver");
MODULE_LICENSE("GPL");
