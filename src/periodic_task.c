
/*
 *  Periodic_task.c
 *  
 *  Usage:
 *  
 *  During _arch_ init procedure, call `periodic_task_setup()`.
 *  In the HW IRQ handler, invoke the `do_global_timer()`
 *  
 *  
 */


#include <linux/module.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/hardirq.h>
#include <linux/smp.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>

#include <asm/localtimer.h>
#include <asm/mach-types.h>
#include <asm/sched_clock.h>
#include <asm/smp_twd.h>
#include <asm/hardware/gic.h>
#include <asm/uaccess.h>

#include <mach/hardware.h>
#include <mach/periodic_task.h>


enum {
	kPROC_ACTIVE_CPU,
	kPROC_PERIOD_MS,
	kPROC_COUNT
};

struct periodic_sched {
	char name[PERIODIC_NAME_LEN + 1];
	void (*action)(unsigned long);
	unsigned long data;
	unsigned long period;

	unsigned long expires;
	unsigned long count;
	unsigned int cpu;
	unsigned long tick;

	struct proc_dir_entry *proc;
	struct proc_dir_entry *proc_i[kPROC_COUNT];
	char proc_name[PERIODIC_NAME_LEN + 10 + 1];

	struct list_head list;
};

#define periodic_head(cpu) (&periodic_queue[cpu].list)

static unsigned int global_timer_irqs[NR_CPUS];
static struct periodic_sched periodic_queue[NR_CPUS];
static DEFINE_SPINLOCK(periodic_task_lock);
static struct proc_dir_entry *periodic_proc;

static void *periodic_task_get_id(struct periodic_sched *task)
{
	return (void *)(~(unsigned long)task);
}

static struct periodic_sched *periodic_task_get_task(void *id)
{
	return (struct periodic_sched *)(~(unsigned long)id);
}

static int periodic_proc_read_cpu(char *page, char **start, off_t off,
                                  int count, int *eof, void *data)
{
	struct periodic_sched *q = (struct periodic_sched *)data;
	int len = 0;

	if (off > 0) {
		return 0;
	}

	len += sprintf(page + len, "%d\n", q->cpu);

	*start = NULL;

	if (count <= len) {
		*eof = 1;
	}

	return len;
}

static int periodic_proc_write_cpu(struct file *file, const char __user *buffer,
                                   unsigned long count, void *data)
{
	struct periodic_sched *q = (struct periodic_sched *)data;
	unsigned int cpu;
	unsigned long flags;

	char *tmp = kmalloc(count + 1, GFP_KERNEL);

	if (copy_from_user(tmp, buffer, count)) {
		printk(KERN_ERR "copy failed: 0x%08lx\n", (unsigned long)buffer);
		kfree(tmp);
		return 0;
	}

	tmp[count] = '\0';

	cpu = simple_strtoul(tmp, NULL, 10);

	if ((cpu != q->cpu) && (cpu < NR_CPUS)) {
		spin_lock_irqsave(&periodic_task_lock, flags);

		list_del(&q->list);
		list_add_tail(&q->list, periodic_head(cpu));

		q->cpu = cpu;

		spin_unlock_irqrestore(&periodic_task_lock, flags);
	}

	kfree(tmp);

	return count;
}

static int periodic_proc_read_period(char *page, char **start, off_t off,
                                     int count, int *eof, void *data)
{
	struct periodic_sched *q = (struct periodic_sched *)data;
	int len = 0;

	if (off > 0) {
		return 0;
	}

	len += sprintf(page + len, "%lu\n", q->period);

	*start = NULL;

	if (count <= len) {
		*eof = 1;
	}

	return len;
}

static int periodic_proc_write_period(struct file *file, const char __user *buffer,
                                      unsigned long count, void *data)
{
	struct periodic_sched *q = (struct periodic_sched *)data;
	unsigned long period;
	unsigned long flags;

	char *tmp = kmalloc(count + 1, GFP_KERNEL);

	if (copy_from_user(tmp, buffer, count)) {
		printk(KERN_ERR "copy failed: 0x%08lx\n", (unsigned long)buffer);
		kfree(tmp);
		return 0;
	}

	tmp[count] = '\0';

	period = simple_strtoul(tmp, NULL, 10);

	if ((period != q->period) && (period != 0)) {
		spin_lock_irqsave(&periodic_task_lock, flags);

		q->period = period;

		spin_unlock_irqrestore(&periodic_task_lock, flags);
	}

	kfree(tmp);

	return count;
}

