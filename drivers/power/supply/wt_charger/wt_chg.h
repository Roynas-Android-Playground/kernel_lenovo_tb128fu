#include <linux/alarmtimer.h>
#include <linux/ktime.h>
#include <linux/types.h>
#include <linux/timer.h>
#include <linux/atomic.h>


/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_WT_CHARGER_H__
#define __LINUX_WT_CHARGER_H__

#define	DISABLE_BOARD_TEMP_CONTROL

#define ATO_BATT_SOC_MAX	79
#define ATO_BATT_SOC_MIN	60

#define BATT_PROTECT_SOC_MAX	60
#define BATT_PROTECT_SOC_MIN	40

#define BATT_MAINTAIN_SOC_MAX	80
#define BATT_MAINTAIN_SOC_MIN	60

#define BATT_SOC_ZERO	0
#define SOC_ZERO_MAX	3
#define BATT_CAPACITY_DEFAULT_VALUE 70
#define CURRENT_ZERO_MAX	3

#define VBUS_VOLT_MIN 2600
#define VBUS_VOLT_ZERO_MAX 3
#define BATT_TEMP_DEFAULT	250
#define BATT_TEMP_MIN	-200
#define BATT_TEMP_MAX	800
#define BATT_TEMP_0	0
#define TEMP_ZERO_MAX	3

#define BATT_VOLT_MIN 3100000
#define BATT_VOLT_MAX 4500000
#define VOLT_ZERO_MAX 3

#define SLAVE_CHG_START_SOC_MAX 90

#define CHARGE_ONLINE_INTERVAL_MS	10000
#define NORMAL_INTERVAL_MS	30000

//Jeita parameter
#define BATT_TEMP_MIN_THRESH	-200
#define BATT_TEMP_0_THRESH	10
#define BATT_TEMP_15_THRESH	160
#define BATT_TEMP_29_THRESH	290
#define BATT_TEMP_33_THRESH	330
#define BATT_TEMP_35_THRESH	350
#define BATT_TEMP_45_THRESH	440
#define BATT_TEMP_50_THRESH	500
//#define BATT_TEMP_55_THRESH	540
#define BATT_TEMP_MAX_THRESH	600
#define BATT_TEMP_HYSTERESIS	20

#define BOARD_TEMP_41_THRESH	410
#define BOARD_TEMP_43_THRESH	430
#define BOARD_TEMP_45_THRESH	450
#define BOARD_TEMP_47_THRESH	470
#define BOARD_TEMP_58_THRESH	580



#define FCC_4000_MA	4000
#define FCC_3600_MA	3600
#define FCC_3000_MA	3000
#define FCC_2600_MA	2600
#define FCC_2200_MA	2200
#define FCC_2000_MA	2000

#define FCC_1800_MA	1800
#define FCC_1600_MA	1600
#define FCC_1500_MA	1500

#define FCC_1200_MA	1200
#define FCC_1100_MA	1100
#define FCC_1000_MA	1000
#define FCC_900_MA	900
#define FCC_800_MA	800
#define FCC_700_MA	700
#define FCC_650_MA	650
#define FCC_600_MA	600
#define FCC_500_MA	500
#define FCC_300_MA	300
#define FCC_0_MA	0

#define SHUTDOWN_BATT_VOLT		3350000

#define BATT_NORMAL_CV		4432
#define BATT_HIGH_TEMP_CV	4200

#define DEFAULT_HVDCP_VOLT 5000

//+ ExtB oak-2480, tankaikun@wt, add 20220121, fix power supply system report error maxChargingMicAmp cause mErrorShowDialog
#define DEFAULT_HVDCP_USB_VOLT_DESIGN	9000
#define DEFAULT_HVDCP3_USB_VOLT_DESIGN	10000
#define DEFAULT_PD_USB_VOLT_DESIGN	10000
#define DEFAULT_USB_VOLT_DESIGN	5000
//- ExtB oak-2480, tankaikun@wt, add 20220121, fix power supply system report error maxChargingMicAmp cause mErrorShowDialog

// ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
#define WT_CHARGER_ALARM_TIME		30000 // 30s

#define DEFAULT_BATT_TEMP	250
#define CHG_CURRENT_BYPASS_VALUE 2000

#define HVDCP20_VOLT_6V 6000

