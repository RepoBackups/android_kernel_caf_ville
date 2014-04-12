/*
 * Author: Davide Rombolà aka rmbq <davide.rombola@gmail.com>
 *
 * sleepy_plug: cpu_hotplug driver based on sleepy_plug by faux123
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

#define SLEEPY_PLUG_MAJOR_VERSION	1
#define SLEEPY_PLUG_MINOR_VERSION	5

#define DEF_SAMPLING_MS			(1000)
#define BUSY_SAMPLING_MS		(500)

#define DOWN_THRESHOLD			8
#define UP_THRESHOLD			11
#define PEAK_THRESHOLD			30

#define RQ_VALUE_ARRAY_DIM		5

static DEFINE_MUTEX(sleepy_plug_mutex);

struct delayed_work sleepy_plug_work;

static struct workqueue_struct *sleepy_plug_wq;

enum mp_decisions {
	DO_NOTHING,
	CPU_UP,
	CPU_DOWN
};

static unsigned int sleepy_plug_active = 1;
module_param(sleepy_plug_active, uint, 0644);

static unsigned int sampling_time = 0;
static bool suspended = false;
static unsigned int rq_values[RQ_VALUE_ARRAY_DIM] = {6};

static int calc_rq_avg(int last_rq_depth) {
	int i;
	int avg = 0;

	//shift all values by 1
	for(i = 0;i < RQ_VALUE_ARRAY_DIM-1;i++) {
		rq_values[i] = rq_values[i+1];
		avg += rq_values[i+1];
	}
	avg += last_rq_depth;
	rq_values[RQ_VALUE_ARRAY_DIM-1] = last_rq_depth;

	return avg/RQ_VALUE_ARRAY_DIM;
}

static enum mp_decisions mp_decision(void)
{
	int nr_cpu_online;
	int avg,i;
	enum mp_decisions decision = DO_NOTHING;

	if(rq_info.rq_avg > PEAK_THRESHOLD) {
		for(i = 0;i < RQ_VALUE_ARRAY_DIM-1;i++) 
			rq_values[i] = rq_values[i+1];
		rq_values[RQ_VALUE_ARRAY_DIM-1] = rq_info.rq_avg;

		avg = rq_info.rq_avg;
	}
	else
		avg = calc_rq_avg(rq_info.rq_avg);
	nr_cpu_online = num_online_cpus();

	if(nr_cpu_online == 1 && avg >= UP_THRESHOLD)
		decision = CPU_UP;
	else if(nr_cpu_online == 2 && avg < DOWN_THRESHOLD)
		decision = CPU_DOWN;

#ifdef DEBUG_SLEEPY_PLUG
	pr_info("[SLEEPY] nr_cpu_online: %d|avg: %d|max: %d|new? %d\n",nr_cpu_online,crav.avg,crav.max,crav.max_is_new == true?1:0);
#endif
	return decision;
}

static void __cpuinit sleepy_plug_work_fn(struct work_struct *work)
{
	enum mp_decisions decision = DO_NOTHING;

	if (sleepy_plug_active == 1) {
		// detect artificial loads or constant loads
		// using msm rqstats

		decision = mp_decision();
#ifdef DEBUG_SLEEPY_PLUG
		pr_info("decision: %d\n",decision);
#endif
		if (!suspended) {
			if (decision == CPU_UP) {
				cpu_up(1);
				sampling_time = BUSY_SAMPLING_MS;
			} else if(decision == CPU_DOWN){
				cpu_down(1);
				sampling_time = DEF_SAMPLING_MS;
			} else if(decision == DO_NOTHING)
				sampling_time = DEF_SAMPLING_MS;
		}
#ifdef DEBUG_SLEEPY_PLUG
		else
			pr_info("sleepy_plug is suspened!\n");
#endif
	}
	queue_delayed_work_on(0, sleepy_plug_wq, &sleepy_plug_work,
		msecs_to_jiffies(sampling_time));
}

#ifdef CONFIG_POWERSUSPEND
static void sleepy_plug_suspend(struct power_suspend *h)
{
	flush_workqueue(sleepy_plug_wq);

	mutex_lock(&sleepy_plug_mutex);
	suspended = true;
	mutex_unlock(&sleepy_plug_mutex);

	cpu_down(1);
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

static int input_dev_filter(const char *input_dev_name)
{
	if (strstr(input_dev_name, "touchscreen") ||
		strstr(input_dev_name, "sec_touchscreen") ||
		strstr(input_dev_name, "touch_dev") ||
		strstr(input_dev_name, "atmel_mxt224e") ||
		strstr(input_dev_name, "-keypad") ||
		strstr(input_dev_name, "-nav") ||
		strstr(input_dev_name, "-oj")) {
		pr_info("touch dev: %s\n", input_dev_name);
		return 0;
	} else {
		pr_info("touch dev: %s\n", input_dev_name);
		return 1;
	}
}

static int sleepy_plug_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	if (input_dev_filter(dev->name))
		return -ENODEV;

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
	{ .driver_info = 1 },
	{ },
};

static struct input_handler sleepy_plug_input_handler = {
	.event          = sleepy_plug_input_event,
	.connect        = sleepy_plug_input_connect,
	.disconnect     = sleepy_plug_input_disconnect,
	.name           = "sleepyplug_handler",
	.id_table       = sleepy_plug_ids,
};

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

