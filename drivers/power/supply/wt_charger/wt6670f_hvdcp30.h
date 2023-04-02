/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_WT6670F_CHARGER_H__
#define __LINUX_WT6670F_CHARGER_H__

/* Register B0h */
#define WT6670F_RERUN_ADSP_CMD		0xB0

/* Register B1h */
#define WT6670F_QC_MODE_SET_CMD		0xB1
#define WT6670F_QC_MODE_SET_MASK		0x03
#define WT6670F_QC_MODE_SET_SHIFT		0

/* Register B3h */
#define WT6670F_SOFT_RESET_CMD		0xB3

/* Register B4h */
#define WT6670F_ENTER_OTG_MODE_CMD	0xB4

/* Register B5h */
#define WT6670F_SLEEP_MODE_CMD		0xB5

/* Register B6h */
#define WT6670F_BC12_DETECT_CMD		0xB6
#define WT6670F_BC12_ENABLE_MASK		0x01
#define WT6670F_BC12_ENABLE_SHIFT		0
#define WT6670F_BC12_ENABLE			0x01

/* Register BAh */
#define WT6670F_QC30_PULSE_MODE_CMD	0xBA
#define WT6670F_QC30_PULSE_MASK		0x7FFF
#define WT6670F_QC30_PULSE_SHIFT		0

#define WT6670F_QC30_RISE_ENABLE		0x01
#define WT6670F_QC30_DOWN_ENABLE		0
#define WT6670F_QC30_RISE_ENABLE_MASK		0xfffe
#define WT6670F_QC30_RISE_ENABLE_SHIFT		15

#define WT6670F_QC30_PULSE_STEP 200

/* Register BBh */
#define WT6670F_QC35_PULSE_MODE_CMD	0xBB
#define WT6670F_QC35_PULSE_MASK		0x7FFF
#define WT6670F_QC35_PULSE_SHIFT		0

#define WT6670F_QC35_RISE_ENABLE		0x01
#define WT6670F_QC35_DOWN_ENABLE		0
#define WT6670F_QC35_RISE_ENABLE_MASK		0xfffe
#define WT6670F_QC35_RISE_ENABLE_SHIFT		15

#define WT6670F_QC35_PULSE_STEP 20

/* Register BDh */
#define WT6670F_CHARGE_TYPE_CMD		0xBD
#define WT6670F_CHARGE_TYPE_MASK		0xFF

/* Register BEh */
#define WT6670F_VBUS_VOLTAGE_CMD		0xBE
#define WT6670F_ADC_BIT	1024
#define WT6670F_VOLT_MV 1000
#define WT6670F_BYTE_BIT		8

#define WT6670F_VBUS_EXIST_VOLT 0

#define WT6670F_VBUS_SET_MIN	5000
#define WT6670F_VBUS_SET_MAX 10000

/* Register BFh */
#define WT6670F_FIRMWARE_VERSION_CMD	0xBF

#define ENABLE_OTG_MODE 0x01
#define ENABLE_SLEEP_MODE 0x01
#define DISABLE_SLEEP_MODE 0x00

/* Vbus Resistance parameters */
#define VBUS_PULLUP_R1	220
#define VBUS_PULLUP_R2	220
#define VBUS_PULLDOWN_R	68

enum wt6670f_data_byte_num {
	WT6670F_ONE_BYTE = 1, 
	WT6670F_TWO_BYTE,
};

enum wt6670f_request_volt_mode {
	WT6670F_VOLT_INCREASE = 1, 
	WT6670F_VOLT_DECREASE,
};

enum wt6670f_chip {
	WT6670F = 1, /* wt6670f  */
};

enum wt6670f_hvdcp_mode {
	WT6670F_HVDCP20_5V = 1, 
	WT6670F_HVDCP20_9V, 
	WT6670F_HVDCP20_12V, 
	WT6670F_HVDCP30_5V, 
};

enum wt6670f_charge_type {
	WT6670F_CHARGE_TYPE_FC = 1, 
	WT6670F_CHARGE_TYPE_SDP,
	WT6670F_CHARGE_TYPE_CDP,
	WT6670F_CHARGE_TYPE_DCP,
	WT6670F_CHARGE_TYPE_QC20,
	WT6670F_CHARGE_TYPE_QC30,
	WT6670F_CHARGE_TYPE_OCP,
	WT6670F_CHARGE_TYPE_QC35_18W,
	WT6670F_CHARGE_TYPE_QC35_27W,
	WT6670F_CHARGE_TYPE_UNKNOWN = 0x11,
};

static DEFINE_MUTEX(wt6670f_i2c_lock);

struct wt6670f_info {
	struct device *dev;
	struct i2c_client *client;
	int id;
	enum wt6670f_chip chip;
	u32 opts;
	const char *name;
	struct mutex lock;

	int reset_gpio;
	int vbus_type;
	int vbus_volt;
	int current_volt;
	int volt_request_mode;
	int vbus_volt_max;
	int firmware_version;
	int float_charge_recheck_cnt;

	struct work_struct irq_handle_work;
	struct delayed_work monitor_work;
	struct power_supply *usb_psy;
	struct power_supply *main_chg_psy;
	struct power_supply *wt_hvdcp_psy;
};

#endif
