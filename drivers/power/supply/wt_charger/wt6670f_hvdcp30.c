/*
 * WT6670F QC3.0 protocol I2C driver
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

#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <asm/unaligned.h>
#include <linux/delay.h>
#include <linux/power_supply.h>
#include "wt6670f_hvdcp30.h"

#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include "wt6670f_fireware.h"

static DEFINE_IDR(wt6670f_id);
static DEFINE_MUTEX(wtchg_mutex);

static int wt6670f_i2c_read(struct wt6670f_info *wt_chip, u8 reg,
				    bool single)
{
	struct i2c_client *client = to_i2c_client(wt_chip->dev);
	struct i2c_msg msg[2];
	u8 data[2];
	int ret;

	if (!client->adapter)
		return -ENODEV;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = &reg;
	msg[0].len = sizeof(reg);
	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = data;
	if (single)
		msg[1].len = 1;
	else
		msg[1].len = 2;

	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (ret < 0)
		return ret;

	if (!single)
		ret = get_unaligned_le16(data);
	else
		ret = data[0];

	return ret;
}

static int wt6670f_i2c_write(struct wt6670f_info *wt_chip, u8 reg,
				     int value, bool single)
{
	struct i2c_client *client = to_i2c_client(wt_chip->dev);
	struct i2c_msg msg;
	u8 data[4];
	int ret;

	if (!client->adapter)
		return -ENODEV;

	data[0] = reg;
	if (single == 1) {
		data[1] = (u8) value;
		msg.len = 2;
	} else {
		put_unaligned_le16(value, &data[1]);
		msg.len = 3;
	}

	msg.buf = data;
	msg.addr = client->addr;
	msg.flags = 0;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EINVAL;

	return 0;
}

static int wt6670f_i2c_bulk_read(struct wt6670f_info *wt_chip, u8 reg,
					 u8 *data, int len)
{
	struct i2c_client *client = to_i2c_client(wt_chip->dev);
	int ret;

	if (!client->adapter)
		return -ENODEV;

	ret = i2c_smbus_read_i2c_block_data(client, reg, len, data);
	if (ret < 0)
		return ret;
	if (ret != len)
		return -EINVAL;

	return 0;
}

static int wt6670f_i2c_bulk_write(struct wt6670f_info *wt_chip,
					  u8 reg, u8 *data, int len)
{
	struct i2c_client *client = to_i2c_client(wt_chip->dev);
	struct i2c_msg msg;
	u8 buf[33];
	int ret;

	if (!client->adapter)
		return -ENODEV;

	buf[0] = reg;
	memcpy(&buf[1], data, len);
	msg.buf = buf;
	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = len + 1;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EINVAL;

	return 0;
}

static int set_address_high_byte_commond(struct wt6670f_info *wt_chip,
					  u8 high_addr)
{
	struct i2c_client *client = to_i2c_client(wt_chip->dev);
	struct i2c_msg msg;
	u8 buf[3];
	int ret;

	if (!client->adapter)
		return -ENODEV;

	buf[0] = 0x10;
	buf[1] = 0x01;
	buf[2] = high_addr;
	msg.buf = buf;
	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = 3;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EINVAL;

	return 0;
}


int wt6670f_write_block_data(struct wt6670f_info *wt_chip, u8 *writebuf, u32 writelen)
{
    int ret = 0;
    int i = 0;
    struct i2c_msg msg;
    struct i2c_client *client = to_i2c_client(wt_chip->dev);

    memset(&msg, 0, sizeof(struct i2c_msg));
    msg.addr = client->addr;
    msg.flags = 0;
    msg.len = writelen;
    msg.buf = writebuf;

    for (i = 0; i < 3; i++) {
        ret = i2c_transfer(client->adapter, &msg, 1);
        if (ret < 0) {
            pr_err("lsw_wt6670f_fw i2c_transfer(write) fail,ret:%d", ret);
        } else {
            break;
        }
    }
    return ret;
}

int wt6670f_read_block_data(struct wt6670f_info *wt_chip, u8 lowe_addr, u8 *readbuf, u32 readlen)
{
    int ret = 0;
    int i = 0;
    struct i2c_msg msg[2];
    struct i2c_client *client = to_i2c_client(wt_chip->dev);
    u8 read_cmd[2] = {0};

    read_cmd[0] =0x61;
    read_cmd[1] =lowe_addr;

	if (!client->adapter)
		return -ENODEV;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = read_cmd;
	msg[0].len = 2;
	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = readbuf;
	msg[1].len = readlen;

    for (i = 0; i < 3; i++) {
        ret = i2c_transfer(client->adapter, msg, 2);
        if (ret < 0) {
            pr_err("lsw_wt6670f_fw i2c_transfer(read) fail,ret:%d", ret);
        } else {
            break;
        }
    }
    return ret;
}

static int wt6670f_i2c_write_block_data(struct wt6670f_info *wt_chip, u8 lowe_addr, u8 *buf, u32 len)
{
    int ret = 0;
    u32 i = 0;
    u32 j = 0;
    u32 packet_number = 0;
    u32 packet_len = 0;
    u32 addr = 0;
    u32 offset = 0;
    u32 remainder = 0;
    u32 cmdlen = 0;
    u8 packet_buf[WT6670F_FLASH_PACKET_LENGTH + WT6670F_CMD_WRITE_LEN] = { 0 };

    packet_number = len / WT6670F_FLASH_PACKET_LENGTH;
    remainder = len % WT6670F_FLASH_PACKET_LENGTH;
    if (remainder > 0)
        packet_number++;
    packet_len = WT6670F_FLASH_PACKET_LENGTH;

    for (i = 0; i < packet_number; i++) {
        offset = i * WT6670F_FLASH_PACKET_LENGTH;
        addr = lowe_addr + offset;

        /* last packet */
        if ((i == (packet_number - 1)) && remainder)
            packet_len = remainder;

		packet_buf[0] = 0x41;
		packet_buf[1] = lowe_addr;
		cmdlen = 2;

        for (j = 0; j < packet_len; j++) {
            packet_buf[cmdlen + j] = buf[offset + j];
        }

        ret = wt6670f_write_block_data(wt_chip, packet_buf, packet_len + cmdlen);
        if (ret < 0) {
            pr_err("lsw_wt6670f_fw app write fail");
            return ret;
        }
        //msleep(1);
        usleep_range(10, 12);
    }
    return ret;
}

