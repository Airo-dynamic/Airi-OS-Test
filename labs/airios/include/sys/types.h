#ifndef AIRI_SYS_TYPES_H // 本用户运行库的类型，不读取宿主sys/types.h。
#define AIRI_SYS_TYPES_H // 只声明一次。
#include <stddef.h> // size_t来自目标编译器的freestanding头。
#include <stdint.h> // 固定宽度整数仍由目标工具链定义。
typedef long ssize_t; // 当前x86-64有符号字节数，负数用于错误。
typedef int64_t off_t; // 当前文件定位使用有符号64位偏移。
typedef long pid_t; // 能容纳内核完整32位正PID，不把槽号当身份。
typedef unsigned mode_t; // open的源码参数类型；当前RAM文件没有权限位模型。
#endif // 结束包含保护。
