/*
 * Author: Davide Rombolà aka rmbq <davide.rombola@gmail.com>
 *
 * sleepy_plug: cpu_hotplug driver based on intelli_plug by faux123
 *
 * Copyright 2012 Paul Reioux
 * Copyright 2014 Davide Rombolà
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/rq_stats.h>
#include <linux/slab.h>
#include <linux/input.h>

#ifdef CONFIG_POWERSUSPEND
#include <linux/powersuspend.h>
#endif

//#define DEBUG_SLEEPY_PLUG
#undef DEBUG_SLEEPY_PLUG

#define SLEEPY_PLUG_MAJOR_VERSION	2
#define SLEEPY_PLUG_MINOR_VERSION	3

#define DEF_SAMPLING_MS			(1000)
#define BUSY_SAMPLING_MS		(500)

#define PEAK_THRESHOLD			30

#define RQ_VALUES_ARRAY_DIM		5

static DEFINE_MUTEX(sleepy_plug_mutex);

struct delayed_work sleepy_plug_work;

static struct workqueue_struct *sleepy_plug_wq;

enum mp_decisions {
	DO_NOTHING = 0,
	ONE_CPU_UP = 1,
	TWO_CPU_UP = 2,
#if CONFIG_NR_CPUS == 4
	THREE_CPU_UP = 3,
	FOUR_CPU_UP = 4,
#endif
};

#if CONFIG_NR_CPUS == 2
static unsigned int up_thresholds[3] = {0, 11, UINT_MAX};
static unsigned int down_thresholds[3] = {0, 8, PEAK_THRESHOLD};
#else
static unsigned int up_thresholds[5] = {0, 11, 18, 27, UINT_MAX};
static unsigned int down_thresholds[5] = {0, 8, 15, 24, PEAK_THRESHOLD};
#endif

static unsigned int sleepy_plug_active = 1;

static unsigned int sampling_time = DEF_SAMPLING_MS;
static bool suspended = false;
static unsigned int rq_values[RQ_VALUES_ARRAY_DIM] = {PEAK_THRESHOLD};

static int calc_rq_avg(int last_rq_depth) {
	int i;
	int avg = 0;

	//shift all values by 1
	for(i = 0;i < RQ_VALUES_ARRAY_DIM-1;i++) {
		rq_values[i] = rq_values[i+1];
		avg += rq_values[i+1];
	}
	avg += last_rq_depth;
	rq_values[RQ_VALUES_ARRAY_DIM-1] = last_rq_depth;

	return avg/RQ_VALUES_ARRAY_DIM;
}

static enum mp_decisions mp_decision(void)
{
	int nr_cpu_online;
	int avg,i;
	enum mp_decisions decision = DO_NOTHING;

	avg = calc_rq_avg(rq_info.rq_avg);
	
	if(rq_info.rq_avg > PEAK_THRESHOLD) {
#if CONFIG_NR_CPUS == 2
		decision = TWO_CPU_UP;
#else
		decision = FOUR_CPU_UP;
#endif
	}
	else {
		nr_cpu_online = num_online_cpus();

		for(i = 1; i <= CONFIG_NR_CPUS; i++) {
			if(avg >= down_thresholds[i - 1] && avg <= up_thresholds[i] && avg <= down_thresholds[i]) {
				if(nr_cpu_online != i)
					decision = i;
				break;
			}
		}
	}
}
#ifdef DEBUG_SLEEPY_PLUG
	pr_info("[SLEEPY] nr_cpu_online: %d|avg: %d\n",nr_cpu_online,avg);
#endif
	return decision;
}

static void set_cpus(int n_cpus_on_requested) {
	int nr_cpus_online = num_online_cpus();

	if(n_cpus_on_requested == DO_NOTHING) return;
	
	if(nr_cpus_online < n_cpus_on_requested) {
		sampling_time = BUSY_SAMPLING_MS;
		for(;nr_cpus_online < n_cpus_on_requested;nr_cpus_online++)
			cpu_up(nr_cpus_online);
	}
	else{
		for(nr_cpus_online--;nr_cpus_online >= n_cpus_on_requested - 1;nr_cpus_online--)
			cpu_down(nr_cpus_online);
	}
}

static void __cpuinit sleepy_plug_work_fn(struct work_struct *work)
{
#ifdef DEBUG_SLEEPY_PLUG
	pr_info("decision: %d\n",decision);
#endif
	if (!suspended) {
		sampling_time = DEF_SAMPLING_MS;
		set_cpus(mp_decision());
	}
#ifdef DEBUG_SLEEPY_PLUG
	else
		pr_info("sleepy_plug is suspened!\n");
#endif
	queue_delayed_work_on(0, sleepy_plug_wq, &sleepy_plug_work,
		msecs_to_jiffies(sampling_time));
}

#ifdef CONFIG_POWERSUSPEND
static void sleepy_plug_suspend(struct power_suspend *h)
{
	int nr_cpu_online;

	flush_workqueue(sleepy_plug_wq);

	mutex_lock(&sleepy_plug_mutex);
	suspended = true;
	mutex_unlock(&sleepy_plug_mutex);

	nr_cpu_online = num_online_cpus() - 1;
	for(;nr_cpu_online > 0;nr_cpu_online--) {
		cpu_down(nr_cpu_online);
	}
}

static void __cpuinit sleepy_plug_resume(struct power_suspend *h)
{
	mutex_lock(&sleepy_plug_mutex);
	/* keep cores awake long enough for faster wake up */
	suspended = false;
	mutex_unlock(&sleepy_plug_mutex);

	cpu_up(1);

	queue_delayed_work_on(0, sleepy_plug_wq, &sleepy_plug_work,
		msecs_to_jiffies(10));
}

