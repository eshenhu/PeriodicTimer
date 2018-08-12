/**
 * 
 */
#ifndef _PERIODIC_TASK_H_
#define _PERIODIC_TASK_H_

#include <linux/seq_file.h>

#define PERIODIC_NAME_LEN (15)

extern void show_global_irqs(struct seq_file *, int);
extern int periodic_task_setup(void);
extern void *periodic_task_add(unsigned int cpu,
                               unsigned long period, /* ms */
                               void (*action)(unsigned long),
                               unsigned long data,
                               const char *name);
extern int periodic_task_remove(void *task);
extern int periodic_task_remove_all(unsigned int cpu);
extern unsigned long periodic_task_get_period(void *task);
extern unsigned long periodic_task_get_cpu(void *task);
extern int periodic_task_get_name(void *task, char *name);
extern void ipi_gtcore_sync(int cpu);

#endif	/* _PERIODIC_TASK_H_ */

