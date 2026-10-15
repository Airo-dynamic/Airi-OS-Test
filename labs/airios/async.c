#include <airi_libc.h> // 真正ring3用户通过自有ABI注册、提交、等待与退出。
static const uint8_t forbidden[32]; // 只读ELF区，完整CQ长度不允许内核写入。
static long submit(unsigned operation, uint64_t handle, uint64_t sector, uint64_t cookie) { // 本节小包装让调用点显示请求身份。
    struct airi_io_submission submission = {sector, cookie}; // 用户栈结构在本次调用里存在。
    return airi_io_submit(operation, handle, &submission); // 内核返回前已复制，不在设备完成前借这个局部地址。
}
static int await_started(void) { // 生命周期演示尽量在真实ACTIVE或已形成CQ后执行退出/exec。
    uint64_t deadline = airi_ticks() + 300; // 有上界，不让演示永久等状态。
    struct airi_io_state state; // 快照只包含当前轮次数值。
    do { // 设备可能很快，completed也说明请求已被服务。
        if (airi_io_state_get(&state)) // 状态错误不当作尚未开始。
            return 0; // 调用者以错误退出。
        if (state.active || state.completed) // 不强迫所有时序都观察到同一在途瞬间。
            return 1; // 关闭日志会记录真实阶段。
        airi_yield(); // 让其他用户运行，PIT会安排普通内核服务。
    } while (airi_ticks() < deadline); // 截止按真实tick。
    return 0; // 源码或设备无法推进，不能假装成功。
}
static int after_exec(void) { // 同一PID的新映像，不继承注册buffer或旧CQ。
    struct airi_io_state state; // 包含本PID旧epoch尚在收集的ACTIVE计数。
    struct airi_io_completion completion; // CQ只属于新epoch。
    if (airi_io_state_get(&state) || state.registered_buffers || state.outstanding || state.completed || airi_io_next(&completion, 0) != -11) // 旧资源不能被新程序收集。
        return 140; // 父wait可见错误。
    long buffer = airi_io_alloc(); // 可能复用旧buffer槽，但新handle与epoch独立。
    if (buffer < 0 || airi_io_put((uint64_t)buffer, "new-image", 9)) // 新数据不能被晚完成覆盖。
        return 141; // 不使用无效句柄。
    uint64_t deadline = airi_ticks() + 300; // 旧后端最迟按两秒驱动截止结束。
    do { // 旧请求若已完成，retired_active初始即可为0。
        if (airi_io_state_get(&state)) // 不能忽略状态错误。
            return 142; // 明确失败。
        if (!state.retired_active) // 同PID旧epoch的设备借用都已结束。
            break; // 无需固定睡一段时间猜结果。
        airi_yield(); // 内核继续pump并按旧epoch丢弃。
    } while (airi_ticks() < deadline); // 不无限等待。
    char data[9]; // 用户自己的输出数组。
    if (state.retired_active || airi_io_next(&completion, 0) != -11 || airi_io_get((uint64_t)buffer, data, sizeof(data)) || memcmp(data, "new-image", 9) || airi_io_free((uint64_t)buffer)) // 新CQ和新数据都不受旧完成影响。
        return 143; // 不是只看exec返回位置。
    airi_puts("[ASYNC_EXEC_RESET_OK] old_epoch=retired new_buffer=unchanged cq=empty\n"); // 是否实际DROP由内核轨迹另记。
    return 0; // 仍以父等待的同一PID结束。
}
int main(int argc, char **argv) { // 正常工作负载与exec目标共用ELF，明确按名字分支。
    if (argc != 1) // 当前自有启动参数固定。
        return 120; // 不读取不存在参数。
    size_t name_bytes = strlen(argv[0]); // 先找NUL，不能对短名字固定比较更长字符串跨越栈页。
    if (name_bytes == 11 && !memcmp(argv[0], "async-reset", 11)) // 新映像入口，静态数据已重新初始化。
        return after_exec(); // 旧C调用链不能回来。
    if (name_bytes != 5 || memcmp(argv[0], "async", 5)) // 只接受明确的正常名字。
        return 121; // 不猜测其他模式。
    long writer = airi_io_alloc(), reader = airi_io_alloc(), error_buffer = airi_io_alloc(), spare = airi_io_alloc(); // 当前owner最多四个注册对象。
    if (writer < 0 || reader < 0 || error_buffer < 0 || spare < 0 || airi_io_alloc() != -28) // 容量错误不能偷偷借其他owner预算。
        return 122; // 尚未提交，exit统一回收。
    uint8_t pattern[512], output[512]; // 用户指针从不直接进入DMA描述符。
    for (unsigned i = 0; i < 512; ++i) // 每个位置可辨认实际往返。
        pattern[i] = (uint8_t)(i * 17u + 3u); // 固定全扇区模式。
    if (airi_io_free((uint64_t)spare) || airi_io_get((uint64_t)spare, output, 1) != -9 || airi_io_put((uint64_t)writer, pattern, 512)) // 释放句柄不再可用，IDLE输入可复制。
        return 123; // 不把句柄当地址解引用。
    if (airi_io_submit(AIRI_IO_READ, (uint64_t)reader, (const struct airi_io_submission *)0xffffffff80000000ull) != -14) // 坏提交地址在接受前拒绝，不锁buffer。
        return 124; // 正常reader仍可提交。
    long child = airi_fork(); // 子有空异步上下文，用户栈里的handle整数仍复制。
    if (child < 0) // 明确进程资源错误。
        return 125; // 不等待不存在孩子。
    if (!child) { // 证明fork不自动赋予父注册buffer的所有权。
        if (airi_io_get((uint64_t)writer, output, 1) != -9) // 子PID无法使用父句柄。
            return 126; // 不能跨进程读内核buffer。
        airi_puts("[ASYNC_OTHER_TASK] inherited_handle=-9 cpu=running\n"); // 可在父CQ等待期间观察另一用户执行。
        uint64_t start = airi_ticks(), until = start + 10; // 约100毫秒CPU工作，时间来自PIT。
        unsigned reported = 0; // 中途记录一次实际取得CPU的进展。
        while (airi_ticks() < until) { // 这是对照工作负载，不是父亲循环重试CQ。
            if (!reported && airi_ticks() - start >= 5) { // 约50毫秒后仍在执行另一个用户任务。
                airi_puts("[ASYNC_OTHER_PROGRESS] cpu=continued\n"); // 节流测试将核对此时父亲仍在同一次CQ等待。
                reported = 1; // 不用大量日志制造人为延迟。
            }
            __asm__ volatile("pause"); // 后端推进由周期内核服务完成。
        }
        return 0; // 父之后wait回收。
    }
    if (submit(AIRI_IO_WRITE, (uint64_t)writer, 256, 30) || submit(AIRI_IO_FLUSH, 0, 0, 10) || submit(AIRI_IO_READ, (uint64_t)reader, 256, 20) || submit(AIRI_IO_READ, (uint64_t)error_buffer, 16, 40)) // 接受四条，最后一条由CQ报告范围错误。
        return 127; // 提交成功不当作I/O成功。
    if (submit(AIRI_IO_FLUSH, 0, 0, 50) != -11 || airi_io_free((uint64_t)writer) != -16 || airi_io_put((uint64_t)writer, pattern, 1) != -16 || airi_io_get((uint64_t)reader, output, 1) != -16) // 未收集CQ就仍占信用和BUSY。
        return 128; // 即使设备已快完成，也不可提前复用。
    struct airi_io_state state; // 只读观察，不消费CQ。
    if (airi_io_next((struct airi_io_completion *)forbidden, 0) != -14 || airi_io_state_get(&state) || state.outstanding != 4) // 坏CQ输出必须在pop之前拒绝。
        return 129; // 不能丢一条完成或提前归还信用。
    struct airi_io_completion completion; // 每次收集实际32字节结果。
    unsigned seen = 0; // 按cookie辨认，不依赖返回位置。
    long write_result = -999, read_result = -999, flush_result = -999; // 设备受限时按实际错误分支。
    for (unsigned i = 0; i < 4; ++i) { // 每条已接受请求都必须可收集。
        if (airi_io_next(&completion, 1) || completion.reserved) // 空CQ在同一次调用里阻塞，返回0表示已复制一条。
            return 130; // I/O字节/错误在记录内部。
        if (completion.cookie == 30 && completion.buffer == (uint64_t)writer) { // 明确写请求身份。
            write_result = (long)completion.result; seen |= 1; // 保存实际结果，不从submit0推断。
        } else if (completion.cookie == 10 && !completion.buffer) { // FLUSH不含数据buffer。
            flush_result = (long)completion.result; seen |= 2; // 同步结果可以是0或具体设备错误。
        } else if (completion.cookie == 20 && completion.buffer == (uint64_t)reader) { // 读结果只写reader。
            read_result = (long)completion.result; seen |= 4; // 收集后reader才IDLE。
        } else if (completion.cookie == 40 && completion.buffer == (uint64_t)error_buffer && completion.result == -22) // 正式日志扇区被有界接口拒绝。
            seen |= 8; // 错误完成仍结束buffer借用。
        else // 不匹配或重复身份不能算完整结果集合。
            return 131; // 避免按第i个结果猜第i个请求。
    }
    if (seen != 15 || airi_io_next(&completion, 0) != -11) // 无丢失也无额外结果。
        return 132; // 空CQ错误与I/O错误分别处理。
    if (write_result == 512) { // 正常可写设备必须实际读回模式并完成同步。
        if (read_result != 512 || flush_result || airi_io_get((uint64_t)reader, output, 512) || memcmp(output, pattern, 512)) // 不只核对完成数量。
            return 133; // 设备内容必须相符。
    } else if (write_result == -30) { // 真实只读特征，read和FLUSH仍可用。
        if (read_result != 512 || flush_result) // 限制不能掩盖其他设备错误。
            return 134; // 保留实际失败。
    } else if (write_result != -5 || read_result != -5 || flush_result != -5) // 已隔离设备以错误CQ交回，不伪造成功。
        return 135; // 未认识的错误不宣称通过。
    if (airi_io_free((uint64_t)writer) || airi_io_free((uint64_t)reader) || airi_io_free((uint64_t)error_buffer)) // 完成收集后才可释放。
        return 136; // 所有注册对象归还。
    int status = -1; // 自有wait普通退出码。
    if (airi_waitpid(child, &status) != child || status) // 另一个用户也必须正常结束。
        return 137; // 不留下工作负载身份。
    if (write_result != 512) { // 故障配置到这里已验证了完整错误结果与资源路径。
        airi_puts(write_result == -30 ? "[ASYNC_READONLY_OK] write=-30 read=512 cq=collected\n" : "[ASYNC_UNAVAILABLE_OK] valid_io=-5 cq=collected\n"); // 不冒充正常写读生命周期演示。
        return 0; // 内核与串口继续可用。
    }
    child = airi_fork(); // 接下来验证退出时仍有已接受请求。
    if (child < 0) // 不忽略容量失败。
        return 144; // 根进程退出仍由内核清理。
    if (!child) { // 子注册对象完全独立。
        long buffer = airi_io_alloc(); // 用自己的owner取得句柄。
        if (buffer < 0 || submit(AIRI_IO_READ, (uint64_t)buffer, 256, 70) || !await_started()) // 等到真实开始或完成，再离开身份。
            return 145; // 不用空队列假装在途退出。
        return 0; // exit撤销QUEUED/CQ，ACTIVE留内核收集，阶段见真实轨迹。
    }
    if (airi_waitpid(child, &status) != child || status) // 父取得退出状态不等于设备已结束。
        return 146; // 不隐藏孩子错误。
    long replacement = airi_io_alloc(); // 复用注册池槽的新owner对象。
    if (replacement < 0 || airi_io_put((uint64_t)replacement, "parent-new", 10) || submit(AIRI_IO_FLUSH, 0, 0, 80) || airi_io_next(&completion, 1) || completion.cookie != 80 || completion.result) // 单设备先收集旧ACTIVE再开始此FLUSH。
        return 147; // FLUSH完成之后旧设备借用必已结束。
    if (airi_io_get((uint64_t)replacement, output, 10) || memcmp(output, "parent-new", 10) || airi_io_free((uint64_t)replacement)) // 晚完成没有覆盖新owner内容。
        return 148; // 实际设备结果与代数身份一起验证。
    child = airi_fork(); // 另一个孩子以同PID换映像。
    if (child < 0) // 不等不存在进程。
        return 149; // 资源由根退出清理。
    if (!child) { // 新映像会检查空注册集合并等待退役ACTIVE。
        long buffer = airi_io_alloc(); // 当前孩子的旧轮次资源。
        if (buffer < 0 || submit(AIRI_IO_READ, (uint64_t)buffer, 256, 90) || !await_started()) // 后端确已推进这条请求。
            return 150; // 不把纯提交当成晚完成实测。
        airi_exec("async-reset"); // 成功保留PID，异步owner使用新epoch。
        return 151; // 成功不能回旧用户调用链。
    }
    if (airi_waitpid(child, &status) != child || status) // 同PID的新程序仍由父正常回收。
        return 152; // 新程序验证失败不能吞掉。
    long last = airi_io_alloc(); // 最后演示不收CQ直接退出的清理。
    if (last < 0 || submit(AIRI_IO_READ, (uint64_t)last, 256, 100) || submit(AIRI_IO_FLUSH, 0, 0, 101)) // 状态可能排队或已在途，以内核轨迹为准。
        return 153; // 不接受半个退出演示。
    airi_puts("[ASYNC_USER_OK] cookies=matched buffers=protected fork=private exec=reset exit=pending\n"); // 内核会在退出后继续排空并核对静态池。
    return 0; // 不提前释放last，exit负责取消/隔离/最终收集。
}
