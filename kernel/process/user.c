#include <airi/user.h> // 用户陷入和演示入口。
#include <airi/memory.h> // 物理页、内核区域与长期内核根。
#include <airi/vm.h> // 用户根借用高半区，低半区单独拥有。
#include <airi/timer.h> // 系统调用只查询实际tick。
#include <airi/kernel.h> // 串口字节输出与失败停止。
#define USER_CODE 0x400000ull // 首个受控用户程序的执行地址。
#define USER_STACK 0x70000000ull // 一页RW/NX用户栈，前后都没有映射。
#define KERNEL_STACK 0xffffe10000001000ull // 用户陷入内核使用的四页栈，前面留保护页。
static struct { // 当前检查点同一时刻只有一个用户上下文。
    struct airi_address_space space; // 自有根与低半区表，借用长期内核映射。
    struct airi_kernel_region stack; // TSS.RSP0指向该内核栈的高端。
    uint64_t owned[2]; // 代码页与用户栈数据页由本对象持有。
    uint64_t fault_error; // 保留用户硬件PF的实际错误位以验证supervisor保护。
    unsigned count, pid, user_ticks; // 数据页数量、用户身份与真实用户态IRQ数。
    bool running; // 只有CPU正在运行本用户或处理其陷入时为真。
    struct airi_trap_frame *frame; // 当前内核栈上完整的用户返回现场。
} process; // 不依赖内核堆扩容，不把它宣称为通用进程表。
static unsigned next_pid = 1; // 两次顺序运行得到不同身份。
static struct airi_trap_frame *kernel_return; // int129保存启动任务继续执行所需的完整现场。
extern const uint8_t airi_user_probe_start[], airi_user_probe_end[]; // 链接进只读段的受控位置无关映像。
extern const uint8_t airi_user_load_bad_start[], airi_user_load_bad_end[]; // 用户直接读取内核映射的硬件权限验证。
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范返回栈必须由内核拒绝。
extern const uint8_t airi_user_bad_start[], airi_user_bad_end[]; // 特权指令故障的第二个映像。
static bool copy_user(void *output, uint64_t address, size_t bytes) { // 只接受本进程低半区且每页USER的完整范围。
    if (address >= AIRI_USER_LIMIT || bytes > AIRI_USER_LIMIT - address) // 先排除右端溢出与内核地址。
        return false; // 不触碰传入地址。
    for (size_t done = 0; done < bytes;) { // 在任何复制或输出前先验证所有跨越的页。
        unsigned flags; // 取得当前页的叶子权限。
        if (!airi_vm_lookup(&process.space, address + done, NULL, &flags) || !(flags & AIRI_VM_USER)) // 必须存在且允许用户访问。
            return false; // 跨到未映射页也整体拒绝。
        size_t part = 4096 - ((address + done) & 4095); // 只推进到当前页末尾。
        if (part > bytes - done) // 最后一段可能不足一页。
            part = bytes - done; // 限制到实际剩余长度。
        done += part; // 继续验证下一页。
    }
    for (size_t done = 0; done < bytes;) { // 只有完整范围通过后才复制到内核有界数组。
        uint64_t physical; // 页内偏移由lookup保留。
        airi_vm_lookup(&process.space, address + done, &physical, NULL); // 当前IF为零，尚无其他执行者修改本进程映射。
        size_t part = 4096 - ((address + done) & 4095); // 本次只读取一页中的有效段。
        if (part > bytes - done) // 最后一段限制长度。
            part = bytes - done; // 不读取请求外字节。
        airi_memcpy((uint8_t *)output + done, airi_memory_direct(physical), part); // 通过已验证物理页的HHDM别名复制，不直接信任用户指针。
        done += part; // 继续复制剩余段。
    }
    return true; // 当前实现没有按需缺页或并发映射变化。
}
static struct airi_trap_frame *finish(int code) { // 仍在用户的内核栈上，不能在此释放它。
    airi_log("[USER_EXIT] pid=%u code=%d user_ticks=%u", process.pid, code, process.user_ticks); // 输出实际退出或故障结果。
    process.running = false; // 后续kernel-only时钟不再计为用户执行。
    airi_memory_use_kernel(); // 先切回长期根，旧用户根稍后由启动任务销毁。
    kernel_return->rax = (uint64_t)(int64_t)code; // int129返回值交给启动调用方。
    return kernel_return; // 汇编将从另一条完整现场恢复，而非返回用户坏指令。
}
struct airi_trap_frame *airi_user_trap(struct airi_trap_frame *f) { // C只在完整现场已保存后选择继续位置。
    if (f->vector == 129 && f->cs == 8 && !process.running && process.space.root) { // 只有内核能通过DPL0入口启动已准备用户上下文。
        kernel_return = f; // 保留启动栈上的现场，栈会一直存在。
        airi_trap_set_stack(process.stack.virtual_address + process.stack.count * 4096ull); // 硬件从user进入kernel时切到本进程栈。
        if (!airi_vm_activate(&process.space)) // 高半区已包含代码、启动栈、TSS与本内核栈。
            airi_panic("cannot activate user address space"); // 切换失败不能进入ring3。
        process.running = true; // 之后的用户陷入才能引用当前身份。
        return process.frame; // iretq按CS/SS降低到ring3，并启用用户RFLAGS.IF。
    }
    if (!process.running || (f->cs & 3) != 3) // 不能把内核故障或非法启动请求当成用户调用。
        airi_panic("user trap without a running user context"); // 内核错误仍停止整个系统。
    process.frame = f; // 保存这次真实硬件形成的用户返回现场。
    if (f->vector != 128) { // 用户CPU故障终止该用户执行，不吞掉内核故障。
        process.fault_error = f->error; // 在清理前保留本次权限错误位。
        airi_log("[USER_FAULT] pid=%u vector=%llu error=0x%llx rip=0x%llx cr2=0x%llx", process.pid, (unsigned long long)f->vector, (unsigned long long)f->error, (unsigned long long)f->rip, (unsigned long long)(f->vector == 14 ? airi_trap_fault_address() : 0)); // 实际CPL3故障信息。
        return finish(-(128 + (int)f->vector)); // 自有教学退出编码，不宣称Linux wait ABI。
    }
    if (f->rax == 1) { // write(fd, pointer, bytes)，目前仅串口stdout/stderr。
        char buffer[256]; // 内核栈有界副本，不借用户缓冲给后续异步执行。
        if (f->rdi != 1 && f->rdi != 2) // 当前没有通用FD表。
            f->rax = (uint64_t)-9ll; // -EBADF。
        else if (f->rdx > sizeof(buffer)) // 防止用户指定巨大长度覆盖内核栈。
            f->rax = (uint64_t)-22ll; // -EINVAL。
        else if (!copy_user(buffer, f->rsi, (size_t)f->rdx)) // 验证完整范围后才复制。
            f->rax = (uint64_t)-14ll; // -EFAULT，内核继续运行。
        else { // 缓冲区现在由本次内核调用持有。
            airi_serial_write_bytes(buffer, (size_t)f->rdx); // 按长度输出，包括潜在NUL，不依赖字符串终止。
            f->rax = f->rdx; // 返回本次逻辑输出字节数。
        }
    } else if (f->rax == 2) // exit不返回当前用户指令。
        return finish((int)(f->rdi & 255)); // 最小退出码范围明确为0–255。
    else if (f->rax == 3) // getpid返回当前上下文身份。
        f->rax = process.pid; // 用户不能伪造内核持有的PID。
    else if (f->rax == 5) // 查询单调tick，不是墙钟时间。
        f->rax = airi_timer_ticks(); // 内核中断门当前关闭IF，不在调用中调度。
    else // 未提供的ABI编号必须明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS，不返回未初始化寄存器作为结果。
    return airi_user_resume(f); // 只把经过规范地址与标志检查的现场交给iretq。
}
void airi_user_tick(void) { // 仅由CS表明来自user的IRQ调用。
    if (!process.running) // 用户态IRQ必须有活的内核所有者。
        airi_panic("user timer without current context"); // 不静默把未知栈当当前进程。
    ++process.user_ticks; // 当前只有一个用户上下文，不宣称已经轮转抢占多个进程。
}
static void cleanup(void) { // 必须已经回到启动栈和长期内核根。
    if (process.space.root && !airi_vm_destroy(&process.space)) // 先解除并归还自有页表，不动借用的内核表。
        airi_panic("cannot destroy inactive user tables"); // 不能归还仍被活动根引用的数据页。
    while (process.count) // 用户代码与用户栈数据页分别归还。
        if (!airi_memory_return_page(process.owned[--process.count])) // 先退到最后一个真正拥有的页。
            airi_panic("cannot return owned user page"); // 归属错误不继续复用内存。
    if (process.stack.count && !airi_memory_region_free(&process.stack)) // 启动栈继续运行，因此此处才可销毁用户内核栈。
        airi_panic("cannot reclaim user kernel stack"); // 不能在finish中提前做这一步。
    airi_memset(&process, 0, sizeof(process)); // 不留下可重复释放的根或句柄。
    kernel_return = NULL; // 原现场已返回，下一次必须重新登记。
}
static bool prepare(const uint8_t *image, size_t bytes) { // 本节是受控位置无关字节映像，下一节再解析独立ELF。
    if (!bytes || bytes > 4096 || !airi_memory_region_create(&process.stack, KERNEL_STACK, 4)) // 内核栈先存在，用户根才能借到它的高半区链接。
        return false; // 没有完整栈就不克隆根。
    if (!airi_memory_user_space(&process.space)) // 根低半区从空开始。
        goto failed; // 已创建内核栈需要回收。
    for (unsigned i = 0; i < 2; ++i) { // 代码一页、用户栈一页。
        uint64_t physical = airi_memory_take_page(); // 数据页所有权属于process。
        if (!physical) // 耗尽时没有本次页可操作。
            goto failed; // 统一回收前面成功部分。
        process.owned[process.count++] = physical; // 先登记所有权，后面任何失败都能找到它。
        airi_memset(airi_memory_direct(physical), 0, 4096); // 用户不能看到旧内核所有者数据。
    }
    airi_memcpy(airi_memory_direct(process.owned[0]), image, bytes); // 复制受控程序，内核原始字节本身从不执行。
    if (!airi_vm_map(&process.space, USER_CODE, process.owned[0], AIRI_VM_USER | AIRI_VM_EXEC) || !airi_vm_map(&process.space, USER_STACK, process.owned[1], AIRI_VM_USER | AIRI_VM_WRITE)) // 用户代码RX，栈RW/NX。
        goto failed; // 页表与数据分别由cleanup回收。
    process.pid = next_pid++; // 准备完成才分配运行身份。
    process.frame = (void *)(uintptr_t)(KERNEL_STACK + 4 * 4096 - sizeof(struct airi_trap_frame)); // 初次iretq现场位于本内核栈顶下方。
    airi_memset(process.frame, 0, sizeof(*process.frame)); // 首次没有旧寄存器值。
    process.frame->rip = USER_CODE; // 处理器将从低半区用户代码取指。
    process.frame->cs = 27; // GDT的ring3代码选择子24加RPL3。
    process.frame->ss = 35; // GDT的ring3数据选择子32加RPL3。
    process.frame->rsp = USER_STACK + 4096 - 16; // 初始用户栈保持16字节对齐。
    process.frame->rflags = 0x202; // 必需bit1加IF，IOPL仍为0，用户不能cli。
    return true; // 还没有切根和进入用户执行。
failed: // 当前仍在长期根和启动栈，半成品都可安全释放。
    cleanup(); // 拆用户表、归还数据、最后解除内核栈。
    return false; // 不运行部分准备的映像。
}
static void run_one(const uint8_t *image, size_t bytes, int expected) { // 同一启动调用链顺序运行四个隔离演示。
    struct airi_memory_stats before, after; // 统计包含页表和两种栈的整个生命周期。
    airi_memory_snapshot(&before); // 准备之前基准。
    if (!prepare(image, bytes)) // 失败已完整回滚。
        airi_panic("cannot prepare user demonstration"); // 不把半成品交给iretq。
    long code; // int129恢复到启动现场时由RAX交回结束原因。
    __asm__ volatile("int $0x81" : "=a"(code) :: "memory", "cc"); // DPL0门保存当前完整现场，再切到受控用户现场。
    if (code != expected || (!expected && process.user_ticks < 2) || (expected == -142 && process.fault_error != 5)) // 正常程序必须真实运行并被用户态IRQ打断。
        airi_panic("user context ended with unexpected result"); // 用户GP与正常退出分别验证。
    cleanup(); // 已经回启动栈，才允许归还用户内核栈。
    airi_memory_snapshot(&after); // 取得本轮结束后的真实空闲数量。
    if (before.pages_free != after.pages_free || before.heap_free != after.heap_free) // 根、子表、数据页和内核栈均须回收。
        airi_panic("user context leaked owned memory"); // 不让隔离演示留下隐藏成本。
}
void airi_user_demo(void) { // 当前只验证边界，不提供shell启动和完整进程调度。
    run_one(airi_user_probe_start, (size_t)((uintptr_t)airi_user_probe_end - (uintptr_t)airi_user_probe_start), 0); // 用户输出、非法指针和真实IRQ后正常退出。
    run_one(airi_user_bad_start, (size_t)((uintptr_t)airi_user_bad_end - (uintptr_t)airi_user_bad_start), -141); // CPL3执行cli得到GP13，仅结束该用户。
    run_one(airi_user_load_bad_start, (size_t)((uintptr_t)airi_user_load_bad_end - (uintptr_t)airi_user_load_bad_start), -142); // 真实user读内核PTE得到PF error=5。
    run_one(airi_user_rsp_bad_start, (size_t)((uintptr_t)airi_user_rsp_bad_end - (uintptr_t)airi_user_rsp_bad_start), -22); // 在内核iretq之前拒绝不规范RSP。
    airi_log("[USER_DEMO_OK] ring=3 syscall=int80 bad_pointers=2 user_gp=13 user_pf=14 bad_rsp=rejected reclaimed=1"); // 内核恢复后继续串口，不宣称ELF/fork已经完成。
}

struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *f) { // CPU将以ring3权限恢复，错误返回现场不能先在ring0引发GP。
    if (f->cs != 27 || f->ss != 35 || f->rip >= AIRI_USER_LIMIT || f->rsp >= AIRI_USER_LIMIT) { // 本ABI只允许低半区规范地址与固定用户段。
        airi_log("[USER_RETURN_REJECTED] pid=%u rip=0x%llx rsp=0x%llx", process.pid, (unsigned long long)f->rip, (unsigned long long)f->rsp); // 记录被拒绝值，不解引用它。
        return finish(-22); // 结束当前用户并回启动栈，不把坏RSP交给iretq。
    }
    f->rflags &= ~((3ull << 12) | (1ull << 14) | (1ull << 17)); // 清IOPL、NT和VM，防止用户标志影响内核返回语义。
    f->rflags |= 0x202; // 保持保留bit1和用户IF，当前不支持用户禁用调度时钟。
    return f; // 内存访问权限继续由user页表硬件检查。
}
