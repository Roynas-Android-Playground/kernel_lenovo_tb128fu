
#ifndef __BQ2589X_CHARGER_HEADER__
#define __BQ2589X_CHARGER_HEADER__

#define BATT_LOW_VOLT		5

#define DEFAULT_BATT_CV	4432
#define DEFAULT_PRECHG_CURR	320
#define DEFAULT_TERM_CURR	256
#define DEFAULT_CHG_CURR	500

#define CHG_FULL_ERROR_MAX	1

enum bq2589x_vbus_type {
	BQ2589X_VBUS_NONE,
	BQ2589X_VBUS_USB_SDP,
	BQ2589X_VBUS_USB_CDP, /*CDP for bq25890, Adapter for bq25892*/
	BQ2589X_VBUS_USB_DCP,
	BQ2589X_VBUS_MAXC,
	BQ2589X_VBUS_UNKNOWN,
	BQ2589X_VBUS_NONSTAND,
	BQ2589X_VBUS_OTG,
	BQ2589X_VBUS_TYPE_NUM,
};

enum bq2589x_part_no {
	SYV690 = 0x01,
	BQ25890 = 0x03,
	BQ25892 = 0x00,
	BQ25895 = 0x07,
};

enum bq2589x_chrs_status {
	BQ2589X_NOT_CHARGING = 0,
	BQ2589X_PRE_CHARGE,
	BQ2589X_FAST_CHARGING,
	BQ2589X_CHARGE_DONE,
};

#define BQ2589X_STATUS_PLUGIN		0x0001
#define BQ2589X_STATUS_PG			0x0002
#define	BQ2589X_STATUS_CHARGE_ENABLE 0x0004
#define BQ2589X_STATUS_FAULT		0x0008

#define BQ2589X_STATUS_EXIST		0x0100

struct bq2589x_config {
	bool	enable_auto_dpdm;
/*	bool	enable_12v;*/

	int charge_voltage;
	int charge_current;

	bool enable_term;
	int term_current;

	int prechg_current;
	bool 	enable_ico;
	bool	use_absolute_vindpm;
};


struct bq2589x {
	struct device *dev;
	struct i2c_client *client;
	enum   bq2589x_part_no part_no;
	int    revision;

	unsigned int    status;
	int		vbus_type;
	int		usb_online;
	int		chg_status;
	int		charge_type;

	bool	enabled;
	bool	is_bq25890h;

	int		vbus_volt;
	int		vbat_volt;

	int		rsoc;
	struct	bq2589x_config	cfg;
	struct delayed_work irq_work;
	struct work_struct adapter_in_work;
	struct work_struct adapter_out_work;
	struct delayed_work monitor_work;
	struct delayed_work ico_work;

	struct power_supply_desc usb;
	struct power_supply_desc wall;
	struct power_supply *batt_psy;
	struct power_supply *usb_psy;
	struct power_supply *wall_psy;
	struct power_supply *wt_hvdcp_psy;

// ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend 
	struct mutex		resume_complete_lock;

	int otg_gpio;
	int irq_gpio;
	int usb_switch1;
	int usb_switch_flag;
	int otg_status;

	bool firmware_flag;
	bool charging_enable;
	bool usb_suspend;

	int chg_error_count;

// ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend
	int resume_completed;
};

extern int main_set_hiz_mode(bool en);
extern int main_set_charge_enable(bool en);
extern int main_set_input_current_limit(int curr);
extern int main_set_charge_current(int curr);
extern int main_get_charge_type(void);
extern int main_enable_otg(bool en);
extern int main_get_charge_status(void);
#endif
