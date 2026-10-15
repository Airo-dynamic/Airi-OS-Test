#ifndef AIRI_CORE_INTERNAL_H  // 防止桌面内部接口重复包含
#define AIRI_CORE_INTERNAL_H  // 标记桌面内部接口已经包含
#include "ui.h"  // 引入前端服务合同与桌面状态
#include <airi/base.h>  // 引入自有字节操作和有界格式化函数
static inline void copy(char *out, size_t n, const char *text) {  // 按调用者数组容量复制NUL文字
    airi_snprintf(out, n, "%s", text);  // 按调用者容量复制NUL文字；n为0时不写内存，过长文字截到n-1字节
}
#endif  // 结束当前条件编译范围
