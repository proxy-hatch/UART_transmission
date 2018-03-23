#define DEBUG

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/types.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/kfifo.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/cdev.h>
#include <linux/irqreturn.h>
#include <linux/device.h>
#include <linux/seq_file.h> /* seq_read, seq_lseek, single_release */


#undef pr_fmt
#define pr_fmt(fmt)                "%s.c:%d %s " fmt, KBUILD_MODNAME, __LINE__, __func__

/* Register defines from Xilinx xuartns550_l.h */
/* base offset for UART registers */
#define UART_REG_OFFSET 0x1000

#define UART_RBR_OFFSET (UART_REG_OFFSET) /**< Receive buffer, read only */
#define UART_THR_OFFSET (UART_REG_OFFSET) /**< Transmit holding register */
#define UART_IER_OFFSET (UART_REG_OFFSET + 0x04) /**< Interrupt enable */
#define UART_IIR_OFFSET (UART_REG_OFFSET + 0x08) /**< Interrupt id, read only */
#define UART_FCR_OFFSET (UART_REG_OFFSET + 0x08) /**< Fifo control, write only */
#define UART_LCR_OFFSET (UART_REG_OFFSET + 0x0C) /**< Line Control Register */
#define UART_MCR_OFFSET (UART_REG_OFFSET + 0x10) /**< Modem Control Register */
#define UART_LSR_OFFSET (UART_REG_OFFSET + 0x14) /**< Line Status Register */
#define UART_MSR_OFFSET (UART_REG_OFFSET + 0x18) /**< Modem Status Regsiter */
#define UART_DRLS_OFFSET (UART_REG_OFFSET + 0x00) /**< Divisor Register LSB */
#define UART_DRLM_OFFSET (UART_REG_OFFSET + 0x04) /**< Divisor Register MSB */

/*
 * The following constant specifies the size of the FIFOs, the size of the
 * FIFOs includes the transmitter and receiver such that it is the total number
 * of bytes that the UART can buffer
 */
#define FIFO_SIZE                 128


#define UART_IER_MODEM_STATUS        0x00000008 /**< Modem status interrupt */
#define UART_IER_RX_LINE                0x00000004 /**< Receive status interrupt */
#define UART_IER_TX_EMPTY                0x00000002 /**< Transmitter empty interrupt */
#define UART_IER_RX_DATA                0x00000001 /**< Receiver data available */

#define UART_IIR_INTID2                         0x0000000E /**< Only the interrupt ID */
#define UART_IIR_INTPEND                                 0x00000001 /**< Interrupt not pending */
#define UART_IIR_FIFOEN         0x000000C0 /**< Only the FIFOs enable */

/* Interrupt IDs */
#define UART_INTID_RX_LINE                        0x3 /**< Receiver Linet */
#define UART_INTID_RX_DATA                         0x2 /**< Received Data Available */
#define UART_INTID_CHAR_TO                        0x6 /**< Character Timeout */
#define UART_INTID_TX_EMPTY                         0x1 /**< Transmitter Holding Register Empty */
#define UART_INTID_MODEM_STATUS         0x0 /**< Modem Status */

#define UART_FIFO_RX_TRIG_MSB        0x00000080 /**< Trigger level MSB */
#define UART_FIFO_RX_TRIG_LSB        0x00000040 /**< Trigger level LSB */
#define UART_FIFO_TX_RESET                0x00000004 /**< Reset the transmit FIFO */
#define UART_FIFO_RX_RESET                0x00000002 /**< Reset the receive FIFO */
#define UART_FIFO_ENABLE                0x00000001 /**< Enable the FIFOs */
#define UART_FIFO_RX_TRIGGER        0x000000C0 /**< Both trigger level bits */

