/*
 * LIRC base driver
 * 
 * (L) by Artur Lipowski <lipowski@comarch.pl>
 *        This code is licensed under GNU GPL
 *
 * $Id: lirc_dev.c,v 1.3 2000/04/11 13:52:38 columbus Exp $
 *
 */

#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 2, 4)
#error "!!! Sorry, this driver needs kernel version 2.2.4 or higher !!!"
#endif

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <asm/uaccess.h>
#include <asm/semaphore.h>
#include <linux/wrapper.h>
#define __KERNEL_SYSCALLS__
#include <linux/unistd.h>

#include "drivers/lirc.h"
#include "lirc_dev.h"

static int debug = 0;

MODULE_PARM(debug,"i");

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 3, 0)
#define DECLARE_MUTEX(foo)         	struct semaphore foo = MUTEX
#define DECLARE_MUTEX_LOCKED(foo)  	struct semaphore foo = MUTEX_LOCKED
#define init_MUTEX(foo)            	*(foo) = MUTEX
#define DECLARE_WAITQUEUE(name,task) 	struct wait_queue name = { task, NULL }
#endif

#define IRCTL_DEV_NAME    "BaseRemoteCtl"
#define IRLOCK            down_interruptible(&ir->lock)
#define IRUNLOCK          up(&ir->lock)
#define SUCCESS           0
#define FAIL              -1
#define NOPLUG            -1
#define dprintk           if (debug) printk

#define LOGHEAD           "lirc_dev (%s[%d]): "

struct irctl
{
	struct lirc_plugin p;
	int open;

	unsigned long features;
	unsigned int buf_len;
	int bytes_in_key;

	unsigned char buffer[BUFLEN];
	unsigned int in_buf;
	int head, tail;

	int tpid;
	struct semaphore *t_notify;
	int shutdown;
	long jiffies_to_wait;

	WAITQ wait_poll;

	struct semaphore lock;
};

DECLARE_MUTEX(plugin_lock);

static struct irctl irctls[MAX_IRCTL_DEVICES];

/*  helper function
 *  return 0 on success
 */
static inline void init_irctl(struct irctl *ir)
{
	memset(&ir->p, 0, sizeof(struct lirc_plugin));
	ir->p.minor = NOPLUG;

	ir->buf_len = 0;
	ir->bytes_in_key = 0;
	ir->features = 0;

	ir->tpid = -1;
	ir->t_notify = NULL;
	ir->shutdown = 0;
	ir->jiffies_to_wait = 0;

	memset(&ir->buffer, 0, BUFLEN);
	ir->in_buf = 0;
	ir->head = ir->tail = 0;
	ir->open = 0;
}

/*  helper function
 *  return 0 on success
 */

inline static int add_to_buf(struct irctl *ir)
{
	unsigned char buf[BUFLEN];
	unsigned int i;

	if (ir->in_buf == ir->buf_len) {
		dprintk(LOGHEAD "buffer overflow\n",
			ir->p.name, ir->p.minor);
		return FAIL;
	}

	for (i=0; i < ir->bytes_in_key; i++) {
		if (ir->p.get_key(ir->p.data, &buf[i], i)) {
			return FAIL;
		}
		dprintk(LOGHEAD "remote code (0x%x) now in buffer\n",
			ir->p.name, ir->p.minor, buf[i]);
	}

	/* here is the only point at which we add key codes to the buffer */
	IRLOCK;
	memcpy(&ir->buffer[ir->tail], buf, ir->bytes_in_key);
	ir->tail += ir->bytes_in_key;
	ir->tail %= ir->buf_len;
	ir->in_buf += ir->bytes_in_key;
	IRUNLOCK;

	return SUCCESS;
}

/* polling thread
 */
static int lirc_thread(void *irctl)
{
	struct irctl *ir = irctl;

#ifdef __SMP__
	lock_kernel();
#endif

	/* This thread doesn't need any user-level access,
	 * so get rid of all our resources
	 */
	exit_mm(current);
	exit_files(current);
	exit_fs(current);
	current->session = 1;
	current->pgrp = 1;
	current->euid = 0;
	current->tty = NULL;
	sigfillset(&current->blocked);

	strcpy(current->comm, "lirc_dev");

#ifdef __SMP__
	unlock_kernel();
#endif

	if(ir->t_notify != NULL) {
		up(ir->t_notify);
	}

	dprintk(LOGHEAD "poll thread started\n",
		ir->p.name, ir->p.minor);

	do {
		if (ir->open) {
			if (ir->jiffies_to_wait) {
				current->state = TASK_INTERRUPTIBLE;
				schedule_timeout(ir->jiffies_to_wait);
			} else {
				interruptible_sleep_on(ir->p.get_queue(ir->p.data));
			}
			if (signal_pending(current)) {
				break;
			}
			if (!add_to_buf(ir)) {
				wake_up_interruptible(&ir->wait_poll);
			}
		} else {
			/* if device not opened so we can sleep half a second */
			current->state = TASK_INTERRUPTIBLE;
			schedule_timeout(HZ/2);
		}
	} while (!ir->shutdown && !signal_pending(current));

	ir->tpid = -1;
	if(ir->t_notify != NULL) {
		up(ir->t_notify);
	}

	dprintk(LOGHEAD "poll thread ended\n",
		ir->p.name, ir->p.minor);

	return 0;
}