static struct power_suspend sleepy_plug_power_suspend_driver = {
	.suspend = sleepy_plug_suspend,
	.resume = sleepy_plug_resume,
};
#endif  /* CONFIG_POWERSUSPEND */

static void sleepy_plug_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
        queue_delayed_work_on(0, sleepy_plug_wq, &sleepy_plug_work,
                msecs_to_jiffies(10));
}

static int sleepy_plug_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "sleepyplug";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;
	pr_info("%s found and connected!\n", dev->name);
	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void sleepy_plug_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id sleepy_plug_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			 INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			    BIT_MASK(ABS_MT_POSITION_X) |
			    BIT_MASK(ABS_MT_POSITION_Y) },
	}, /* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			 INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			    BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	}, /* touchpad */
	{ },
};

static struct input_handler sleepy_plug_input_handler = {
	.event          = sleepy_plug_input_event,
	.connect        = sleepy_plug_input_connect,
	.disconnect     = sleepy_plug_input_disconnect,
	.name           = "sleepyplug_handler",
	.id_table       = sleepy_plug_ids,
};

/********************* SYSF START *********************/
static ssize_t sleepy_plug_active_show(struct device *dev, struct device_attribute *attr, char *buf) {
	size_t count = 0;

	count += sprintf(buf, "%d\n", sleepy_plug_active);

	return count;
}

static ssize_t sleepy_plug_active_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
	if (buf[0] >= '0' && buf[0] <= '1' && buf[1] == '\n')
		if (sleepy_plug_active != buf[0] - '0') {
			sleepy_plug_active = buf[0] - '0';

			if(sleepy_plug_active == 0) {
				int nr_cpu_online = CONFIG_NR_CPUS - 1;
				flush_workqueue(sleepy_plug_wq);
				for(;nr_cpu_online > 0;nr_cpu_online--)
					cpu_up(nr_cpu_online);
			} else {
				queue_delayed_work_on(0, sleepy_plug_wq, &sleepy_plug_work,
					msecs_to_jiffies(10));
			}
		}

	return count;
}
static DEVICE_ATTR(sleepyplug, (S_IWUSR|S_IRUGO),
	sleepy_plug_active_show, sleepy_plug_active_store);

static struct kobject *android_touchkey_kobj;

static int sleepyplug_sysfs_init(void) {
	int ret;

	android_touchkey_kobj = kobject_create_and_add("sleepy_plug", NULL);
	if (android_touchkey_kobj == NULL) {
		printk(KERN_ERR "%s: subsystem_register failed\n", __func__);
		ret = -ENOMEM;
		return ret;
	}
	ret = sysfs_create_file(android_touchkey_kobj, &dev_attr_sleepyplug.attr);
	if (ret) {
		printk(KERN_ERR "%s: sysfs_create_file failed\n", __func__);
		return ret;
	}

	return 0;
}
/********************* SYSF END ***********************/
int __init sleepy_plug_init(void)
{
	int rc;

	pr_info("sleepy_plug: version %d.%d by rmbq\n",
		 SLEEPY_PLUG_MAJOR_VERSION,
		 SLEEPY_PLUG_MINOR_VERSION);

	rc = input_register_handler(&sleepy_plug_input_handler);

	sleepy_plug_wq = alloc_workqueue("sleepyplug",
				WQ_HIGHPRI | WQ_UNBOUND, 1);

#ifdef CONFIG_POWERSUSPEND
	register_power_suspend(&sleepy_plug_power_suspend_driver);
#endif

	sleepyplug_sysfs_init();

	INIT_DELAYED_WORK(&sleepy_plug_work, sleepy_plug_work_fn);
	queue_delayed_work_on(0, sleepy_plug_wq, &sleepy_plug_work,
		msecs_to_jiffies(10));

	return 0;
}

MODULE_AUTHOR("Davide Rombolà <davide.rombola@gmail.com>");
MODULE_DESCRIPTION("'sleepy_plug' - An intelligent cpu hotplug driver for "
	"Low Latency Frequency Transition capable processors");
MODULE_LICENSE("GPL");

late_initcall(sleepy_plug_init);