#define UART_LCR_DLAB                        0x00000080 /**< Divisor latch access */
#define UART_LCR_SET_BREAK                0x00000040 /**< Cause a break condition */
#define UART_LCR_STICK_PARITY        0x00000020 /**< Stick Parity */
#define UART_LCR_EVEN_PARITY        0x00000010 /**< 1 = even, 0 = odd parity */
#define UART_LCR_ENABLE_PARITY        0x00000008 /**< 1 = Enable, 0 = Disable parity*/
#define UART_LCR_2_STOP_BITS        0x00000004 /**< 1= 2 stop bits,0 = 1 stop bit */
#define UART_LCR_8_DATA_BITS        0x00000003 /**< 8 Data bits selection */
#define UART_LCR_7_DATA_BITS        0x00000002 /**< 7 Data bits selection */
#define UART_LCR_6_DATA_BITS        0x00000001 /**< 6 Data bits selection */
#define UART_LCR_LENGTH_MASK        0x00000003 /**< Both length bits mask */
#define UART_LCR_PARITY_MASK        0x00000018 /**< Both parity bits mask */

#define UART_MCR_LOOP                0x00000010 /**< Local loopback */
#define UART_MCR_OUT_2                0x00000008 /**< General output 2 signal */
#define UART_MCR_OUT_1                0x00000004 /**< General output 1 signal */
#define UART_MCR_RTS                0x00000002 /**< RTS signal */
#define UART_MCR_DTR                0x00000001 /**< DTR signal */

#define UART_LSR_RX_FIFO_ERROR                0x00000080 /**< An errored byte is in FIFO */
#define UART_LSR_TX_EMPTY                        0x00000040 /**< Transmitter is empty */
#define UART_LSR_TX_BUFFER_EMPTY         0x00000020 /**< Transmit holding reg empty */
#define UART_LSR_BREAK_INT                        0x00000010 /**< Break detected interrupt */
#define UART_LSR_FRAMING_ERROR                0x00000008 /**< Framing error on current byte */
#define UART_LSR_PARITY_ERROR                0x00000004 /**< Parity error on current byte */
#define UART_LSR_OVERRUN_ERROR                0x00000002 /**< Overrun error on receive FIFO */
#define UART_LSR_DATA_READY                        0x00000001 /**< Receive data ready */
#define UART_LSR_ERROR_BREAK                0x0000001E /**< Errors except FIFO error and
                                                break detected */

#define UART_DIVISOR_BYTE_MASK        0x000000FF

static void init_uart(void);

/**
 * struct uart_data - the driver data
 * @in_fifo:        input queue for write
 * @out_fifo:        output queue for read
 * @fifo_lock:        lock for queues
 * @readable:        waitqueue for blocking read
 * @writeable:        waitqueue for blocking write
 *
 * Can be retrieved from platform_device with
 * struct misc_loop_drv_data *drvdata = platform_get_drvdata(pdev);
 */

struct uart_data {
        struct mutex read_lock;
        struct mutex write_lock;
        DECLARE_KFIFO(in_fifo, char, FIFO_SIZE);
        DECLARE_KFIFO(out_fifo, char, FIFO_SIZE);
        wait_queue_head_t readable, writeable;
        struct tasklet_struct uart_tasklet;
        void __iomem *base_addr;
        unsigned long mem_start;
        unsigned long mem_end;
        unsigned int baud;
        unsigned int irq;
        unsigned int clk_freq;
};

static struct uart_data *uart;


int insmodirq;
int insmodaddr;
int major = 188;
#define NAME "tty_USB_351"
static struct cdev mycdev;
static struct class *myclass = NULL;

/**
 * uart_received - puts data to receive queue
 * @data: received data
 */
static void uart_received(char data)
{
    // put data into the in_fifo
    kfifo_in(&uart->in_fifo, &data,
                    sizeof(data));
    wake_up_interruptible(&uart->readable);     //waitqueue for blocking read
}

/**
 * uart_send - sends data to HW
 * @data: data to send
 */
static void uart_send(char data)
{
    // ***** Finish me by adding stuff here!
	// Write data to the hardware register
	iowrite32(data, uart->base_addr + UART_THR_OFFSET);
	wake_up_interruptible(&uart->writeable);
}

// returns a truth value indicating whether the UART is ready to transmit another byte of data
static inline u32 tx_ready(void)
{
    // ***** Finish me by adding stuff here!
	return ioread32(uart->base_addr + UART_LSR_OFFSET) & UART_LSR_TX_BUFFER_EMPTY;
}