static int create_periodic_proc(struct periodic_sched *q)
{
	int ret = 0;

	q->proc_i[kPROC_ACTIVE_CPU] = create_proc_entry("active_cpu", 0600, q->proc);

	if (!q->proc_i[kPROC_ACTIVE_CPU]) {
		ret = -1;
	} else {
		q->proc_i[kPROC_ACTIVE_CPU]->read_proc = periodic_proc_read_cpu;
		q->proc_i[kPROC_ACTIVE_CPU]->write_proc = periodic_proc_write_cpu;
		q->proc_i[kPROC_ACTIVE_CPU]->data = q;
	}

	q->proc_i[kPROC_PERIOD_MS] = create_proc_entry("period_ms", 0600, q->proc);

	if (!q->proc_i[kPROC_PERIOD_MS]) {
		ret = -1;
	} else {
		q->proc_i[kPROC_PERIOD_MS]->read_proc = periodic_proc_read_period;
		q->proc_i[kPROC_PERIOD_MS]->write_proc = periodic_proc_write_period;
		q->proc_i[kPROC_PERIOD_MS]->data = q;
	}

	return ret;
}

static int remove_periodic_proc(struct periodic_sched *q)
{
	int ret = 0;

	remove_proc_entry("period_ms", q->proc);
	remove_proc_entry("active_cpu", q->proc);
	remove_proc_entry(q->proc_name, periodic_proc);

	return ret;
}

static int periodic_task_proc(char *page, char **start, off_t off,
                              int count, int *eof, void *data)
{
	struct periodic_sched *q;
	unsigned int cpu;
	int len = 0;

	if (off > 0) {
		/* FIXME: too many clients may force to change this */
		return 0;
	}

	len += sprintf(page + len, "ID:      CPU:     count    period      tick name\n");

	for_each_possible_cpu(cpu) {
		list_for_each_entry(q, periodic_head(cpu), list) {
			len += sprintf(page + len, "%p  %2d: %9lu %9lu %9lu %s\n",
			               periodic_task_get_id(q),
			               cpu, q->count, q->period, q->tick, q->name);
		}
	}

	*start = NULL;

	if (count <= len) {
		*eof = 1;
	}

	return len;
}

int periodic_task_setup(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		memset(&periodic_queue[cpu], 0, sizeof(struct periodic_sched));
		INIT_LIST_HEAD(periodic_head(cpu));
	}

	periodic_proc = proc_mkdir("periodic_task", NULL);
	create_proc_read_entry("list", 0, periodic_proc, periodic_task_proc, (void *)periodic_queue);

	return 0;
}
EXPORT_SYMBOL(periodic_task_setup);

void *periodic_task_add(unsigned int cpu,
                        unsigned long period, /* ms */
                        void (*action)(unsigned long),
                        unsigned long data,
                        const char *name)
{
	struct periodic_sched *q = NULL;
	unsigned long flags;

	if (unlikely((cpu >= NR_CPUS) || (action == NULL) || (name == NULL))) {
		printk(KERN_ERR "null context: failed to add task on cpu%d\n", cpu);
		return NULL;
	}

	if (period == 0) {
		printk(KERN_ERR "null context: period == 0 is not supported\n");
		return NULL;
	}

	q = (struct periodic_sched *)kzalloc(sizeof(*q), GFP_KERNEL);

	if (unlikely(q == NULL)) {
		printk(KERN_ERR "no memory: failed to add task on cpu%d\n", cpu);
		return NULL;
	}

	q->cpu = cpu;
	q->action = action;
	q->data = data;
	q->period = period;

	strncpy(q->name, name, PERIODIC_NAME_LEN);
	strncpy(q->proc_name, name, PERIODIC_NAME_LEN);
	snprintf(q->proc_name + strlen(q->proc_name), 10, "-%08lx", (unsigned long)periodic_task_get_id(q));

	spin_lock_irqsave(&periodic_task_lock, flags);
	list_add_tail(&q->list, periodic_head(cpu));
	spin_unlock_irqrestore(&periodic_task_lock, flags);

	q->proc = proc_mkdir(q->proc_name, periodic_proc);

	if (!q->proc) {
		printk(KERN_ERR "Failed to create proc dir for task %p\n", periodic_task_get_id(q));
	} else if (create_periodic_proc(q)) {
		printk(KERN_ERR "Failed to create proc entry for task %p\n", periodic_task_get_id(q));
	}

	printk(KERN_INFO "Periodic task `%s' has been added on cpu%d. ID: %p\n",
	       q->name, cpu, periodic_task_get_id(q));

	return periodic_task_get_id(q);
}
EXPORT_SYMBOL(periodic_task_add);

int periodic_task_remove(void *task)
{
	struct periodic_sched *q = NULL;
	struct periodic_sched *t = periodic_task_get_task(task);
	unsigned long flags;
	unsigned int cpu;
	int ret = -1;

	if (unlikely(task == NULL)) {
		return 0;
	}

	spin_lock_irqsave(&periodic_task_lock, flags);

	for_each_possible_cpu(cpu) {
		list_for_each_entry(q, periodic_head(cpu), list) {
			if (q == t) {
				list_del(&q->list);
				remove_periodic_proc(q);
				kfree(q);
				q = NULL;
				ret = 0;
				break;
			}
		}
	}

	spin_unlock_irqrestore(&periodic_task_lock, flags);

	if (!ret) {
		printk(KERN_INFO "Periodic task 0x%lx has been removed", (unsigned long)task);
	}

	return ret;
}
EXPORT_SYMBOL(periodic_task_remove);

