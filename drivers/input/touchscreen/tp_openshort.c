#include <linux/kernel.h>
#include <linux/initrd.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/slab.h>
//+OAK78,shenwenbin.wt,MOD,20211115,tuning double wakeup
#include <linux/types.h>
#include <linux/uaccess.h>
//-OAK78,shenwenbin.wt,MOD,20211115,tuning double wakeup

#ifdef CONFIG_TOUCHSCREEN_HIMAX_CHIPSET
extern uint32_t g_hx_chip_inited;
extern int himax_self_test(struct seq_file *s, void *v);
#endif

//+bug697279,shenwenbin.wt,MOD,20211022,bringup selftest function
#ifdef CONFIG_TOUCHSCREEN_NT36xxx_HOSTDL_SPI
extern uint8_t NVTTouchProbe;
extern int32_t nvt_selftest(struct seq_file *s, void *v);
#endif
//-bug697279,shenwenbin.wt,MOD,20211022,bringup selftest function

uint8_t DoubleWakeUpEnable = 1;	//OAK78,shenwenbin.wt,MOD,20211115,tuning double wakeup

static struct proc_dir_entry *tp_proc_root;
static void *tp_os_start(struct seq_file *seq, loff_t *pos)
{
        if (pos) {
                printk("%s\n",__func__);
                return *pos < 1 ? pos : NULL;
        }
        return NULL;
}

static void *tp_os_next(struct seq_file *seq, void *v, loff_t *pos)
{
        return (void *)(*pos);
}

static void tp_os_stop(struct seq_file *seq, void *v)
{
}

static int tp_os_show(struct seq_file *seq, void *v)
{
	int ret = -1;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_CHIPSET
	if (g_hx_chip_inited == true) {
		ret = himax_self_test(seq,v);
        printk("himax_self_test func %s ret = %d\n",__func__, ret);
	}
#endif

//+bug697279,shenwenbin.wt,MOD,20211022,bringup selftest function
#ifdef CONFIG_TOUCHSCREEN_NT36xxx_HOSTDL_SPI
			if (NVTTouchProbe == true) {
				ret = nvt_selftest(seq,v);
				printk("nvt_selftest func %s ret = %d\n",__func__, ret);
			}
#endif
//-bug697279,shenwenbin.wt,MOD,20211022,bringup selftest function

        printk("func %s ret = %d\n",__func__, ret);
        if (!ret)
                seq_printf(seq, "result=1");
        return 0;
}


static const struct seq_operations tp_os_ops = {
        .start  = tp_os_start,
        .next   = tp_os_next,
        .stop   = tp_os_stop,
        .show   = tp_os_show,
};

static int tp_os_open(struct inode *inode, struct file *file)
{
        return seq_open(file, &tp_os_ops);
}

static const struct file_operations tp_os_fops = {
        .owner          = THIS_MODULE,
        .open           = tp_os_open,
        .read           = seq_read,
        .llseek         = seq_lseek,
        .release        = seq_release,
};

//+OAK78,shenwenbin.wt,MOD,20211115,tuning double wakeup
static ssize_t Double_WakeUp_read(struct file *file, char *buf,
		size_t len, loff_t *pos)
{
	size_t count = 0;
	char *temp_buf = NULL;

	temp_buf = kcalloc(len, sizeof(char), GFP_KERNEL);
	if (temp_buf != NULL) {
			count = snprintf(temp_buf, PAGE_SIZE, "%d\n",
					DoubleWakeUpEnable);

			if (copy_to_user(buf, temp_buf, len))
			printk("%s, DoubleWakeUpEnable:%d\n", __func__, __LINE__);

			kfree(temp_buf);
	} else {
			printk("%s, Failed to allocate memory\n", __func__);
	}

	return count;
}

static ssize_t Double_WakeUp_write(struct file *file, const char *buff,
		size_t len, loff_t *pos)
{
	char buf[80] = {0};

	if (len >= 80) {
		printk("%s: no command exceeds 80 chars.\n", __func__);
		return -EFAULT;
	}

	if (copy_from_user(buf, buff, len))
		return -EFAULT;

	if (buf[0] == '0')
		DoubleWakeUpEnable = 0;
	else if (buf[0] == '1')
		DoubleWakeUpEnable = 1;
	else
		return -EINVAL;

	return len;
}

static const struct file_operations Double_WakeUp_ops = {
	.owner = THIS_MODULE,
	.read = Double_WakeUp_read,
	.write = Double_WakeUp_write,
};

uint8_t Double_WakeUp_Status(void){
	return DoubleWakeUpEnable;
}
EXPORT_SYMBOL(Double_WakeUp_Status);
//-OAK78,shenwenbin.wt,MOD,20211115,tuning double wakeup

static int  __init tp_openshort_init(void)
{
        struct proc_dir_entry *p;

        tp_proc_root = proc_mkdir("touchscreen", NULL);

        if (!tp_proc_root) goto create_gmnode;

        p = proc_create("ctp_openshort_test", 0444, tp_proc_root, &tp_os_fops);

        if (!p) {
        	remove_proc_entry("touchscreen", NULL);
		tp_proc_root = NULL;
		goto create_gmnode;
	}

	//+OAK78,shenwenbin.wt,MOD,20211115,tuning double wakeup
	p = proc_create("gesture_control", 0666, NULL, &Double_WakeUp_ops);	//OAK78,shenwenbin.wt,MOD,20211201, double wakeup node path as app
	if (!p) {
		printk("%s: create touch_double_wakeup error.\n",__func__);
	}
	//-OAK78,shenwenbin.wt,MOD,20211115,tuning double wakeup

        return 0;

create_gmnode:

	p = proc_create("gmnode00000100", 0440, NULL, &tp_os_fops);

	if (!p)
        	return -1;
	return 0;
}

static void __exit tp_openshort_exit(void)
{
	if (tp_proc_root) {
        	remove_proc_entry("ctp_openshort_test", tp_proc_root);
        	remove_proc_entry("touchscreen", NULL);
	} else
		remove_proc_entry("gmnode00000100", NULL);
}

MODULE_LICENSE("GPL");
module_init(tp_openshort_init);
module_exit(tp_openshort_exit);
