#include<linux/vmalloc.h>
#include<linux/list.h>
#include<linux/fs.h>
#include<linux/major.h>
#include<linux/blkdev.h>
#include<linux/cdev.h>
#include<linux/moduleparam.h>
#include<linux/kfifo.h>
#include<linux/uaccess.h>
#include<linux/slab.h>
#include<linux/types.h>
#include<asm/io.h>
#include<linux/kdev_t.h>
#include<linux/wait.h>
#include<linux/errno.h>
#include<asm/irq.h>
#include<linux/interrupt.h>
#include<linux/module.h>
#include<linux/kernel.h>
#include<linux/version.h>
#include<linux/init.h>
#include<linux/device.h>
#include<linux/pci.h>
#include<linux/ioport.h>
#include<asm/unistd.h>
#include<linux/slab.h>
#include<linux/fs.h>
#include<linux/types.h>
#include<asm/uaccess.h>
#include<asm/io.h>
#include<linux/kdev_t.h>
#include<asm/fcntl.h>
#include<linux/sched.h>
#include<linux/wait.h>
#include<linux/errno.h>
#include<linux/kfifo.h>
#include<asm/irq.h>
#include<asm/errno.h>
#include<asm/ioctl.h>
#include<linux/string.h>
#include<linux/interrupt.h>
#include<linux/cdev.h>
#include <linux/vmalloc.h>
#include <linux/list.h>
#include <linux/major.h>
#include <linux/blkdev.h>

#define HW_FIFO_SIZE 16  
#define KMEM_BUFSIZE (1024*1024)

char lk_buff_wr[16];
char lk_buff_rd[16];
static int my_dev_open(struct inode *inode, struct file *file);
static int my_dev_release(struct inode *inode, struct file *file);
static ssize_t my_dev_read(struct file *file, char __user *buf, size_t count, loff_t *pposs);
static ssize_t my_dev_write(struct file *file, char __user * buf , size_t count, loff_t *ppos);
static int  __init custom_serial_init(void);
static void  __exit custom_serial_exit(void);

irqreturn_t custom_int_handler(int irq_no, void *dev);

int kfifo_siz;

typedef struct serial_dev{

	struct list_head list;
	struct cdev cdev;
	dev_t dev1;
	int base_addr;
	int irq_no;
	int kfifo_size;
	struct kfifo write_kfifo;
	struct kfifo read_kfifo;
	spinlock_t spinlock1;
	spinlock_t spinlock2;
	wait_queue_head_t read_queue;
	wait_queue_head_t write_queue;
	struct tasklet_struct tx;
	struct tasklet_struct rx;
}SERIAL_DEV;


static struct file_operations serial_file_ops={

	.open = my_dev_open,
	.read = my_dev_read,
	.write = my_dev_write,
	.release= my_dev_release,
	.owner = THIS_MODULE,
};


static int my_dev_open(struct inode *inode, struct file *file){

	SERIAL_DEV *dev;

	dev = container_of(inode->i_cdev,SERIAL_DEV,cdev);
	file->private_data=dev;
	printk("THIS IS OPEN METHOD");	
	
	outb(0x80, dev->base_addr +3);
	outb(0x00, dev->base_addr +1);
	outb(0x0c, dev->base_addr +0);
	outb(0x03, dev->base_addr +3);
	
	outb(0x03, dev->base_addr +1);
	outb(0xc7, dev->base_addr +2);

	if(request_irq(dev->irq_no,custom_int_handler,IRQF_DISABLED|IRQF_SHARED,"custom_serial_dev",dev))
	{
		outb(0x00, dev->base_addr +1);
		printk("Resourse Unavailable");
		return -EBUSY;	
	}
	
		outb(0x18,dev->base_addr + 4);
		return 0;
}


static int my_dev_release(struct inode *inode, struct file *file){


	SERIAL_DEV *dev;
	
	dev=file->private_data;
	
	outb(0x00, dev->base_addr + 4);
	outb(0x00,dev->base_addr+1);
		
	if((inb(dev->base_addr+0x02)&0x01))
	{
		free_irq(dev->irq_no,dev);
	}
		
	printk("THIS IS THE RELEASE METHOD\n");
	
	return 0;

}

static ssize_t my_dev_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	SERIAL_DEV *dev;
	dev= file->private_data;

	int bytes;
	bytes=kfifo_len(&dev->read_kfifo);

	if(bytes==0)
	{
		printk("Inside Read byte==0\n");
		if(file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		else
			wait_event_interruptible(dev->read_queue,(kfifo_len(&dev->read_kfifo)!=0));
	}

	printk("THIS IS THE READ METHOD");

	if(access_ok(VERIFY_WRITE,(void __user*)buf,count))
	{
		bytes= kfifo_out(&dev->read_kfifo,(void*)buf,count);
		return bytes;
	}
	else
		return -EFAULT;
}

