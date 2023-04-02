
#ifndef __SGM4151X_HEADER__
#define __SGM4151X_HEADER__

/* Register 00h */
#define SGM4151X_REG_00      		0x00
#define SGM4151X_ENHIZ_MASK		    0x80
#define SGM4151X_ENHIZ_SHIFT		    7
#define SGM4151X_HIZ_ENABLE          1
#define SGM4151X_HIZ_DISABLE         0

#define SGM4151X_IINLIM_MASK		    0x1F
#define SGM4151X_IINLIM_SHIFT		0
#define SGM4151X_IINLIM_BASE         100
#define SGM4151X_IINLIM_LSB          100

/* Register 01h */
#define SGM4151X_REG_01		    	0x01
#define SGM4151X_OTG_CONFIG_MASK     0x20
#define SGM4151X_OTG_CONFIG_SHIFT    5
#define SGM4151X_OTG_ENABLE          1
#define SGM4151X_OTG_DISABLE         0

#define SGM4151X_CHG_CONFIG_MASK     0x10
#define SGM4151X_CHG_CONFIG_SHIFT    4
#define SGM4151X_CHG_ENABLE          1
#define SGM4151X_CHG_DISABLE         0

#define SGM4151X_SYS_MINV_MASK       0x0E
#define SGM4151X_SYS_MINV_SHIFT      1

/* Register 0x02 */
#define SGM4151X_REG_02              0x02
#define SGM4151X_BOOST_LIM_MASK      0x80
#define SGM4151X_BOOST_LIM_SHIFT     7
#define SGM4151X_BOOST_LIM_500MA     0
#define SGM4151X_BOOST_LIM_1200MA     1

#define SGM4151X_ICHG_MASK           0x3F
#define SGM4151X_ICHG_SHIFT          0
#define SGM4151X_ICHG_BASE           0
#define SGM4151X_ICHG_LSB            60

/* Register 0x03*/
#define SGM4151X_REG_03              0x03
#define SGM4151X_IPRECHG_MASK        0xF0
#define SGM4151X_IPRECHG_SHIFT       4
#define SGM4151X_IPRECHG_BASE        60
#define SGM4151X_IPRECHG_LSB         60

#define SGM4151X_ITERM_MASK          0x0F
#define SGM4151X_ITERM_SHIFT         0
#define SGM4151X_ITERM_BASE          60
#define SGM4151X_ITERM_LSB           60

/* Register 0x04*/
#define SGM4151X_REG_04              0x04
#define SGM4151X_VREG_MASK           0xF8
#define SGM4151X_VREG_SHIFT          3
#define SGM4151X_VREG_BASE           3856
#define SGM4151X_VREG_LSB            32

/* Register 0x05*/
#define SGM4151X_REG_05              0x05
#define SGM4151X_EN_TERM_MASK        0x80
#define SGM4151X_EN_TERM_SHIFT       7
#define SGM4151X_TERM_ENABLE         1
#define SGM4151X_TERM_DISABLE        0

#define SGM4151X_WDT_MASK            0x30
#define SGM4151X_WDT_SHIFT           4
#define SGM4151X_WDT_DISABLE         0
#define SGM4151X_WDT_40S             1
#define SGM4151X_WDT_80S             2
#define SGM4151X_WDT_160S            3
#define SGM4151X_WDT_BASE            0
#define SGM4151X_WDT_LSB             40

#define SGM4151X_EN_TIMER_MASK       0x08
#define SGM4151X_EN_TIMER_SHIFT      3

#define SGM4151X_CHG_TIMER_ENABLE    1
#define SGM4151X_CHG_TIMER_DISABLE   0

#define SGM4151X_CHG_TIMER_MASK      0x04
#define SGM4151X_CHG_TIMER_SHIFT     2
#define SGM4151X_CHG_TIMER_4HOURS    0
#define SGM4151X_CHG_TIMER_6HOURS    1

#define SGM4151X_JEITA_ISET_MASK     0x01
#define SGM4151X_JEITA_ISET_SHIFT    0
#define SGM4151X_JEITA_ISET_50PCT    0
#define SGM4151X_JEITA_ISET_20PCT    1

/* Register 0x06*/
#define SGM4151X_REG_06              0x06
#define SGM4151X_OVP_MASK		0xC0
#define SGM4151X_OVP_SHIFT        6

#define SGM4151X_OVP_5V        0x00
#define SGM4151X_OVP_6V5        0x01
#define SGM4151X_OVP_9V        0x02
#define SGM4151X_OVP_12V        0x03

#define SGM4151X_BOOSTV_MASK         0x30
#define SGM4151X_BOOSTV_SHIFT        4
#define SGM4151X_BOOSTV_BASE         4550
#define SGM4151X_BOOSTV_LSB          64

#define SGM4151X_VINDPM_MASK         0x0F
#define SGM4151X_VINDPM_SHIFT        0
#define SGM4151X_VINDPM_BASE         3900
#define SGM4151X_VINDPM_LSB          100

