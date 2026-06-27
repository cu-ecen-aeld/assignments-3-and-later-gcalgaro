/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesdchar.h"
#include "aesd_ioctl.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("gcalgaro");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;
    PDEBUG("open");
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset = 0;
    size_t bytes_to_read = 0;

    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->circular_buffer, 
                                                                *f_pos, &entry_offset);

    if(!entry)
    {
        retval = 0;
        goto out;
    }

    bytes_to_read = entry->size - entry_offset;
    if (bytes_to_read > count)
        bytes_to_read = count;

    if (copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_read))
    {
        retval = -EFAULT;
        goto out;
    }
    
    *f_pos += bytes_to_read;
    retval = bytes_to_read;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    char *newline;
    char *combined_buffer;
    char *combined;
    size_t combined_size = 0;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    combined_buffer = kmalloc(count, GFP_KERNEL);
    if (!combined_buffer)
        return -ENOMEM;

    if (copy_from_user(combined_buffer, buf, count)) 
    {
        kfree(combined_buffer);
        return -EFAULT;
    }

    if (mutex_lock_interruptible(&dev->lock))
    {
        kfree(combined_buffer);
        return -ERESTARTSYS;
    }

    combined_size = dev->partial_entry_size + count;
    combined = kmalloc(combined_size, GFP_KERNEL);
    if (!combined)
    {
        mutex_unlock(&dev->lock);
        kfree(combined_buffer);
        return -ENOMEM;
    }

    if (dev->partial_entry_size > 0)
        memcpy(combined, dev->partial_entry, dev->partial_entry_size);

    memcpy(combined + dev->partial_entry_size, combined_buffer, count);
    
    kfree(dev->partial_entry);
    kfree(combined_buffer);

    dev->partial_entry = combined;
    dev->partial_entry_size = combined_size;

    newline = memchr(dev->partial_entry, '\n', dev->partial_entry_size);
    if (newline)
    {
        size_t entry_len = (newline - dev->partial_entry) + 1;
        struct aesd_buffer_entry new_entry;
        const struct aesd_buffer_entry *freed_entry;

        if (dev->circular_buffer.full)
        {
            freed_entry = &dev->circular_buffer.entry[dev->circular_buffer.out_offs];
            kfree(freed_entry->buffptr);
        }

        new_entry.buffptr = dev->partial_entry;
        new_entry.size = entry_len;
        aesd_circular_buffer_add_entry(&dev->circular_buffer, &new_entry);

        if (entry_len < dev->partial_entry_size)
        {
            size_t leftover_size = dev->partial_entry_size - entry_len;
            char *leftover = kmalloc(leftover_size, GFP_KERNEL);
            if (leftover)
                memcpy(leftover, dev->partial_entry + entry_len, leftover_size);

            dev->partial_entry = leftover;
            dev->partial_entry_size = leftover ? leftover_size : 0;
        } else {
            dev->partial_entry = NULL;
            dev->partial_entry_size = 0;
        }
    }

    retval = count;
    mutex_unlock(&dev->lock);
    return retval;
}

loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    loff_t retval = 0;
    size_t combined_size = 0;
    size_t index = 0;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->circular_buffer, index)
    {
        if (entry->buffptr != NULL)
            combined_size += entry->size;
    }

    switch (whence) 
    {
        case SEEK_SET:
            retval = offset;
            break;
        case SEEK_CUR:
            retval = filp->f_pos + offset;
            break;
        case SEEK_END:
            retval = combined_size + offset;
            break;
        default:
            retval = -EINVAL;
            goto out;
    }

    if (retval < 0 || retval > combined_size)
    {
        retval = -EINVAL;
        goto out;
    }
    else
        filp->f_pos = retval;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

static long aesd_adjust_file_offset(struct file *filp, uint32_t write_cmd, uint32_t write_cmd_offset)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    loff_t f_pos = 0;
    size_t index = 0;
    size_t max_entries = 0;
    size_t cmd_index = 0;
    long retval = 0;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->circular_buffer, index)
    {
        if (entry->buffptr != NULL)
            max_entries++;
    }

    if (write_cmd >= max_entries)
    {
        retval = -EINVAL;
        goto out;
    }

    index = 0;
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->circular_buffer, index)
    {
        if (entry->buffptr != NULL)
        {
            if (cmd_index == write_cmd)
            {
                if (write_cmd_offset > entry->size)
                {
                    retval = -EINVAL;
                    goto out;
                }
                f_pos += write_cmd_offset;
                filp->f_pos = f_pos;
                retval = 0;
                goto out;
            }
            f_pos += entry->size;
            cmd_index++;
        }
    }
out:
    mutex_unlock(&dev->lock);
    return retval;
}

static long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    long retval = 0;

    switch (cmd)
    {
        case AESDCHAR_IOCSEEKTO:
            {
                struct aesd_seekto seekto;
                if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)) != 0)
                    retval = -EFAULT;
                else
                    retval = aesd_adjust_file_offset(filp, seekto.write_cmd, seekto.write_cmd_offset);
                break;
            }
        default:
            return -EINVAL;
    }

    return retval;
}

struct file_operations aesd_fops = {
    .owner          = THIS_MODULE,
    .llseek         = aesd_llseek,
    .read           = aesd_read,
    .write          = aesd_write,
    .unlocked_ioctl = aesd_ioctl,
    .open           = aesd_open,
    .release        = aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1, "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) 
    {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device, 0, sizeof(struct aesd_dev));

    aesd_circular_buffer_init(&aesd_device.circular_buffer);
    mutex_init(&aesd_device.lock);
    aesd_device.partial_entry = NULL;
    aesd_device.partial_entry_size = 0;

    result = aesd_setup_cdev(&aesd_device);

    if( result )
        unregister_chrdev_region(dev, 1);

    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    size_t index;
    struct aesd_buffer_entry *entry;

    cdev_del(&aesd_device.cdev);

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.circular_buffer, index)
    {
        if (entry->buffptr)
            kfree(entry->buffptr);
    }

    if (aesd_device.partial_entry)
        kfree(aesd_device.partial_entry);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);