void tasklet_rx(unsigned long private_obj)
{
	int j =0;
	printk("THIS IS THE Receive Tasklet METHOD\n");
	SERIAL_DEV *dev = (SERIAL_DEV *) private_obj;
	for(j=0;;j++)  
	{
		if(inb(dev->base_addr+0x05)&0x01)
		{
			lk_buff_rd[j] = inb(dev->base_addr+0x0);
		}
		else
			break;

	}
	kfifo_in(&dev->read_kfifo,(char*)lk_buff_rd,sizeof(int));
	wake_up_interruptible (&(dev->read_queue));
}
void tasklet_tx(unsigned long private_obj)
{
	int ret_val,i;
	SERIAL_DEV *dev = (SERIAL_DEV *) private_obj;
	ret_val = kfifo_out(&dev->write_kfifo,(char*)lk_buff_wr,HW_FIFO_SIZE);
	printk("THIS IS THE Tansmit Tasklet METHOD\n");

	if(ret_val!=0)
	{
		for(i=0;i<ret_val; i++)
			outb(lk_buff_wr[i],dev->base_addr+0x0);
	}
	wake_up_interruptible(&dev->write_queue);
}


irqreturn_t custom_int_handler(int irq_no, void *dev)
{
	printk("THIS IS THE Interrup METHOD\n");
	irqreturn_t irq_flag=0;
	irq_flag |= IRQ_NONE;
	SERIAL_DEV *dev1=(SERIAL_DEV*) dev;
	if(inb(dev1->base_addr + 0x05)&0x20)
	{
		tasklet_schedule(&dev1->tx);
		irq_flag |= IRQ_HANDLED;
	}
	
	if(inb(dev1->base_addr +0x05)&0x01)
	{
		tasklet_schedule(&dev1->rx);
		irq_flag |= IRQ_HANDLED;
	}
	return irq_flag;
}
	
static ssize_t my_dev_write(struct file *file, char __user *buf , size_t count,loff_t *ppos)
{
	SERIAL_DEV *dev = file->private_data;
	int bytes;
	printk("THIS IS THE WRITE METHODE");
	bytes = kfifo_len(&dev->write_kfifo);
	bytes = kfifo_siz - bytes; 	
	if (bytes==0)
	{
		printk("Inside Write bytes==0\n");
		if(file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		else 
			wait_event_interruptible(dev->write_queue,(kfifo_len(&dev->write_kfifo)!=0));
	}


	if(access_ok(VERIFY_WRITE,(void __user*)buf,count))
	{
		bytes= kfifo_in(&dev->read_kfifo,(void*)buf,count);
		return bytes;
	}
	else return -EFAULT;
}
	

struct resource *no1;
SERIAL_DEV *my_dev;
static dev_t start;
int base_addr,irq_no;


struct class *pseudo_class;
module_param(base_addr,int,S_IRUGO);
module_param(irq_no,int,S_IRUGO);

static int __init custom_serial_init(void)
{

	int ret,ret1,ret2;
	printk("THIS IS INIT METHODE\n");
	my_dev = kmalloc(sizeof(SERIAL_DEV),GFP_KERNEL);
	kfifo_siz = 64;
	no1= request_region(base_addr,8,"custom_serial_device");

	if(no1==NULL)
	{
	kfree(my_dev);
	return -EBUSY;
	}
	
	//list_add_tail(&my_dev->list,&dev_list);

	ret1 = kfifo_alloc(&my_dev->write_kfifo, KMEM_BUFSIZE , GFP_KERNEL);
	if(ret)
	{
	kfree(&my_dev);
	release_region(base_addr,8);
	}	

	ret2 = kfifo_alloc(&my_dev->read_kfifo, KMEM_BUFSIZE , GFP_KERNEL);
	
	if(ret)
	{
	kfree(&my_dev);
	release_region(base_addr,8);
	}	
	init_waitqueue_head(&my_dev->write_queue);
    	init_waitqueue_head(&my_dev->read_queue);

	tasklet_init(&my_dev->tx,tasklet_tx,&my_dev);
	tasklet_init(&my_dev->rx,tasklet_rx,&my_dev);

	alloc_chrdev_region(&start,0,1,"custom_serial_driver");
	cdev_init(&my_dev->cdev,&serial_file_ops);

	my_dev->cdev.ops = &serial_file_ops;

	my_dev->base_addr= base_addr;
	my_dev->irq_no = irq_no;

	if(cdev_add(&my_dev->cdev,start,1))
	{
	unregister_chrdev_region(start,1);
	kfree(my_dev);
	return -EBUSY;
	}

	//pseudo_class = class_create(THIS_MODULE,"custom_serial_class");
	//device_create(&pseudo_class,NULL,start,NULL,"custom_serial_dev");
	return 0;
}


static void __exit custom_serial_exit(void){

	//device_destroy(pseudo_class,&start);
	//class_destroy(pseudo_class);
	printk("THIS IS EXIT METHODE");

	cdev_del(&my_dev->cdev);
	unregister_chrdev_region(start,1);
	kfree(my_dev);
	release_region(base_addr,8);
}

MODULE_DESCRIPTION("Demonstrate kernel memory allocation");
module_init(custom_serial_init);
module_exit(custom_serial_exit);
MODULE_LICENSE("GPL");
	

