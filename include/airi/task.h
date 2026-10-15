#ifndef AIRI_TASK_H // 教学任务接口只声明一次。
#define AIRI_TASK_H // 包含保护。
void airi_task_yield(void); // 当前内核任务主动交接CPU，尚不是时钟抢占。
void airi_task_demo(void); // 两个独立栈任务轮流执行并回收其映射。
#endif // 结束包含保护。
