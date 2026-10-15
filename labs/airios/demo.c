#include <airi_libc.h> // 页、FD、进程与异步请求都走自有用户系统调用。
static void require(int yes, const char *message) { // 应用失败只退出当前程序，shell继续存在。
    if (!yes) { airi_puts("[PIPELINE_FAIL] "); airi_puts(message); airi_puts("\n"); airi_exit(1); } // 不要求内核PANIC。
}
static void operation(unsigned op, uint64_t handle, uint64_t cookie, long expected) { // 每次接受、等待、验证是三个不同动作。
    struct airi_io_submission submission={.sector=257,.cookie=cookie}; // 独立实验扇区，不覆盖正式追加日志。
    require(airi_io_submit(op,handle,&submission)==0,"submit"); // 只表示资源与CQ信用已接受。
    struct airi_io_completion cqe; // 用户栈一直存活到该调用返回。
    require(airi_io_next(&cqe,1)==0,"wait CQ"); // 空队列登记等待，期间别的进程可执行。
    require(cqe.cookie==cookie && cqe.operation==op && cqe.buffer==handle && cqe.result==expected,"CQ identity/result"); // 不把接口返回0误读成读到0字节。
    airi_puts("[PIPELINE_CONTINUE] CQ consumed; buffer reusable\n"); // 真实收取后才继续。
}
int main(void) { // shell fork后exec，独立页表和异步owner。
    unsigned char *page=airi_alloc(4096); require(page!=NULL,"allocate page"); // 页退出时统一回收，没有单对象free。
    for (unsigned i=0;i<4096;++i) require(page[i]==0,"zero page"); // 新映射不能泄露旧进程数据。
    for (unsigned i=0;i<512;++i) page[i]=(unsigned char)(i*17+3); // 端到端读回的真实数据。
    long file=airi_open("/demo",AIRI_READ|AIRI_WRITE|AIRI_CREATE|AIRI_TRUNCATE); require(file>=0,"RAM file"); // 同时覆盖真实FD路径。
    require(airi_write((unsigned)file,page,256)==256 && airi_write((unsigned)file,page+256,256)==256,"file writes"); // 单次用户副本上限256，显式分两段。
    require(airi_pread((unsigned)file,page+1024,256,0)==256 && airi_pread((unsigned)file,page+1280,256,256)==256,"file reads"); // 显式偏移不改写位置。
    require(!memcmp(page,page+1024,512) && airi_close((unsigned)file)==0,"file roundtrip"); // 路径内容由RAMFS拥有，FD按引用释放。
    long write_buffer=airi_io_alloc(),read_buffer=airi_io_alloc(); require(write_buffer>0 && read_buffer>0,"register copies"); // 代数句柄不是用户VA。
    require(airi_io_put((uint64_t)write_buffer,page,512)==0,"copy payload"); // 请求借用的是内核注册副本，不直接DMA用户页。
    long child=airi_fork(); require(child>=0,"fork worker"); // 子异步owner为空，不继承两个handle的所有权。
    if (!child) { // 另一个真实用户做少量工作，展示父等待期间CPU仍能推进。
        uint64_t start=airi_ticks(); while (airi_ticks()-start<3) { } // 等真实三tick，使父先登记CQ等待。
        airi_puts("[PIPELINE_OTHER_PROGRESS]\n"); // 测试核对它是否位于父登记与唤醒之间。
        while (airi_ticks()-start<30) { } // 仅演示用户计算，不伪造设备延迟。
        airi_exit(0); // 父随后wait收取身份和页资源。
    }
    operation(AIRI_IO_WRITE,(uint64_t)write_buffer,1,512); // sector257写入完成。
    operation(AIRI_IO_FLUSH,0,2,0); // 再同步设备，不能以WRITE完成代替FLUSH。
    operation(AIRI_IO_READ,(uint64_t)read_buffer,3,512); // 最后从同一设备读回。
    require(airi_io_get((uint64_t)read_buffer,page+512,512)==0 && !memcmp(page,page+512,512),"device readback"); // CQ之后才允许获取注册副本。
    require(airi_io_free((uint64_t)write_buffer)==0 && airi_io_free((uint64_t)read_buffer)==0,"release buffers"); // 不在完成前释放。
    int status=-1; require(airi_waitpid(child,&status)==child && status==0,"reap worker"); // 孩子也必须正常结束。
    airi_puts("[PIPELINE_USER_OK] page=4096 ram_file=512 device=512 flush=0 child=reaped\n"); return 0; // shell收取这个程序的退出码。
}