/* Register 0x07*/
#define SGM4151X_REG_07			0x07
#define SGM4151X_IINDET_EN		7
#define SGM4151X_TMR2X_EN		6
#define SGM4151X_BATFET_DIS		5
#define SGM4151X_JEITA_VSET		4
#define SGM4151X_BATFET_DLY		3
#define SGM4151X_BATFET_RST_EN		2
#define SGM4151X_VDPM_BAT_TRACK_MASK	0x03

#define SGM4151X_BATFET_DIS_MASK     0x20
#define SGM4151X_BATFET_DIS_SHIFT    5
#define SGM4151X_BATFET_OFF          1

/* Register 0x08*/
#define SGM4151X_REG_08              0x08
#define SGM4151X_VBUS_STAT_MASK      0xE0
#define SGM4151X_VBUS_STAT_SHIFT     5
#define SGM4151X_CHRG_STAT_MASK      0x18
#define SGM4151X_CHRG_STAT_SHIFT     3
#define SGM4151X_CHRG_STAT_IDLE      0
#define SGM4151X_CHRG_STAT_PRECHG    1
#define SGM4151X_CHRG_STAT_FASTCHG   2
#define SGM4151X_CHRG_STAT_CHGDONE   3

#define SGM4151X_PG_STAT_MASK        0x04
#define SGM4151X_PG_STAT_SHIFT       2
#define SGM4151X_THERM_STAT_MASK       0x02
#define SGM4151X_THERM_STAT_SHIFT      1
#define SGM4151X_VSYS_STAT_MASK      0x01
#define SGM4151X_VSYS_STAT_SHIFT     0

/* Register 0x09*/
#define SGM4151X_REG_09              0x09
#define SGM4151X_FAULT_WDT_MASK      0x80
#define SGM4151X_FAULT_WDT_SHIFT     7
#define SGM4151X_FAULT_BOOST_MASK    0x40
#define SGM4151X_FAULT_BOOST_SHIFT   6
#define SGM4151X_FAULT_CHRG_MASK     0x30
#define SGM4151X_FAULT_CHRG_SHIFT    4
#define SGM4151X_FAULT_CHRG_NORMAL   0
#define SGM4151X_FAULT_CHRG_INPUT    1
#define SGM4151X_FAULT_CHRG_THERMAL  2
#define SGM4151X_FAULT_CHRG_TIMER    3

#define SGM4151X_FAULT_BAT_MASK      0x08
#define SGM4151X_FAULT_BAT_SHIFT     3
#define SGM4151X_FAULT_NTC_MASK      0x07
#define SGM4151X_FAULT_NTC_SHIFT     0

#define SGM4151X_FAULT_NTC_GOOD    1
#define SGM4151X_FAULT_NTC_WARM      2
#define SGM4151X_FAULT_NTC_COOL      3
#define SGM4151X_FAULT_NTC_COLD      5
#define SGM4151X_FAULT_NTC_HOT       6

#define SGM4151X_REG_0A			0x0A

/* Register 0x0B*/
#define SGM4151X_REG_0B              0x0B
#define SGM4151X_RESET_MASK          0x80
#define SGM4151X_RESET_SHIFT         7
#define SGM4151X_RESET               1

#define SGM4151X_PN_MASK             0x78
#define SGM4151X_PN_SHIFT            3

#define SGM4151X_DEV_REV_MASK        0x03
#define SGM4151X_DEV_REV_SHIFT       0

#define SGM4151x_OVP_5000mV 5000
#define SGM4151x_OVP_6000mV 6000
#define SGM4151x_OVP_8000mV 8000
#define SGM4151x_OVP_10000mV 10000

#define DEFAULT_BATT_CV	4432
#define DEFAULT_PRECHG_CURR	240
#define DEFAULT_TERM_CURR	240
#define DEFAULT_CHG_CURR	500

enum sgm4151x_part_no {
	SGM41511 = 0x02,
	SY6974 = 0x09,
};

enum sgm4151x_chrs_status {
	SGM4151X_NOT_CHARGING = 0,
	SGM4151X_PRE_CHARGE,
	SGM4151X_FAST_CHARGING,
	SGM4151X_CHARGE_DONE,
};

enum sgm4151x_ovp {
	SGM4151x_OVP_5500mV,
	SGM4151x_OVP_6500mV,
	SGM4151x_OVP_10500mV,
	SGM4151x_OVP_14000mV,		
};


struct sgm4151x_config {
	bool	enable_auto_dpdm;
/*	bool	enable_12v;*/

	int	charge_voltage;
	int	charge_current;

	bool	enable_term;
	int	term_current;

	int prechg_current;

	bool 	enable_ico;
	bool	use_absolute_vindpm;
};

struct sgm4151x_device {
	struct device *dev;
	struct i2c_client *client;

	struct sgm4151x_config  cfg;
	struct delayed_work irq_work;
	struct delayed_work monitor_work;
	struct power_supply *wall_psy;

// ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend 
	struct mutex		resume_complete_lock;

	enum sgm4151x_part_no part_no;
	int    revision;
	bool	is_sgm41511;

	unsigned int    status;
	int vbus_type;
	int usb_online;
	int chg_status;
	int charge_type;

	bool	enabled;

	int chg_en_gpio;
	int irq_gpio;

// ExtB oak-2318, tankaikun@wt, add 20220125, fix iic read/write return -13 when system suspend 
	int resume_completed;
};
#endif
