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

MODULE_AUTHOR("Gino Calgaro");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    struct aesd_dev *dev;
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
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);

    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entryOffset = 0;
    size_t bytesToRead = 0;
    
    if (mutex_lock_interruptible(&dev->lock))
    {
        return -ERESTARTSYS;
    }

    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->circular_buffer, *f_pos, &entryOffset);

    if (!entry)
    {
        retval = 0;
        goto out;
    }

    bytesToRead = entry->size - entryOffset;

    if (bytesToRead > count)
    {
        bytesToRead = count;
    }

    if (copy_to_user(buf, entry->buffptr + entryOffset, bytesToRead))
    {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += bytesToRead;
    retval = bytesToRead;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    struct aesd_dev *dev = filp->private_data;
    char *newLine;
    char *combinedBuffer;
    char *combined;
    size_t combinedSize = 0;
    
    combinedBuffer = kmalloc(count, GFP_KERNEL);

    if (!combinedBuffer)
    {
        return -ENOMEM;
    }

    if (copy_from_user(combinedBuffer, buf, count))
    {
        kfree(combinedBuffer);
        return -EFAULT;
    }

    if (mutex_lock_interruptible(&dev->lock))
    {
        kfree(combinedBuffer);
        return -ERESTARTSYS;
    }

    combinedSize = dev->partial_entry_size + count;
    combined = kmalloc(combinedSize, GFP_KERNEL);

    if (!combined)
    {
        mutex_unlock(&dev->lock);
        kfree(combinedBuffer);
        return -ENOMEM;
    }

    if (dev->partial_entry_size > 0)
    {
        memcpy(combined, dev->partial_entry, dev->partial_entry_size);
    }

    memcpy(combined + dev->partial_entry_size, combinedBuffer, count);

    kfree(dev->partial_entry);
    kfree(combinedBuffer);

    dev->partial_entry = combined;
    dev->partial_entry_size = combinedSize;

    newLine = memchr(dev->partial_entry, '\n', dev->partial_entry_size);

    if (newLine)
    {
        size_t entry_len = (newLine - dev->partial_entry) + 1;
        struct aesd_buffer_entry newEntry;
        const struct aesd_buffer_entry *freedEntry;

        if (dev->circular_buffer.full)
        {
            freedEntry = &dev->circular_buffer.entry[dev->circular_buffer.out_offs];
            kfree(freedEntry->buffptr);
        }

        newEntry.buffptr = dev->partial_entry;
        newEntry.size = entry_len;

        aesd_circular_buffer_add_entry(&dev->circular_buffer, &newEntry);

        if (entry_len < dev->partial_entry_size)
        {
            size_t remainderSize = dev->partial_entry_size - entry_len;
            char *remainder = kmalloc(remainderSize, GFP_KERNEL);

            if (remainder)
            {
                memcpy(remainder, dev->partial_entry + entry_len, remainderSize);
            }

            dev->partial_entry = remainder;
            dev->partial_entry_size = remainder ? remainderSize : 0;
        }
        else
        {
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
    size_t combinedSize = 0;
    size_t index = 0;

    if (mutex_lock_interruptible(&dev->lock))
    {
        return -ERESTARTSYS;
    }

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->circular_buffer, index)
    {
        if (entry->buffptr != NULL)
        {
            combinedSize += entry->size;
        }
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
            retval = combinedSize + offset;
            break;
        default:
            retval = -EINVAL;
            goto out;
    }

    if (retval < 0 || retval > combinedSize)
    {
        retval = -EINVAL;
        goto out;
    }
    else
    {
        filp->f_pos = retval;
    }

out:
    mutex_unlock(&dev->lock);
    return retval;
}

static long aesd_adjust_file_offset(struct file *filp, uint32_t writeCmd, uint32_t writeCmdOffset)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    loff_t f_pos = 0;
    size_t index = 0;
    size_t maxEntries = 0;
    size_t cmdIndex = 0;
    long retval = 0;

    if (mutex_lock_interruptible(&dev->lock))
    {
        return -ERESTARTSYS;
    }

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->circular_buffer, index)
    {
        if (entry->buffptr != NULL)
        {
            maxEntries++;
        }

        if (writeCmd >= maxEntries)
        {
            retval = -EINVAL;
            goto out;
        }

        index = 0;

        AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->circular_buffer, index)
        if (entry->buffptr != NULL)
        {
            if (cmdIndex == writeCmd)
            {
                if (writeCmdOffset > entry->size)
                {
                    retval = -EINVAL;
                    goto out;
                }

                f_pos += writeCmdOffset;
                filp->f_pos = f_pos;
                retval = 0;
                goto out;
            }

            f_pos += entry->size;
            cmdIndex++;
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
            struct aesd_seekto seekTo;

            if (copy_from_user(&seekTo, (const void __user *)arg, sizeof(seekTo)) != 0)
            {
                retval = -EFAULT;
            }
            else
            {
                retval = aesd_adjust_file_offset(filp, seekTo.write_cmd, seekTo.write_cmd_offset);
            }
            break;
        }
        default:
            return -EINVAL;
    }

    return retval;
}

struct file_operations aesd_fops = {
    .owner =            THIS_MODULE,
    .llseek =           aesd_llseek,
    .read =             aesd_read,
    .write =            aesd_write,
    .unlocked_ioctl =   aesd_ioctl,
    .open =             aesd_open,
    .release =          aesd_release,
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

    if (result) 
    {
        unregister_chrdev_region(dev, 1);
    }

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
        {
            kfree(entry->buffptr);
        }
    }

    if (aesd_device.partial_entry)
    {
        kfree(aesd_device.partial_entry);
    }

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