int periodic_task_remove_all(unsigned int cpu)
{
	struct periodic_sched *q, *next;
	unsigned long flags;

	spin_lock_irqsave(&periodic_task_lock, flags);

	list_for_each_entry_safe(q, next, periodic_head(cpu), list) {
		list_del(&q->list);
		kfree(q);
	}

	spin_unlock_irqrestore(&periodic_task_lock, flags);

	return 0;
}
EXPORT_SYMBOL(periodic_task_remove_all);

unsigned long periodic_task_get_period(void *task)
{
	struct periodic_sched *q = NULL;
	struct periodic_sched *t = periodic_task_get_task(task);
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		list_for_each_entry(q, periodic_head(cpu), list) {
			if (q == t) {
				return q->period;
			}
		}
	}

	return 0;
}
EXPORT_SYMBOL(periodic_task_get_period);

unsigned long periodic_task_get_cpu(void *task)
{
	struct periodic_sched *q = NULL;
	struct periodic_sched *t = periodic_task_get_task(task);
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		list_for_each_entry(q, periodic_head(cpu), list) {
			if (q == t) {
				return cpu;
			}
		}
	}

	return 0;
}
EXPORT_SYMBOL(periodic_task_get_cpu);

int periodic_task_get_name(void *task, char *name)
{
	struct periodic_sched *q = NULL;
	struct periodic_sched *t = periodic_task_get_task(task);
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		list_for_each_entry(q, periodic_head(cpu), list) {
			if (q == t) {
				strncpy(name, q->name, PERIODIC_NAME_LEN);
				return 0;
			}
		}
	}

	return -1;
}
EXPORT_SYMBOL(periodic_task_get_name);

static int global_timer_ack(void)
{
	//read arch specified timer status, please refer to the SoC docs.
	return 0;
}

#define POLL_INCREMENT (1)		/* 1ms */

static void ppi_timer(unsigned int cpu)
{
	struct periodic_sched *q = NULL;
	if(!periodic_head(cpu)){
		return ;
	}
	list_for_each_entry(q, periodic_head(cpu), list) {
		q->expires += POLL_INCREMENT;

		if (q->expires >= q->period) {
			unsigned long t = get_tick();
			q->count++;
			q->action(q->data);
			q->tick = get_tick() - t;
			q->expires = 0;
		}
	}
}

asmlinkage void __exception_irq_entry do_global_timer(int ppinr, struct pt_regs *regs)
{
	struct pt_regs *old_regs = set_irq_regs(regs);
	unsigned int cpu = smp_processor_id();
	unsigned long flags;

	if (global_timer_ack()) {
		global_timer_irqs[cpu]++;
		irq_enter();
		local_irq_save(flags);
		ppi_timer(cpu);			/* in_irq() == in_interrupt() == true */
		local_irq_restore(flags);
		irq_exit();
	}

	set_irq_regs(old_regs);
}

void show_global_irqs(struct seq_file *p, int prec)
{
	unsigned int cpu;

	seq_printf(p, "%*s: ", prec, "GBL");

	for_each_present_cpu(cpu)
		seq_printf(p, "%10u ", global_timer_irqs[cpu]);

	seq_printf(p, " Global timer interrupts\n");
}

/** @brief This function is IPI handler and called in linux interrupt mode
	    to process global timer callbacks on this(local) core
    @param cpu - the current cpu id (smp_processor_id())
*/
void ipi_gtcore_sync(int cpu)
{
    unsigned long flags;
    global_timer_irqs[cpu]++;
    local_irq_save(flags);
    ppi_timer(cpu);			/* in_irq() == in_interrupt() == true */
    local_irq_restore(flags);
}

void global_timer_send_sync(void)
{
    // IPI call
    // this function sends notification to other ARM core
    // to process global timer callbacks on that core
    // it's designed to be called directly by driver
    // to be able to implement more flexible logic
    // and to minimize the number of syncronizations
    // 
    smp_send_gtcore_sync(smp_processor_id() ? 0 : 1);
}
EXPORT_SYMBOL(global_timer_send_sync);

/** @brief This function is designed to be called in Linux interrupt mode
	    by some specific driver to start global timer callbacks processing
	    on this(local) ARM core
*/
void global_timer_proc (void)
{
    unsigned long flags;
    unsigned int cpu = smp_processor_id();

    global_timer_irqs[cpu]++;
    local_irq_save(flags);
    ppi_timer(cpu);			/* in_irq() == in_interrupt() == true */
    local_irq_restore(flags);
}
EXPORT_SYMBOL(global_timer_proc);