static int update_firmware(struct wt6670f_info *wt_chip)
{
	struct i2c_client *client = to_i2c_client(wt_chip->dev);
	int ret;
	struct i2c_msg enter_isp_mode_msg[2] = {
		{
			.addr = 0x2B,
			.flags = 0,
			.len = 1,
			.buf = "\x00",
		}, {
			.addr = 0x48,
			.flags = 0,
			.len = 1,
			.buf = "\x00",
		}
	};
	u8 id_cmd[2] = {0x80,0x00};
	u8 id_data[2];
	struct i2c_msg read_chip_id_msg[2] = {
		{
			.addr = 0x34,
			.flags = 0,
			.len = 2,
			.buf = id_cmd,
		}, {
			.addr = 0x34,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = id_data,
		}
	};
	u8 enable_isp_mode_buf[6] = {0x54,0x36,0x36,0x37,0x30,0x46};//0x57,0x54,0x36,0x36,0x37,0x30,0x46
	u8 enable_isp_flash_mode_buf[2] = {0x02,0x08};//0x10,0x02,0x08
	u8 chip_erase_command_buf[2] = {0x00,0x00};//0x20,0x00,0x00
	u8 finish_command_buf[2] = {0x00,0x00};//0x00,0x00,0x00
	u8 end_command_buf[2] = {0x00,0x02};//0x10,0x00,0x00

    int firmware_length;
    unsigned char *firmware_buf = NULL;
    unsigned char *p = NULL;
    unsigned int i, j;
    u8 high_addr,lowe_addr;
    u8 read_buf[WT6670F_PROGRAM_BUFFER_SIZE] = {0};
    u8 repeat_update_count = 0;
/***************************************************************************************
 *                          Step1, enter isp mode sequence                             *
 ***************************************************************************************/
	gpio_direction_output(wt_chip->reset_gpio, true);
	mdelay(5);
	gpio_direction_output(wt_chip->reset_gpio, false);
	mdelay(1);
	i2c_transfer(client->adapter, enter_isp_mode_msg, 2);

/***************************************************************************************
 *                          Step2, enable isp mode commamd                             *
 ***************************************************************************************/
	mdelay(10);
	wt_chip->client->addr = 0x34;
	ret = wt6670f_i2c_bulk_write(wt_chip, 0x57, enable_isp_mode_buf, 6);
	if (ret < 0) {
		dev_err(wt_chip->dev, "lsw %s: enable_isp_mode fail: %d\n", __func__, ret);
	}

/***************************************************************************************
 *                          Step3, read chip id = 0x70                                 *
 ***************************************************************************************/
	i2c_transfer(client->adapter, read_chip_id_msg, 2);
	if (ret < 0) {
		dev_err(wt_chip->dev, "lsw %s: fail: %d\n", __func__, ret);
		return ret;
	}
	pr_err("lsw %s chip id:0x%x\n", __func__, id_data[0]);

/***************************************************************************************
 *                          Step4, enable isp flash mode command                       *
 ***************************************************************************************/
	ret = wt6670f_i2c_bulk_write(wt_chip, 0x10, enable_isp_flash_mode_buf, 2);
	if (ret < 0) {
		dev_err(wt_chip->dev, "lsw %s: enable_isp_flash_mode fail: %d\n", __func__, ret);
	}

/***************************************************************************************
 *                          Step5, chip erase command                                  *
 ***************************************************************************************/
	ret = wt6670f_i2c_bulk_write(wt_chip, 0x20, chip_erase_command_buf, 2);
	if (ret < 0) {
		dev_err(wt_chip->dev, "lsw %s: enable_isp_flash_mode fail: %d\n", __func__, ret);
	}
	mdelay(30);
	ret = wt6670f_i2c_bulk_write(wt_chip, 0x00, finish_command_buf, 2);
	if (ret < 0) {
		dev_err(wt_chip->dev, "lsw %s: enable_isp_flash_mode fail: %d\n", __func__, ret);
	}
/***************************************************************************************
 *                          Step6, program command(programming 64 byte)                *
 ***************************************************************************************/
repeat_update:
	firmware_buf = WT6670_FW;
    firmware_length = sizeof(WT6670_FW);
    p = firmware_buf;

    for (i = 0; i < (4 * 1024) / WT6670F_PROGRAM_BUFFER_SIZE; i++)
    {
		//distinguish address high byte and low byte
		if(i*WT6670F_PROGRAM_BUFFER_SIZE < 255){
			high_addr = 0;
			lowe_addr = i*WT6670F_PROGRAM_BUFFER_SIZE;
		} else {
			high_addr = i*WT6670F_PROGRAM_BUFFER_SIZE >> 8;
			lowe_addr = (i*WT6670F_PROGRAM_BUFFER_SIZE) & 0xff;
		}

		//set address high byte command
		ret = set_address_high_byte_commond(wt_chip, high_addr);
		if (ret < 0) {
			dev_err(wt_chip->dev, "lsw %s: set_address_high_byte_commond fail: %d\n", __func__, ret);
		}

		wt6670f_i2c_write_block_data(wt_chip, lowe_addr, p, WT6670F_PROGRAM_BUFFER_SIZE);
		if (ret < 0) {
			dev_err(wt_chip->dev, "lsw %s: wt6670f_i2c_write_block_data fail: %d\n", __func__, ret);
		}
		p = p + WT6670F_PROGRAM_BUFFER_SIZE;

		ret = wt6670f_i2c_bulk_write(wt_chip, 0x00, finish_command_buf, 2);
		if (ret < 0) {
			dev_err(wt_chip->dev, "lsw %s: program command fail: %d\n", __func__, ret);
		}
    }


/***************************************************************************************
 *                          Step8, read command                                       *
 ***************************************************************************************/
	for (i = 0; i < (4 * 1024) / WT6670F_PROGRAM_BUFFER_SIZE; i++)
    {
		//distinguish address high byte and low byte
		if(i*WT6670F_PROGRAM_BUFFER_SIZE < 255){
			high_addr = 0;
			lowe_addr = i*WT6670F_PROGRAM_BUFFER_SIZE;
		} else {
			high_addr = i*WT6670F_PROGRAM_BUFFER_SIZE >> 8;
			lowe_addr = (i*WT6670F_PROGRAM_BUFFER_SIZE) & 0xff;
		}

		//set address high byte command
		ret = set_address_high_byte_commond(wt_chip, high_addr);
		if (ret < 0) {
			dev_err(wt_chip->dev, "lsw %s: set_address_high_byte_commond fail: %d\n", __func__, ret);
		}

		ret = wt6670f_read_block_data(wt_chip, lowe_addr, read_buf, WT6670F_PROGRAM_BUFFER_SIZE);
		if (ret < 0) {
			dev_err(wt_chip->dev, "lsw %s: wt6670f_i2c_write_block_data fail: %d\n", __func__, ret);
		}

		for(j = 0;j<WT6670F_PROGRAM_BUFFER_SIZE;j++)
			READ_FW_CHECK[i*WT6670F_PROGRAM_BUFFER_SIZE+j] = read_buf[j];
		//dev_err(wt_chip->dev, "lsw start test %s fw_read_check 0x%x - 0x%x read_buf 0x%x - 0x%x\n", __func__, READ_FW_CHECK[(i*WT6670F_PROGRAM_BUFFER_SIZE)], READ_FW_CHECK[(i*WT6670F_PROGRAM_BUFFER_SIZE)+63], read_buf[0], read_buf[63]);
    }
	if(strcmp(READ_FW_CHECK,WT6670_FW) ==0){
		dev_err(wt_chip->dev, "lsw %s: fw_read_check ok\n", __func__);
	} else {
		repeat_update_count++;
		if(repeat_update_count<=3)
			goto repeat_update;
		else
			dev_err(wt_chip->dev, "lsw %s: fw_read_check update FW failed\n", __func__);
		goto update_fail;
	}
/***************************************************************************************
 *                          Step9, end command                                        *
 ***************************************************************************************/
	ret = wt6670f_i2c_bulk_write(wt_chip, 0x10, end_command_buf, 2);
	if (ret < 0) {
		dev_err(wt_chip->dev, "lsw %s: program command fail: %d\n", __func__, ret);
	}

	wt_chip->client->addr = 0x36;
    dev_err(wt_chip->dev, "lsw_wt6670 [%s] ---- Program successful\n", __func__);
	return 0;
update_fail:
	dev_err(wt_chip->dev, "lsw_wt6670 [%s] ---- Program failed\n", __func__);
	return -1;
}

