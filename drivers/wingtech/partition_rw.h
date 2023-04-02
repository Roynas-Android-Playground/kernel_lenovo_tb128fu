#ifndef __PRIVATE_SLOT_RW_H__
#define __PRIVATE_SLOT_RW_H__

int ufs_partition_rw(const char *part_name, int write, loff_t offset,void *buffer, size_t len);

#endif /* !__PRIVATE_SLOT_RW_H__ */
