#ifndef AIRI_PTHREAD_INTERNAL_H // 自有运行库内部协议，应用不得直接操纵引用。
#define AIRI_PTHREAD_INTERNAL_H // 包含保护。
#include <pthread.h> // 当前对象布局，不兼容其他系统pthread ABI。
int __airi_pthread_mutex_wait_ref(pthread_mutex_t *mutex); // 持锁登记，随后才可释放并等待，正错误不改errno。
int __airi_pthread_mutex_wait_unref(pthread_mutex_t *mutex); // 同一次操作重取该锁以后配对归还。
#endif // 这两个入口不承诺标准pthread API。