#if 0
static int wt6670f_enter_sleep_mode(struct wt6670f_info *wt_chip, bool en)
{
	int ret;
	u8 val = 0;

	
	if (en == true) {
		val = ENABLE_SLEEP_MODE;
	} else {
		val = DISABLE_SLEEP_MODE;
	}

	ret = wt6670f_charge_i2c_write(wt_chip, WT6670F_SLEEP_MODE_CMD, val, true);
	if (ret < 0) {
		dev_err(wt_chip->dev, "bus error write: set hvdcp mode: %d\n", ret);
	}

	return ret;
}

static int wt6670f_enter_otg_mode(struct wt6670f_info *wt_chip, bool en)
{
	int ret;
	u8 val = 0;

	if (en == true) {
		val = ENABLE_OTG_MODE;
	}

	ret = wt6670f_charge_i2c_write(wt_chip, WT6670F_ENTER_OTG_MODE_CMD, val, true);
	if (ret < 0) {
		dev_err(wt_chip->dev, "bus error write: set hvdcp mode: %d\n", ret);
	}

	return ret;
}


static int wt6670f_soft_reset(struct wt6670f_info *wt_chip)
{
	int ret;
	u8 val = 0x55;

	ret = wt6670f_charge_i2c_write(wt_chip, WT6670F_SOFT_RESET_CMD, val, true);
	if (ret < 0) {
		dev_err(wt_chip->dev, "bus error write: soft reset: %d\n", ret);
	}

	return ret;
}