/*
 *
 */
int lirc_register_plugin(struct lirc_plugin *p)
{
	struct irctl *ir;
	int minor;
	DECLARE_MUTEX_LOCKED(tn);

	if (!p) {
		printk("lirc_dev: lirc_register_plugin:"
		       "plugin pointer must be not NULL!\n");
		return -FAIL;
	}

	if (MAX_IRCTL_DEVICES <= p->minor) {
		printk("lirc_dev: lirc_register_plugin:"
		       "\" minor\" must be beetween 0 and %d (%d)!\n",
		       MAX_IRCTL_DEVICES-1, p->minor);
		return FAIL;
	}

	if (1 > p->code_length || (BUFLEN*8) < p->code_length) {
		printk("lirc_dev: lirc_register_plugin:"
		       "code length in bits for minor (%d) "
		       "must be less than %d!\n",
		       p->minor, BUFLEN*8);
		return FAIL;
	}

	if (p->sample_rate) {
		if (2 > p->sample_rate || 50 < p->sample_rate) {
			printk("lirc_dev: lirc_register_plugin:"
			       "sample_rate must be beetween 2 and 50!\n");
			return FAIL;
		}
	} else {
		if (!p->get_queue) {
			printk("lirc_dev: lirc_register_plugin:"
			       "get_queue cannot be NULL!\n");
			return FAIL;
		}
	}

	down_interruptible(&plugin_lock);

	minor = p->minor;

	if (0 > minor) {
		/* find first free slot for plugin */
		for (minor=0; minor<MAX_IRCTL_DEVICES; minor++)
			if (irctls[minor].p.minor == NOPLUG)
				break;
		if (MAX_IRCTL_DEVICES == minor) {
			printk("lirc_dev: lirc_register_plugin: "
			       "no free slots for plugins!\n");
			up(&plugin_lock);
			return FAIL;
		}
	} else if (irctls[minor].p.minor != NOPLUG) {
		printk("lirc_dev: lirc_register_plugin:"
		       "minor (%d) just registerd!\n", minor);
		up(&plugin_lock);
		return FAIL;
	}

	ir = &irctls[minor];

	if (p->sample_rate) {
		ir->jiffies_to_wait = HZ / p->sample_rate;
	} else {
                /* it means - wait for externeal event in task queue */
		ir->jiffies_to_wait = 0;
	} 

	/* some safety check 8-)  */
	p->name[sizeof(p->name)-1] = '\0';

	ir->bytes_in_key = p->code_length/8 + (p->code_length%8 ? 1 : 0);
	
	/* this simplifies boundary checking during buffer access */
	ir->buf_len = BUFLEN - (BUFLEN%ir->bytes_in_key);

	ir->features = (p->code_length > 8) ? LIRC_CAN_REC_LIRCCODE : 
					      LIRC_CAN_REC_CODE;

	ir->p = *p;
	ir->p.minor = minor;

	/* try to fire up polling thread */
	ir->t_notify = &tn;
	ir->tpid = kernel_thread(lirc_thread, (void*)ir, 0);
	if (ir->tpid < 0) {
		IRUNLOCK;
		up(&plugin_lock);
		printk("lirc_dev: lirc_register_plugin:"
		       "cannot run poll thread for minor = %d\n", p->minor);
		return FAIL;
	}
	down(&tn);
	ir->t_notify = NULL;

	up(&plugin_lock);

	MOD_INC_USE_COUNT;

	dprintk("lirc_dev: plugin %s registered at minor number = %d\n",
		ir->p.name, ir->p.minor);

	return minor;
}

/*
 *
 */
