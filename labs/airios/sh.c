#include <airi_libc.h> // shell只通过用户库访问内核，不链接console内部对象。
static int equal(const char *a, const char *b) { // 有界行已经NUL终止。
    size_t n=strlen(a); return n==strlen(b) && !memcmp(a,b,n); // 先比长度，避免读过短字符串。
}
static void number(long value) { // 最小十进制输出，不引入完整printf。
    char digits[24]; unsigned used=0; unsigned long n; // 数值最大PID与错误码均可容纳。
    if (value<0) { airi_puts("-"); n=(unsigned long)(-(value+1))+1; } // 即使最小有符号数也不溢出。
    else n=(unsigned long)value; // 正数直接转换。
    do { digits[used++]=(char)('0'+n%10); n/=10; } while (n); // 先产生逆序数字。
    while (used) airi_write(1,&digits[--used],1); // 从高位逐字输出。
}
static void execute(const char *name) { // 一条无参数程序名对应一个孩子。
    long child=airi_fork(); // shell保留原页，子得到独立用户副本与共享FD引用。
    if (child<0) { airi_puts("fork failed: "); number(child); airi_puts("\n"); return; } // 资源满仍回到提示符。
    if (!child) { // 只有孩子执行替换，父shell不能被覆盖。
        long error=airi_exec(name); // 成功不返回，CLOEXEC关闭复制来的输入FD0。
        airi_puts("exec failed: "); number(error); airi_puts("\n"); airi_exit(127); // 失败旧孩子仍存在，明确退出供父收取。
    }
    int status=0; long waited=airi_waitpid(child,&status); // 父阻塞等待指定孩子，不以yield轮询。
    if (waited!=child) { airi_puts("wait failed\n"); return; } // 不把缺状态当成功。
    airi_puts("[SHELL_COMMAND_DONE] name="); airi_puts(name); // 名称是父自己的行缓冲，孩子exec不影响它。
    airi_puts(" code="); number(status); airi_puts("\n"); // 这是孩子真实退出结果。
}
int main(void) { // 内核监控台run sh后从此用户入口继续。
    char guard[1]={'Q'}; // 小容量失败不能覆盖用户哨兵。
    if (airi_programs(guard,1)!=-28 || guard[0]!='Q' || airi_programs((char *)0,256)!=-14 || airi_read(0,guard,0)!=0 || airi_read(0,(void *)0,1)!=-14) // 目录与UART输出先完整验证，空读不阻塞。
        return 2; // 未通过不能发布shell就绪。
    char line[64]; size_t used=0; int overflow=0,skip_lf=0; // 固定输入预算，过长行不截断执行。
    airi_puts("[USER_SHELL_READY]\nCommands: help programs exit, or one program name.\n"); // 交互与内核monitor的run语法不同。
    for (;;) { // 用户shell保持运行，exit才回到内核监控台。
        airi_puts("[USER_SHELL_PROMPT]\nsh> "); used=0; overflow=0; // 每条新命令有明确观察边界。
        for (;;) { // 行编辑留在用户，不把桌面控件引进内核。
            char c; long n=airi_read(0,&c,1); // 没字节时真正IO_READ阻塞，由PIT驱动UART重查。
            if (n!=1) { airi_puts("shell stdin unavailable\n"); return 1; } // 嵌套exec sh没有stdin时安全退出。
            if (skip_lf && c=='\n') { skip_lf=0; continue; } // CRLF只执行一次，不产生空命令。
            skip_lf=0; // 非LF字节仍正常解释。
            if (c=='\r' || c=='\n') { skip_lf=c=='\r'; airi_puts("\n"); break; } // 提交整行。
            if ((c==8 || c==127) && !overflow) { // 支持Backspace和Delete退格。
                if (used) { --used; airi_puts("\b \b"); } // 只撤已输入字符。
                continue; // 退格不是程序名的一部分。
            }
            if ((unsigned char)c<32 || (unsigned char)c>126) continue; // 首轮命令限定可见ASCII。
            if (used+1<sizeof line && !overflow) { line[used++]=c; airi_write(1,&c,1); } // 始终保留NUL位置。
            else overflow=1; // 后续字节一直消费到换行，不能执行被截短的前缀。
        }
        line[used]=0; // 所有strlen与exec都有终止界限。
        if (overflow) { airi_puts("line too long; discarded\n"); continue; } // 放弃整条过长输入。
        if (!used) continue; // 空行只重画提示符。
        if (equal(line,"exit")) { airi_puts("[USER_SHELL_EXIT]\n"); return 0; } // 退出与普通孩子结束分开。
        if (equal(line,"help")) { airi_puts("Use programs, one name, or exit. No arguments, redirects, jobs or nested sh.\n"); continue; } // 准确说明当前语法。
        if (equal(line,"programs")) { // 目录从内核真实注册表查询，避免维护第二套名单。
            char names[256]; long n=airi_programs(names,sizeof names); // 完整结果或错误，不显示半目录。
            if (n>=0) airi_puts(names); else airi_puts("catalog unavailable\n"); // 名称文本没有内部指针。
            continue; // 内建查询不用fork。
        }
        if (equal(line,"sh")) { airi_puts("nested sh has no separate input terminal\n"); continue; } // 不与父竞争单UART。
        execute(line); // 其他名称交给真实exec，未知名字同样有明确失败结果。
    }
}
