

#define schedtune_cpu_boost_with(cpu, p) 0
#ifdef CONFIG_UCLAMP_TASK
#define schedtune_task_boost(tsk) uclamp_eff_value(p, UCLAMP_MIN) > 0
#else
#define schedtune_task_boost(tsk) 0
#endif

#ifdef CONFIG_UCLAMP_TASK_GROUP
#define schedtune_prefer_idle(tsk) uclamp_latency_sensitive(tsk)
#else
#define schedtune_prefer_idle(tsk) 0
#endif

#ifdef CONFIG_UCLAMP_TASK
#define schedtune_prefer_high_cap(tsk) uclamp_boosted(tsk)
#else
#define schedtune_prefer_high_cap(tsk) 0
#endif

#define schedtune_enqueue_task(task, cpu) do { } while (0)
#define schedtune_dequeue_task(task, cpu) do { } while (0)