static int wt6670f_bc12_detect_enable(struct wt6670f_info *wt_chip)
{
	int ret;
	u8 val;
	u8 reg;
	bool single;

	single = WT6670F_ONE_BYTE;
	reg = WT6670F_BC12_DETECT_CMD;
	val = WT6670F_BC12_ENABLE;
	ret = wt6670f_i2c_write(wt_chip, reg, val, single);
	if (ret < 0) {
		dev_err(wt_chip->dev, "bc12 enable fail !: %d\n", ret);
	}

	return ret;
}
#endif

static int wt6670f_set_hvdcp_mode(struct wt6670f_info *wt_chip, int type)
{
	int ret;
	u8 reg;
	int val;
	bool single;

	switch (type) {
	case POWER_SUPPLY_DP_DM_FORCE_5V:
		val = WT6670F_HVDCP20_5V;
		break;
	case POWER_SUPPLY_DP_DM_FORCE_9V:
		val = WT6670F_HVDCP20_9V;
		break;
	case POWER_SUPPLY_DP_DM_FORCE_12V:
		val = WT6670F_HVDCP20_12V;
		break;
	case POWER_SUPPLY_DP_DM_CONFIRMED_HVDCP3P5:
		val = WT6670F_HVDCP30_5V;
		break;
	default:
		dev_err(wt_chip->dev, "set type: %d is not supported\n", ret);
		ret = -EINVAL;
		return ret;
		break;
	}

	single = WT6670F_ONE_BYTE;
	reg = WT6670F_QC_MODE_SET_CMD;
	ret = wt6670f_i2c_write(wt_chip, reg, val, single);
	if (ret < 0) {
		dev_err(wt_chip->dev, "%s: fail: %d\n", __func__, ret);
	}

	return ret;
}

static int wt6670f_rerun_adsp(struct wt6670f_info *wt_chip)
{
	int ret;
	u8 reg;
	int val = 0x55;
	bool single;

	single = WT6670F_ONE_BYTE;
	reg = WT6670F_RERUN_ADSP_CMD;
	ret = wt6670f_i2c_write(wt_chip, reg, val, single);
	if (ret < 0) {
		dev_err(wt_chip->dev, "%s: fail: %d\n", __func__, ret);
	}

	return ret;
}