// returns a truth value indicating whether or not data is ready to be received from the UART
static inline u32 rx_ready(void)
{
    // check if ioread32 returned LSB = 1
    // Line status register
    return ioread32(uart->base_addr + UART_LSR_OFFSET) & UART_LSR_DATA_READY;
}

// used in tasklet_init(&uart->uart_tasklet, uart_tasklet_func, (unsigned long)uart);
static void uart_tasklet_func(unsigned long d) {
        struct uart_data *uart = (void *)d;
	
	// ***** Finish me by adding stuff here!
	if (tx_ready() && !kfifo_is_empty(&uart->out_fifo) ) {
		char data;
		while (tx_ready()) {
			kfifo_out(&uart->out_fifo, &data, sizeof(data));
			uart_send(data);
		}
	}

        printk(KERN_ALERT "end of %s\n", __func__);
}

static int isr_counter;

// perform interrupt service routine for read operation from fifo
static irqreturn_t uart_isr(int irq, void *d)
{
        u32 intid;
        struct uart_data *uart = (void *)d;

        isr_counter++;
        // read the intID in UART, and check if its INTID2
        intid = (ioread32(uart->base_addr + UART_IIR_OFFSET) & UART_IIR_INTID2) >> 1;
        printk(KERN_ALERT "%s: intid=0x%X\n", __func__, intid);

        if (rx_ready()) {
                while (rx_ready()) {
                        char data_in = ioread32(uart->base_addr + UART_RBR_OFFSET);     // read the content in Read Buffer Register
                        printk(KERN_ALERT "%s: data_in=%d %c\n", __func__, data_in, data_in >= 32 ? data_in : ' ');
                        uart_received(data_in);
                }
        } else {
                tasklet_schedule(&uart->uart_tasklet);
        }
        return IRQ_HANDLED;
}

static int uart_open(struct inode *inode, struct file *file) {
        printk(KERN_ALERT "%s: from %s\n", __func__, current->comm);
        return 0;
}

static int uart_release(struct inode *inode, struct file *file) {
        printk(KERN_ALERT "%s: from %s\n", __func__, current->comm);
        return 0;
}

static ssize_t uart_read(struct file *file, char __user *buf,
                size_t count, loff_t *ppos)
{
        int ret = 0;
        unsigned int copied;

        printk(KERN_ALERT "%s: from %s\n", __func__, current->comm);

        if (mutex_lock_interruptible(&uart->read_lock)) {
                return -EINTR;
        }

        printk(KERN_ALERT "%s: from %s\n", __func__, current->comm);

        if (kfifo_is_empty(&uart->in_fifo)) {
                if (file->f_flags & O_NONBLOCK) {
                    	mutex_unlock(&uart->read_lock);
                        return -EAGAIN;
                } else {
                        printk(KERN_ALERT "%s: waiting\n", __func__);
            			ret = wait_event_interruptible(uart->readable,
            					!kfifo_is_empty(&uart->in_fifo));
                        printk(KERN_ALERT "%s: done waiting\n", __func__);
                        if (ret == -ERESTARTSYS) {
                                printk(KERN_ALERT "%s:interrupted\n", __func__);
                                mutex_unlock(&uart->read_lock);
                                return -EINTR;
                        }
                }
        }

        ret = kfifo_to_user(&uart->in_fifo, buf, count, &copied);

        mutex_unlock(&uart->read_lock);

        return ret ? ret : copied;
}

static ssize_t uart_write(struct file *file, const char __user *buf,
                size_t count, loff_t *ppos)
{
        int ret;
        unsigned int copied;

        printk(KERN_ALERT "%s: from %s\n", __func__, current->comm);

        if (mutex_lock_interruptible(&uart->write_lock)) {
                return -EINTR;
        }

        printk(KERN_ALERT "%s: from %s\n", __func__, current->comm);
        if (kfifo_is_full(&uart->out_fifo)) {
                if (file->f_flags & O_NONBLOCK) {
                		mutex_unlock(&uart->write_lock);
                        return -EAGAIN;
                } else {
        			ret = wait_event_interruptible(uart->writeable,
        					!kfifo_is_full(&uart->out_fifo));
                        if (ret == -ERESTARTSYS) {
                                printk(KERN_ALERT "%s:interrupted\n", __func__);
                                mutex_unlock(&uart->write_lock);
                                return -EINTR;
                        }
                }
        }

        ret = kfifo_from_user(&uart->out_fifo, buf, count, &copied);

        tasklet_schedule(&uart->uart_tasklet);

        mutex_unlock(&uart->write_lock);

        return ret ? ret : copied;
}