enum batt_jeita_status {
	BATT_TEMP_COLD = 0,
	BATT_TEMP_COOL,
	BATT_TEMP_NORMAL,
	BATT_TEMP_WARM,
	BATT_TEMP_HOT,
};

enum wt_hvdcp_mode {
	WT_HVDCP20_5V = 1, 
	WT_HVDCP20_9V, 
	WT_HVDCP20_12V, 
	WT_HVDCP30_5V, 
};

/*hvdcp30*/
struct hvdcp30_profile_t {
	unsigned int vbat;
	unsigned int vchr;
};

struct wt_chg {
	struct platform_device *pdev;
	struct device *dev;
	struct charger_device *master_dev;
	struct charger_device *slave_dev;
	struct charger_device *bbc_dev;

	struct iio_channel *main_chg_therm;
	struct iio_channel *usb_port_therm;
	struct iio_channel *board_pcb_therm;

	struct power_supply *main_psy;
	struct power_supply *slave_psy;
	struct power_supply *batt_psy;
	struct power_supply *ac_psy;
	struct power_supply *usb_psy;
	struct power_supply *bms_psy;
	struct power_supply *hvdcp_psy;

	struct timer_list hvdcp_timer;
	struct delayed_work wt_chg_work;

//+ ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	struct alarm		chg_main_alarm;
	struct wakeup_source	*main_chg_ws;
	struct mutex		resume_complete_lock;
	struct mutex		battery_protect_lock;

	spinlock_t ws_lock;

	atomic_t pm_awake_active;

	ktime_t resume_main_work_time;
//- ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend

	int pd_active;
	int real_type;
	int pd_min_vol;
	int pd_max_vol;
	int pd_cur_max;
	int pd_usb_suspend;
	int pd_in_hard_reset;

	int battery_exist;
	int vbus_online;
	int vbus_volt;
	int chg_status;
	int usb_online;
	int ac_online;
	int otg_enable;
	int batt_volt;
	int chg_current;
	int batt_capacity;
	int batt_capacity_level;
	int batt_temp;
	int batt_temp_true;
	int batt_health;
	int board_temp;
	int main_chg_temp;
	int usb_port_temp;
	int lcd_on;
	const char *batt_id_string;

	int batt_cv_max;
	int fast_charger;
	int jeita_batt_cv;
	int jeita_batt_cv_pre;

	int batt_fcc_max;
	int jeita_ibatt;
	int chg_ibatt;
	int chg_ibatt_pre;

	int main_chg_ibatt;
	int slave_chg_ibatt;
	int slave_chg_enable;

	int chg_type_ibatt;
	int chg_type_input_curr;
	int chg_type_input_curr_pre;
	int chg_type_input_curr_max;
	int chg_type_input_voltage_max;

	int main_chg_input_curr;
	int slave_chg_input_curr;

	int batt_jeita_stat_prev;
	int chg_iterm;
	int batt_iterm;
	int thermal_levels;
	int system_temp_level;

	int start_charging_test;
	int ato_soc_control;
	int interval_time;
	int soc_zero_count;
	int temp_zero_count;
	int volt_zero_count;

	bool chg_init_flag;
	int batt_maintain_mode;
	int maintain_soc_control;
	int batt_protected_mode;
	int protected_soc_control;

	int current_zero_count;
	int vbus_volt_zero_count;

	int pre_vbus;
	int ship_mode;

	bool	usb_temp_debug_flag;
	int	usb_debug_temp;
	bool	batt_temp_debug_flag;
	int	batt_debug_temp;
	bool	board_temp_debug_flag;
	int	board_debug_temp;

// ExtB oak-3798, tankaikun@wt, add 20220126, fix plug in charger slowly cause identify float type
	int float_recheck_cnt;

// ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	int resume_completed;

//+ ExtB OAK-3916, tankaikun@wt, add 20220209, add battery protect mode
	int batt_protected_mode_disable_charge;
	int batt_protected_mode_disable_charge_pre;
	int batt_protected_mode_disable_suspend;
	int batt_protected_mode_disable_suspend_pre;
	
	int typec_pr_role;
//- ExtB OAK-3916, tankaikun@wt, add 20220209, add battery protect mode

//Bug 697289 ,lishuwen.wt,ADD,20211027, add typec cc orientation node
	int typec_cc_orientation;
//chk SC19978, lishuwen.wt, add, 20211028, add start/stop charging node
};

#endif
