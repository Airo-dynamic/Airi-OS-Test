#include <airi_libc.h> // 用户只通过int80进入存储服务。
static const char message[] = "airios: durable request"; // 固定可复现记录，payload不包含NUL。
int main(int argc, char **argv) { // 同一ELF通过明确名字选择写入或只读恢复。
    if (argc != 1) // 当前自有启动只传一个名字。
        return 60; // 不猜测参数布局。
    long count = airi_log_count(); // 当前启动从设备恢复的已确认前缀。
    if (count < 0) { // 缺设备或不确定日志不当成空盘。
        airi_puts("[STORAGE_UNAVAILABLE]\n"); // 说明普通应用失败，内核仍可继续。
        return 61; // 应用退出值不等于内核PANIC。
    }
    if (airi_log_append((const void *)0xffffffff80000000ull, 1) != -14 || airi_log_append(message, 481) != -22 || airi_log_read(0, (void *)message, 1) != -14 || airi_log_count() != count) // 坏指针和长度必须在任何写入或读取副作用之前拒绝。
        return 65; // 原前缀数量不能因错误请求改变。
    airi_puts("[STORAGE_USER_ERRORS_OK] bad_pointer=-14 length=-22 prefix=unchanged\n"); // 真实用户边界结果。
    if (!memcmp(argv[0], "storage", 8)) { // 显式追加一次，普通启动不会自动写正式日志。
        long sequence = airi_log_append(message, sizeof(message) - 1); // 复制字节后等待数据与提交同步。
        if (sequence == -28) { // 有界容量满，不覆盖旧记录。
            airi_puts("[STORAGE_FULL] records=64\n"); // 供用户决定后续处理。
            return 28; // 普通应用错误可从监控器继续。
        }
        if (sequence != count + 1) // 成功必须承认正确新记录身份。
            return 62; // 不假装短写或设备错误已完成。
        ++count; // 本程序接下来读取刚确认的最后一条。
    } else if (memcmp(argv[0], "recover", 8)) // 只读名字来自内核初始程序目录。
        return 63; // 不接受隐式其他模式。
    if (!count) { // 干净盘尚无记录。
        airi_puts("[STORAGE_EMPTY]\n"); // 空是合法状态，恢复演示另要求已写记录。
        return 0; // 不误读索引-1。
    }
    char buffer[64]; // 用户输出缓冲比当前payload长，内核仍按长度复制。
    long bytes = airi_log_read((unsigned)(count - 1), buffer, sizeof(buffer)); // 从实际设备重新验证最后记录。
    if (bytes != sizeof(message) - 1 || memcmp(buffer, message, (size_t)bytes)) // 不只看缓存计数。
        return 64; // 旧映像或损坏数据不能通过。
    airi_puts(!memcmp(argv[0], "storage", 8) ? "[STORAGE_USER_OK] append=committed read=verified\n" : "[RECOVERY_USER_OK] last_record=verified\n"); // 标记来自真正ring3用户代码。
    return 0; // 同步设备请求结束后正常退出。
}