static unsigned int uart_poll(struct file *file, poll_table *pt)
{
        unsigned int mask = 0;
        poll_wait(file, &uart->readable, pt);
        poll_wait(file, &uart->writeable, pt);

        if (!kfifo_is_empty(&uart->in_fifo))
                mask |= POLLIN | POLLRDNORM;
        mask |= POLLOUT | POLLWRNORM;
/*
        if case of output end of file set
        mask |= POLLHUP;
        in case of output error set
        mask |= POLLERR;
*/
        return mask;
}

static struct file_operations uart_fops = {
    .owner = THIS_MODULE,
    .open = uart_open,
    .release = uart_release,
    .read = uart_read,
    .write = uart_write,
    .poll = uart_poll,
};

static struct miscdevice uart_dev = {
        .minor = MISC_DYNAMIC_MINOR,
        .name = KBUILD_MODNAME,
        .fops = &uart_fops,
};

/*
 * Initialization and cleanup section
 */

static void uart_cleanup(void)
{
        if (uart_dev.this_device)
                misc_deregister(&uart_dev);
        if (uart->irq)
                free_irq(uart->irq, uart);
        tasklet_kill(&uart->uart_tasklet);

        printk(KERN_ALERT "%s: isr_counter=%d\n", __func__, isr_counter);

        if (uart->mem_start && uart->mem_end)
                release_mem_region(uart->mem_start, uart->mem_end - uart->mem_start+1);
        kfree(uart);
}


static struct uart_data *uart_data_init(void)
{
        struct uart_data *uart;

        uart = kzalloc(sizeof(*uart), GFP_KERNEL);
        if (!uart)
                return NULL;
        init_waitqueue_head(&uart->readable);
        init_waitqueue_head(&uart->writeable);
        INIT_KFIFO(uart->in_fifo);
        INIT_KFIFO(uart->out_fifo);
        mutex_init(&uart->read_lock);
        mutex_init(&uart->write_lock);
        // the tasklet is scheduled two places in the code with a function called tasklet_schedule
        // One use of a tasklet is to allow one to defer execution from the immediate servicing of a particular interrupt,
        // for example to allow other interrupts to get time on the processor.
        // To simplify thinking, you can think of scheduling a tasklet to be similar to simply calling a tasklet.
        // The difference is that with scheduling a tasklet the call might not happen immediately,
        // like would be the case with a simple function call to the tasklet function.
        tasklet_init(&uart->uart_tasklet,
                        uart_tasklet_func, (unsigned long)uart);

        uart->mem_start = insmodaddr;
        uart->mem_end = insmodaddr + 0xffff;
        uart->clk_freq = 100000000;
        uart->irq = insmodirq;

        return uart;
}