static int wt6670f_request_adapter_voltage(struct wt6670f_info *wt_chip, int volt)
{
	u8 reg;
	u8 buf[2];
	int volt_diff;
	int pulse_count;
	int volt_step;
	int len;
	int ret = 0;

	if (volt < WT6670F_VBUS_SET_MIN  || volt > WT6670F_VBUS_SET_MAX) {
		dev_err(wt_chip->dev, "QC30 request voltage over range !\n");
		return ret;
	}

	len = WT6670F_TWO_BYTE;
	volt_diff = abs(volt - wt_chip->current_volt);
	dev_err(wt_chip->dev, "%s:current_volt: %d, request_volt: %d\n",
								__func__, wt_chip->current_volt, volt);
	if (volt > wt_chip->current_volt) {
		wt_chip->volt_request_mode =  WT6670F_VOLT_INCREASE;
	} else if (volt < wt_chip->current_volt) {
		wt_chip->volt_request_mode =  WT6670F_VOLT_DECREASE;
	} else {
		dev_err(wt_chip->dev, "request voltage not change, exit\n");
		return ret;
	}

	switch (wt_chip->vbus_type) {
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		pulse_count = volt_diff /WT6670F_QC30_PULSE_STEP;
		if (wt_chip->volt_request_mode == WT6670F_VOLT_INCREASE) {
			/* Register BAh: bit15 set 1  */
			volt_step = (WT6670F_QC30_RISE_ENABLE & (~WT6670F_QC30_RISE_ENABLE_MASK))
								<< WT6670F_QC30_RISE_ENABLE_SHIFT;

			volt_step |= pulse_count & WT6670F_QC30_PULSE_MASK;
		} else {
			/* Register BAh: bit15 set 0  */
			volt_step = (WT6670F_QC30_DOWN_ENABLE & (~WT6670F_QC30_RISE_ENABLE_MASK))
								<< WT6670F_QC30_RISE_ENABLE_SHIFT;
			volt_step |= pulse_count & WT6670F_QC30_PULSE_MASK;
		}

		buf[0] = volt_step / 256;
		buf[1] = volt_step % 256;
		dev_err(wt_chip->dev, "QC30 mode:%d, volt_step:0x%x, buf[1]:%d, buf[0]:%d\n",
								wt_chip->volt_request_mode, volt_step, buf[1], buf[0]);

		reg = WT6670F_QC30_PULSE_MODE_CMD;
		wt6670f_i2c_bulk_write(wt_chip, reg, buf, len);
		if (ret < 0) {
			dev_err(wt_chip->dev, "%s: QC30 request voltage fail: %d\n", ret);
			return ret;
		}
		break;

	case POWER_SUPPLY_TYPE_USB_HVDCP_3P5:
		pulse_count = volt_diff /WT6670F_QC35_PULSE_STEP;
		if (wt_chip->volt_request_mode == WT6670F_VOLT_INCREASE) {
			/* Register BBh: bit15 set 1  */
			volt_step = (WT6670F_QC35_RISE_ENABLE & (~WT6670F_QC35_RISE_ENABLE_MASK))
								<< WT6670F_QC35_RISE_ENABLE_SHIFT;

			volt_step |= pulse_count & WT6670F_QC35_PULSE_MASK;
		} else {
			/* Register BBh: bit15 set 0  */
			volt_step = (WT6670F_QC35_DOWN_ENABLE & (~WT6670F_QC35_RISE_ENABLE_MASK))
								<< WT6670F_QC35_RISE_ENABLE_SHIFT;
			volt_step |= pulse_count & WT6670F_QC35_PULSE_MASK;
		}

		buf[0] = volt_step / 256;
		buf[1] = volt_step % 256;
		dev_err(wt_chip->dev, "QC35 mode:%d, volt_step:0x%x, buf[1]:%d, buf[0]:%d\n",
								wt_chip->volt_request_mode, volt_step, buf[1], buf[0]);

		reg = WT6670F_QC35_PULSE_MODE_CMD;
		wt6670f_i2c_bulk_write(wt_chip, reg, buf, len);
		if (ret < 0) {
			dev_err(wt_chip->dev, "%s: QC35 request voltage fail: %d\n", ret);
			return ret;
		}
		break;

	default:
		break;
	}

	return ret;
}

static int wt6670f_get_vbus_voltage(struct wt6670f_info *wt_chip)
{
	int ret;
	u8 reg;
	int len;
	u8 buf[2];
	int adc_volt;
	float temp_volt;
	float vref_volt = 2.4;

	len = WT6670F_TWO_BYTE;
	reg = WT6670F_VBUS_VOLTAGE_CMD;
	ret = wt6670f_i2c_bulk_read(wt_chip, reg, buf, len);
	if (ret < 0) {
		dev_err(wt_chip->dev, "%s: fail: %d\n", __func__, ret);
		return ret;
	}

	adc_volt = (buf[0] << WT6670F_BYTE_BIT) |buf[1];
	temp_volt = ((adc_volt * (vref_volt * WT6670F_VOLT_MV)) /WT6670F_ADC_BIT)
		 *( (VBUS_PULLUP_R1 + VBUS_PULLUP_R2 + VBUS_PULLDOWN_R) /VBUS_PULLDOWN_R);
	wt_chip->vbus_volt  = (int)temp_volt;
	dev_info(wt_chip->dev, "%s: buf[0]: 0x%x, buf[1]: 0x%x, adc_volt: 0x%x, vbus_volt: %d\n",
						__func__, buf[0], buf[1], adc_volt, wt_chip->vbus_volt);

	return ret;
}