int lirc_unregister_plugin(int minor)
{
	struct irctl *ir;
	DECLARE_MUTEX_LOCKED(tn);

	if (minor < 0 || minor >= MAX_IRCTL_DEVICES) {
		printk("lirc_dev: lirc_unregister_plugin:"
		       "\" minor\" must be beetween 0 and %d!\n",
		       MAX_IRCTL_DEVICES-1);
		return FAIL;
	}

	ir = &irctls[minor];

	down_interruptible(&plugin_lock);

	if (ir->p.minor != minor) {
		printk("lirc_dev: lirc_unregister_plugin:"
		       "minor (%d) device not registered!", minor);
		up(&plugin_lock);
		return FAIL;
	}

	if (ir->open) {
		printk("lirc_dev: lirc_unregister_plugin:"
		       "plugin %s[%d] in use!", ir->p.name, ir->p.minor);
		up(&plugin_lock);
		return FAIL;
	}

	/* end up polling thread */
	if (ir->tpid >= 0) 
	{
		ir->t_notify = &tn;
		ir->shutdown = 1;
		down(&tn);
		ir->t_notify = NULL;
	}

	dprintk("lirc_dev: plugin %s unregistered from minor number = %d\n",
		ir->p.name, ir->p.minor);

	init_irctl(ir);

	up(&plugin_lock);

	MOD_DEC_USE_COUNT;

	return SUCCESS;
}

/*
 *
 */
static int irctl_open(struct inode *inode, struct file *file)
{
	struct irctl *ir;
	
	if (MINOR(inode->i_rdev) >= MAX_IRCTL_DEVICES) {
		dprintk("lirc_dev [%d]: open result = -ENODEV\n",
			MINOR(inode->i_rdev));
		return -ENODEV;
	}

	ir = &irctls[MINOR(inode->i_rdev)];

	dprintk(LOGHEAD "open called\n", ir->p.name, ir->p.minor);

	down_interruptible(&plugin_lock);

	if (ir->p.minor == NOPLUG) {
		up(&plugin_lock);
		dprintk(LOGHEAD "open result = -ENODEV\n",
			ir->p.name, ir->p.minor);
		return -ENODEV;
	}

	if (ir->open) {
		up(&plugin_lock);
		dprintk(LOGHEAD "open result = -EBUSY\n",
			ir->p.name, ir->p.minor);
		return -EBUSY;
	}

	/* rhere is no need for locking here because ir->open is 0 
         * and lirc_thread isn't using buffer
         */
	ir->head = ir->tail;
	ir->in_buf = 0;

	++ir->open;
	ir->p.set_use_inc(ir->p.data);

	up(&plugin_lock);

	dprintk(LOGHEAD "open result = %d\n",
		ir->p.name, ir->p.minor, SUCCESS);

	return SUCCESS;
}

/*
 *
 */
static int irctl_close(struct inode *inode, struct file *file)
{
	struct irctl *ir = &irctls[MINOR(inode->i_rdev)];

	dprintk(LOGHEAD "close called\n",
		ir->p.name, ir->p.minor);
 
	down_interruptible(&plugin_lock);

	--ir->open;
	ir->p.set_use_dec(ir->p.data);

	up(&plugin_lock);

	return SUCCESS;
}

/*
 *
 */
static unsigned int irctl_poll(struct file *file, poll_table *wait)
{
	struct irctl *ir = &irctls[MINOR(file->f_dentry->d_inode->i_rdev)];

	dprintk(LOGHEAD "poll called\n",
		ir->p.name, ir->p.minor);

	if (!ir->in_buf) {
		poll_wait(file, &ir->wait_poll, wait);
	}

	dprintk(LOGHEAD "poll result = %s\n",
		ir->p.name, ir->p.minor, 
		ir->in_buf ? "POLLIN|POLLRDNORM" : "SUCCESS");

	return ir->in_buf ? (POLLIN|POLLRDNORM) : SUCCESS;
}

/*
 *
 */
static int irctl_ioctl(struct inode *inode, struct file *file,
                       unsigned int cmd, unsigned long arg)
{
	unsigned long mode;
	int result = SUCCESS;
	struct irctl *ir = &irctls[MINOR(inode->i_rdev)];

	dprintk(LOGHEAD "poll called (%u)\n",
		ir->p.name, ir->p.minor, cmd);

	if (ir->p.minor == NOPLUG) {
		dprintk(LOGHEAD "ioctl result = -ENODEV\n",
			ir->p.name, ir->p.minor);
		return -ENODEV;
	}

	switch(cmd)
	{
	case LIRC_GET_FEATURES:
		result = put_user(ir->features, (unsigned long*)arg);
		break;
	case LIRC_GET_REC_MODE:
		result = put_user(LIRC_REC2MODE(ir->features),
				  (unsigned long*)arg);
		break;
	case LIRC_SET_REC_MODE:
		result = get_user(mode, (unsigned long*)arg);
		if(!result && !(LIRC_MODE2REC(mode) & ir->features)) {
			result = -EINVAL;
		}
		break;
	case LIRC_GET_LENGTH:
		result = put_user((unsigned long)ir->p.code_length, 
				  (unsigned long *)arg);
		break;
	default:
		result = -ENOIOCTLCMD;
	}

	dprintk(LOGHEAD "ioctl result = %d\n",
		ir->p.name, ir->p.minor, result);

	return result;
}