static int uart_init(void)
{
		int device_created;
        int ret = 0;

        printk(KERN_ALERT "%s: MODNAME=%s\n", __func__, KBUILD_MODNAME);

        uart = uart_data_init();
        if (!uart) {
                printk(KERN_ALERT "%s:uart_data_init failed\n", __func__);
                goto exit;
        }

        /* Get iospace for the device */
        if (!request_mem_region(uart->mem_start, uart->mem_end - uart->mem_start + 1, KBUILD_MODNAME)) {
                printk(KERN_ALERT "%s:request_mem_region failed for %p\n", __func__, (void *)uart->mem_start);
                return -EBUSY;
        }

        uart->base_addr = ioremap(uart->mem_start, uart->mem_end - uart->mem_start + 1);
        if (!uart->base_addr) {
                printk(KERN_ALERT "%s:ioremap failed\n", __func__);
                return -EIO;
        }

        isr_counter = 0;
        /* Register misc device */
        ret = misc_register(&uart_dev);
        if (ret < 0) {
                printk(KERN_ALERT "%s:misc_register failed\n", __func__);
                goto exit;
        }
        printk(KERN_ALERT "%s: uart_dev.minor=%d\n", __func__, uart_dev.minor);
        ret = request_irq(uart->irq, (void *)uart_isr, 0, KBUILD_MODNAME, uart);
        if (ret < 0) {
                printk(KERN_ALERT "%s:request_irq failed\n", __func__);
                return ret;
        }

        init_uart();

        device_created = 0;
                 printk(KERN_ALERT "%s:Creating device file\n", __func__);

            /* cat /proc/devices */
            if (alloc_chrdev_region(&major, 0, 1, NAME "_proc") < 0) {
                    printk(KERN_ALERT "%s:allocate region failed\n", __func__);
                goto error;
            }
            /* ls /sys/class */
            if ((myclass = class_create(THIS_MODULE, NAME "_sys")) == NULL) {
                        printk(KERN_ALERT "%s:create class failed\n", __func__);
                goto error;
            }
            /* ls /dev/ */
            if (device_create(myclass, NULL, major, NULL, NAME) == NULL) {
                        printk(KERN_ALERT "%s:device create failed\n", __func__);
                goto error;
            }
            device_created = 1;
            cdev_init(&mycdev, &uart_fops);
            if (cdev_add(&mycdev, major, 1) == -1) {
                    printk(KERN_ALERT "%s:cdev add failed\n", __func__);
                goto error;
            }
            return 0;
        error:
exit:
        printk(KERN_ALERT "%s: ret=%d\n", __func__, ret);
        if (ret < 0)
                uart_cleanup();
        return ret;
}

// Helper Functions -------

static void set_baud_rate(int baud_rate)
{
        unsigned int lcr = ioread32((uart->base_addr) + UART_LCR_OFFSET);

        /* Configure the Line Control Register to allow access to the Divisor Latch */
        iowrite32(UART_LCR_DLAB, ((uart->base_addr) + UART_LCR_OFFSET));

        /* Calculate baud rate based off the bus frequency */
        uart->baud = uart->clk_freq / (16 * baud_rate);

        /* Write lower and upper bytes */
        iowrite32(uart->baud, ((uart->base_addr) + UART_DRLS_OFFSET));
        iowrite32(uart->baud >> 8, ((uart->base_addr) + UART_DRLM_OFFSET));

        /* Restore line control register */
        iowrite32(lcr, ((uart->base_addr) + UART_LCR_OFFSET));
}

static void init_uart(void)
{
        /* Disable interrupts */
        iowrite32(0x0, ((uart->base_addr) + UART_IER_OFFSET));

        /* Set UART to 300 bps (gross) */
        set_baud_rate(300);

        /* Set Line Control Register 8-N-1, No Parity */
        iowrite32(UART_LCR_8_DATA_BITS, ((uart->base_addr) + UART_LCR_OFFSET));

        /* Initialize FIFOs */
        iowrite32(UART_FIFO_ENABLE | UART_FIFO_RX_RESET | UART_FIFO_TX_RESET | UART_FIFO_RX_TRIGGER,
                        ((uart->base_addr) + UART_FCR_OFFSET));

        /* Enable RX and TX interrupts */
        iowrite32(UART_IER_RX_DATA | UART_IER_TX_EMPTY, ((uart->base_addr) + UART_IER_OFFSET));
}

// ignore Eclipse Code-Analysis error messages regarding syntax errors below.

module_init(uart_init);
module_exit(uart_cleanup);

module_param(insmodirq, int, 0);
module_param(insmodaddr, int, 0);
MODULE_PARM_DESC(insmodirq, "A long integer");
MODULE_PARM_DESC(insmodaddr, "An integer");

MODULE_DESCRIPTION("zedboard axi uart driver");
MODULE_AUTHOR("Graham Holland, Karol Swietlicki, Craig Scratchley, Eric Matthews et al.");
MODULE_LICENSE("GPL");