static int wt6670f_get_firmware_version(struct wt6670f_info *wt_chip)
{
	int ret;
	u8 reg;
	bool single;

	single = WT6670F_ONE_BYTE;
	reg = WT6670F_FIRMWARE_VERSION_CMD;
	ret = wt6670f_i2c_read(wt_chip, reg, single);
	if (ret < 0) {
		dev_err(wt_chip->dev, "%s: fail: %d\n", __func__, ret);
		return ret;
	}

	wt_chip->firmware_version = ret;
        dev_err(wt_chip->dev, "%s:firmware_version = 0x%x\n", __func__, ret);

	return ret;
}

static int wt6670f_get_charger_type(struct wt6670f_info *wt_chip)
{
	int ret;
	u8 reg;
	bool single;
	int chg_type = 0;

	single = WT6670F_ONE_BYTE;
	reg = WT6670F_CHARGE_TYPE_CMD;
	ret = wt6670f_i2c_read(wt_chip, reg, single);
	if (ret < 0) {
		dev_err(wt_chip->dev, "%s: fail: %d\n", __func__, ret);
		return ret;
	}
	
	chg_type = ret & WT6670F_CHARGE_TYPE_MASK;
	switch (chg_type) {
	case WT6670F_CHARGE_TYPE_SDP:
		wt_chip->vbus_type = POWER_SUPPLY_TYPE_USB;
		break;
	case WT6670F_CHARGE_TYPE_CDP:
		wt_chip->vbus_type = POWER_SUPPLY_TYPE_USB_CDP;
		break;
	case WT6670F_CHARGE_TYPE_DCP:
		wt_chip->vbus_type = POWER_SUPPLY_TYPE_USB_DCP;
		break;
	case WT6670F_CHARGE_TYPE_QC20:
		wt_chip->vbus_type = POWER_SUPPLY_TYPE_USB_HVDCP;
		break;
	case WT6670F_CHARGE_TYPE_QC30:
		wt_chip->vbus_type = POWER_SUPPLY_TYPE_USB_HVDCP_3;
		break;
	case WT6670F_CHARGE_TYPE_QC35_18W:
	case WT6670F_CHARGE_TYPE_QC35_27W:
		wt_chip->vbus_type = POWER_SUPPLY_TYPE_USB_HVDCP_3P5;
		break;
	default:
		wt_chip->vbus_type = POWER_SUPPLY_TYPE_USB_FLOAT;
		break;
	}

	return ret;
}

static irqreturn_t wt6670f_charge_irq_handler(int irq, void *data)
{
	struct wt6670f_info *wt_chip = data;

	dev_err(wt_chip->dev, "%s: start !!!\n", __func__);
	schedule_work(&wt_chip->irq_handle_work);
	return IRQ_HANDLED;
}

static void wt6670f_charge_irq_workfunc(struct work_struct *work)
{
	struct wt6670f_info *wt_chip = container_of(work,
									struct wt6670f_info, irq_handle_work);
	int ret;
	union power_supply_propval val = {0,};

	wt6670f_get_charger_type(wt_chip);
	//+ ExtB oak-3798, tankaikun@wt, add 20220126, fix plug in charger slowly cause identify float type
	if(wt_chip->vbus_type == POWER_SUPPLY_TYPE_USB_FLOAT && wt_chip->float_charge_recheck_cnt++ < 3){
	   pr_err(" wt6670f_charge_irq_workfunc rerun again \n");
	   msleep(100);
	   wt6670f_rerun_adsp(wt_chip);
	   return ;
	}
	//- ExtB oak-3798, tankaikun@wt, add 20220126, fix plug in charger slowly cause identify float type
	wt_chip->usb_psy = power_supply_get_by_name("usb");
	if(wt_chip->usb_psy) {
		val.intval = wt_chip->vbus_type;
		ret = power_supply_set_property(wt_chip->usb_psy,
								POWER_SUPPLY_PROP_REAL_TYPE, &val);
		if (ret < 0) {
			dev_err(wt_chip->dev,
					"Couldn't set power_supply_prop_usb_type: ret = %d\n",  ret);
		}

		ret = power_supply_set_property(wt_chip->usb_psy,
								POWER_SUPPLY_PROP_DP_DM, &val);
		if (ret < 0) {
			dev_err(wt_chip->dev,
					"Couldn't set POWER_SUPPLY_PROP_DP_DM: ret = %d\n",  ret);
		}
	}

	wt_chip->main_chg_psy = power_supply_get_by_name("main_chg");
	if(wt_chip->main_chg_psy) {
		ret = power_supply_set_property(wt_chip->main_chg_psy,
								POWER_SUPPLY_PROP_DP_DM, &val);
		if (ret < 0) {
			dev_err(wt_chip->dev,
					"Couldn't set POWER_SUPPLY_PROP_DP_DM: ret = %d\n",  ret);
		}
	}

	//cancel_delayed_work_sync(&wt_chip->monitor_work);
	//schedule_delayed_work(&wt_chip->monitor_work, msecs_to_jiffies(200));
}

