#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/of_fdt.h>
#include <asm/setup.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/genhd.h>
#include <asm/uaccess.h>
#include <linux/utsname.h>
#include <linux/platform_device.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include "partition_rw.h"

struct gendisk *ufs_disk_partition;

static dev_t lookup_partition(dev_t devt,const char *part_name, sector_t *start, sector_t *nr_sect)
{
    struct disk_part_iter piter;
    struct hd_struct *part;

    if (!ufs_disk_partition) {
        printk("[mzss] emmc disk = null\n");
        return devt;

    }
    disk_part_iter_init(&piter, ufs_disk_partition, DISK_PITER_INCL_EMPTY);
    while ((part = disk_part_iter_next(&piter))) {
        if (part->info && !strcmp(part->info->volname, part_name)) {
            devt = part->__dev.devt;
            *start = (part->start_sect)/8;
            *nr_sect = (part->nr_sects)/8;
            break;

        }
    }
    disk_part_iter_exit(&piter);
 
    return devt;
}

static int ufs_block_rw(int write, sector_t index, void *buffer, size_t len)
{
    struct block_device *bdev;
    struct buffer_head *bh = NULL;
    fmode_t mode = FMODE_READ;
    int block_size,err = -EIO;

    block_size = 4096;
    if (len > block_size)
        return -EINVAL;

    bdev = bdget(MKDEV(ufs_disk_partition->major, 0));
    if (!bdev)
        return -EIO;

    mode = write ? FMODE_WRITE : FMODE_READ;
    if (blkdev_get(bdev, mode, NULL)) {
        bdput(bdev);
        goto out;
    }
    bh = __getblk(bdev, index, block_size);
 
    if (bh) {
        if (!write) {
            clear_buffer_uptodate(bh);
            get_bh(bh);
            lock_buffer(bh);
            bh->b_end_io = end_buffer_read_sync;
            submit_bh(REQ_OP_READ, 0, bh);
            wait_on_buffer(bh);
            if (unlikely(!buffer_uptodate(bh))) {
                pr_err("emmc read error!!\n");
                brelse(bh);
                goto out;
            }
            memcpy(buffer, bh->b_data, len);
        } else {
            lock_buffer(bh);
            memcpy(bh->b_data, buffer, len);
            bh->b_end_io = end_buffer_write_sync;
            get_bh(bh);
            submit_bh(REQ_OP_WRITE, 1, bh);
            wait_on_buffer(bh);
            pr_err("emmc go to write sucess!!\n");
            if (unlikely(!buffer_uptodate(bh))) {
                pr_err("emmc write error!!\n");
                brelse(bh);
                goto out;
            }
        }
        err = 0;
    } else {
        pr_info("%s error\n", __func__);
    }

out:
    blkdev_put(bdev, mode);
    return err;


}
 
int ufs_partition_rw(const char *part_name, int write, loff_t offset,void *buffer, size_t len)
{
    int block_size,ret = 0;
    sector_t index;
    void *p = buffer;
    struct block_device *bdev;
    sector_t start=0, nr_sect=0;
    if (!ufs_disk_partition) {
        printk("[mzss] ufs disk lun-sda = null\n");
        return -EINVAL;
    }
    bdev = bdget(MKDEV(ufs_disk_partition->major, 0));
    block_size = 4096;
    //set_blocksize(bdev, block_size);
    if (buffer == NULL)
        return -EINVAL;

    if (offset % block_size) {
        printk("[mzss]%s: offset(%lld) unalign to 512Byte!\n", __func__, offset);
        return -EINVAL;
    }
 
    if (!(lookup_partition(bdev->bd_dev, part_name, &start, &nr_sect))) {
        printk("[mzss]%s: can't find eMMC partition(%s)\n", __func__, part_name);
        return -ENODEV;
    }
 
    if (offset < 0 || (offset + len) >= nr_sect * block_size) {
        printk("[mzss]%s: access area exceed parition(%s) range.\n", __func__, part_name);
        return -EINVAL;
    }
 
    index = start + offset / block_size;
    
    while (len > 0) {
        size_t size = len;
 
        if (size > block_size)
            size = block_size;

        ret = ufs_block_rw(write, index, p, size);
        if (ret) {
            printk("[mzss]%s (%lu) error %d\n", __func__, (unsigned long)len, ret);
            break;
        }
 
        len -= size;
        index++;
        p += block_size;
    }
 
    return ret;
}

EXPORT_SYMBOL(ufs_partition_rw);