/*
 *
 */
static ssize_t irctl_read(struct file *file,
			  char *buffer,   
			  size_t length, 
			  loff_t *ppos)     
{
	unsigned char buf[BUFLEN];
	struct irctl *ir = &irctls[MINOR(file->f_dentry->d_inode->i_rdev)];
	int ret;
	DECLARE_WAITQUEUE(wait, current);

	dprintk(LOGHEAD "read called\n", ir->p.name, ir->p.minor);

	if (ir->bytes_in_key != length) {
		dprintk(LOGHEAD "read result = -EIO\n", ir->p.name, ir->p.minor);
		return -EIO;
	}

	/* we add ourselves to the task queue before buffer check 
         * to avoid losing scan code (in case when queue is awaken somewhere 
	 * beetwen while condition checking and scheduling)
	 */
	add_wait_queue(&ir->wait_poll, &wait);
	current->state = TASK_INTERRUPTIBLE;

	/* while input buffer is empty and device opened in blocking mode, 
	 * wait for input 
	 */
	while (!ir->in_buf) {
		if (file->f_flags & O_NONBLOCK) {
			dprintk(LOGHEAD "read result = -EWOULDBLOCK\n", 
				ir->p.name, ir->p.minor);
			remove_wait_queue(&ir->wait_poll, &wait);
			current->state = TASK_RUNNING;
			return -EWOULDBLOCK;
		}
		if (signal_pending(current)) {
			dprintk(LOGHEAD "read result = -ERESTARTSYS\n", 
				ir->p.name, ir->p.minor);
			remove_wait_queue(&ir->wait_poll, &wait);
			current->state = TASK_RUNNING;
			return -ERESTARTSYS;
		}
		schedule();
		current->state = TASK_INTERRUPTIBLE;
	}

	remove_wait_queue(&ir->wait_poll, &wait);
	current->state = TASK_RUNNING;

	/* here is the only point at which we remove key codes from the buffer */
	IRLOCK;
	memcpy(buf, &ir->buffer[ir->head], length);
	ir->head += length;
	ir->head %= ir->buf_len;
	ir->in_buf -= length;
	IRUNLOCK;

	ret = copy_to_user(buffer, buf, length);

	dprintk(LOGHEAD "read result = %s (%d)\n",
		ir->p.name, ir->p.minor, ret ? "-EFAULT" : "OK", ret);

	return ret ? -EFAULT : length;
}


static struct file_operations fops = {
	NULL,   /* seek */
	irctl_read, 
	NULL,   /* write */
	NULL,   /* readdir */
	irctl_poll,
	irctl_ioctl,
	NULL,   /* mmap */
	irctl_open,
	NULL,   /* flush */
	irctl_close
};


EXPORT_SYMBOL(lirc_register_plugin);
EXPORT_SYMBOL(lirc_unregister_plugin);

/*
 *
 */
int lirc_dev_init(void)
{  	
	int i;

	for (i=0; i < MAX_IRCTL_DEVICES; ++i) {
		init_irctl(&irctls[i]);	
		init_MUTEX(&irctls[i].lock);
		init_waitqueue_head(&irctls[i].wait_poll);
	}

	i = module_register_chrdev(IRCTL_DEV_MAJOR, 
				   IRCTL_DEV_NAME,
				   &fops);
	
	if (i < 0) {
		printk ("lirc_dev: device registration failed with %d\n", i);
		return i;
	}
	
	printk("lirc_dev: IR Remote Control driver registered, at major %d \n", 
	       IRCTL_DEV_MAJOR);

	return SUCCESS;
}

/* ---------------------------------------------------------------------- */

/* For now dont try to use it as a static version !  */

#ifdef MODULE

MODULE_DESCRIPTION("LIRC base driver module");
MODULE_AUTHOR("Artur Lipowski");

/*
 *
 */
int init_module(void)
{
	return lirc_dev_init();
}

/*
 *
 */
void cleanup_module(void)
{
	int ret;
	
	ret = module_unregister_chrdev(IRCTL_DEV_MAJOR, IRCTL_DEV_NAME);
 
	if (0 > ret){
		printk("lirc_dev: error in module_unregister_chrdev: %d\n", ret);
	} else {
		dprintk("lirc_dev: module successfully unloaded\n");
	}
}
#endif

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-basic-offset: 8
 * End:
 */