static void wt6670f_monitor_workfunc(struct work_struct *work)
{
	struct wt6670f_info *wt_chip = container_of(work,
									struct wt6670f_info, monitor_work.work);
#if 0	
	//for qc30 test
	static int monitor_count = 1;
	int request_mode;

	if (wt_chip->vbus_type == POWER_SUPPLY_TYPE_USB_HVDCP_3
			||wt_chip->vbus_type == POWER_SUPPLY_TYPE_USB_HVDCP_3P5) {
		monitor_count++;
	} else {

		monitor_count = 1;
	}

	request_mode = monitor_count % 2;
	if (wt_chip->vbus_type == POWER_SUPPLY_TYPE_USB_HVDCP_3) {
		if (request_mode == 1) {
			wt6670f_request_adapter_voltage(wt_chip, 8000);
			wt_chip->current_volt = 8000;
		} else {
			wt6670f_request_adapter_voltage(wt_chip, 5000);
			wt_chip->current_volt = 5000;
		}
	} else if  (wt_chip->vbus_type == POWER_SUPPLY_TYPE_USB_HVDCP_3P5) {
		if (request_mode == 1) {
			wt6670f_request_adapter_voltage(wt_chip, 5500);
			wt_chip->current_volt = 5500;
		} else {
			wt6670f_request_adapter_voltage(wt_chip, 5000);
			wt_chip->current_volt = 5000;
		}
	}
#endif

	wt6670f_get_charger_type(wt_chip);
	wt6670f_get_vbus_voltage(wt_chip);
	//schedule_delayed_work(&wt_chip->monitor_work, msecs_to_jiffies(10000));
}


/************************
 * USB PSY REGISTRATION *
 ************************/
static enum power_supply_property wt6670f_usb_props[] = {
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_TYPE,
	POWER_SUPPLY_PROP_REAL_TYPE,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_HVDCP_OPTI_ALLOWED,
	POWER_SUPPLY_PROP_APSD_RERUN,
	POWER_SUPPLY_PROP_INPUT_VOLTAGE_SETTLED,
};

static int wt6670f_usb_get_prop(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct wt6670f_info *wt_chip = power_supply_get_drvdata(psy);
	int ret = 0;
	val->intval = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = wt_chip->vbus_volt_max * 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		wt6670f_get_vbus_voltage(wt_chip);
		val->intval = wt_chip->vbus_volt * 1000;
		break;

	case POWER_SUPPLY_PROP_TYPE:
	case POWER_SUPPLY_PROP_REAL_TYPE:
		val->intval = wt_chip->vbus_type;
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


static int wt6670f_usb_set_prop(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct wt6670f_info *wt_chip = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_APSD_RERUN:
		// ExtB oak-3798, tankaikun@wt, add 20220126, fix plug in charger slowly cause identify float type
		wt_chip->float_charge_recheck_cnt = 0;
		ret = wt6670f_rerun_adsp(wt_chip);
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		wt_chip->current_volt = val->intval;
		break;

	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_SETTLED:
		ret = wt6670f_request_adapter_voltage(wt_chip, val->intval);
		break;

	case POWER_SUPPLY_PROP_HVDCP_OPTI_ALLOWED:
		ret = wt6670f_set_hvdcp_mode(wt_chip, val->intval);
		break;

	default:
		dev_err(wt_chip->dev, "set prop: %d is not supported\n", psp);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int wt6670f_usb_prop_is_writeable(struct power_supply *psy,
		enum power_supply_property psp)
{

	switch (psp) {
	case POWER_SUPPLY_PROP_APSD_RERUN:
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_SETTLED:
	case POWER_SUPPLY_PROP_HVDCP_OPTI_ALLOWED:
		return 1;

	default:
		break;
	}

	return 0;
}

static const struct power_supply_desc usb_psy_desc = {
	.name = "wt_hvdcp",
	.type = POWER_SUPPLY_TYPE_MAIN,
	.properties = wt6670f_usb_props,
	.num_properties = ARRAY_SIZE(wt6670f_usb_props),
	.get_property = wt6670f_usb_get_prop,
	.set_property = wt6670f_usb_set_prop,
	.property_is_writeable = wt6670f_usb_prop_is_writeable,
};

static int wt6670f_init_usb_psy(struct wt6670f_info *wt_chip)
{
	struct power_supply_config usb_cfg = {};

	usb_cfg.drv_data = wt_chip;
	usb_cfg.of_node = wt_chip->dev->of_node;
	wt_chip->wt_hvdcp_psy = devm_power_supply_register(wt_chip->dev,
						  &usb_psy_desc,
						  &usb_cfg);
	if (IS_ERR(wt_chip->wt_hvdcp_psy)) {
		pr_err("Couldn't register USB power supply\n");
		return PTR_ERR(wt_chip->wt_hvdcp_psy);
	}

	return 0;
}

static int wt6670_parse_dt(struct wt6670f_info *chip, struct device *dev)
{
	struct device_node *np = dev->of_node;
	int ret = 0;

	ret = of_get_named_gpio(np, "wt6670f,reset-gpio", 0);
	if (ret < 0) {
		pr_err("lsw %s no wt6670f,reset-gpio info\n", __func__);
		return ret;
	}
	chip->reset_gpio = ret;

	return ret < 0 ? ret : 0;
}

static int wt6670f_charge_probe(struct i2c_client *client,
				     const struct i2c_device_id *id)
{
	struct wt6670f_info *wt_chip;
	int ret;
	char *name;
	int num;

	/* Get new ID for the new battery device */
	mutex_lock(&wtchg_mutex);
	num = idr_alloc(&wt6670f_id, client, 0, 0, GFP_KERNEL);
	mutex_unlock(&wtchg_mutex);
	if (num < 0)
		return num;

	name = devm_kasprintf(&client->dev, GFP_KERNEL, "%s-%d", id->name, num);
	if (!name)
		goto err_mem;

	wt_chip = devm_kzalloc(&client->dev, sizeof(*wt_chip), GFP_KERNEL);
	if (!wt_chip)
		goto err_mem;

	wt_chip->id = num;
	wt_chip->dev = &client->dev;
	wt_chip->client = client;
	wt_chip->chip = id->driver_data;
	wt_chip->name = name;
	wt_chip->vbus_volt_max = WT6670F_VBUS_SET_MAX;
	wt_chip->float_charge_recheck_cnt = 0; // ExtB oak-3798, tankaikun@wt, add 20220126, fix plug in charger slowly cause identify float type

	ret = wt6670f_init_usb_psy(wt_chip);
	if (ret < 0) {
		pr_err("Couldn't initialize usb psy rc=%d\n", ret);
		goto err_mem;
	}

	i2c_set_clientdata(client, wt_chip);

	ret = wt6670_parse_dt(wt_chip, &client->dev);
	if (ret < 0)
		return ret;
	if(wt6670f_get_firmware_version(wt_chip) != WT6670_FW_VERSION){
		dev_err(wt_chip->dev, "lsw_wt6670 [%s] ---- FW_VERSION is not new,start to update!\n", __func__);
		update_firmware(wt_chip);
	} else {
		dev_err(wt_chip->dev, "lsw_wt6670 [%s] ---- FW_VERSION is new,no need to update!\n", __func__);
	}

	if (client->irq) {
		ret = devm_request_threaded_irq(&client->dev, client->irq,
				NULL, wt6670f_charge_irq_handler,
				IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				wt_chip->name, wt_chip);
		if (ret) {
			dev_err(&client->dev,
				"Unable to register IRQ %d error %d\n", client->irq, ret);
			return ret;
		}
	}

	INIT_WORK(&wt_chip->irq_handle_work, wt6670f_charge_irq_workfunc);
	INIT_DELAYED_WORK(&wt_chip->monitor_work, wt6670f_monitor_workfunc);
	wt6670f_get_firmware_version(wt_chip);
	schedule_delayed_work(&wt_chip->monitor_work, msecs_to_jiffies(10000));
	pr_info("wt6670f charge probe successfully\n");

	return 0;

err_mem:
	ret = -ENOMEM;
	mutex_lock(&wtchg_mutex);
	idr_remove(&wt6670f_id, num);
	mutex_unlock(&wtchg_mutex);

	return ret;
}

static int wt6670f_charge_remove(struct i2c_client *client)
{
	struct wt6670f_info *wt_chip = i2c_get_clientdata(client);

	mutex_lock(&wtchg_mutex);
	idr_remove(&wt6670f_id, wt_chip->id);
	mutex_unlock(&wtchg_mutex);

	return 0;
}

static const struct i2c_device_id wt6670f_i2c_id_table[] = {
	{ "wt6670f", WT6670F },
	{},
};
MODULE_DEVICE_TABLE(i2c, wt6670f_i2c_id_table);

#ifdef CONFIG_OF
static const struct of_device_id wt6670f_charge_i2c_of_match_table[] = {
	{ .compatible = "weltrend,wt6670f" },
	{},
};
MODULE_DEVICE_TABLE(of, wt6670f_charge_i2c_of_match_table);
#endif

static struct i2c_driver wt6670f_charge_i2c_driver = {
	.driver = {
		.name = "wt6670f-charge",
		.of_match_table = of_match_ptr(wt6670f_charge_i2c_of_match_table),
	},
	.probe = wt6670f_charge_probe,
	.remove = wt6670f_charge_remove,
	.id_table = wt6670f_i2c_id_table,
};
module_i2c_driver(wt6670f_charge_i2c_driver);

MODULE_AUTHOR("Allan.ouyang <yangpingao@wingtech.com>");
MODULE_DESCRIPTION("wt6670f qc30 protocol i2c driver");
MODULE_LICENSE("GPL");
