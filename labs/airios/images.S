.section .rodata.user_elf,"a" /* 原始ELF作为内核只读输入，不在内核直接执行。 */
.balign 8 /* 输入开头至少按8字节对齐，解析仍使用memcpy。 */
.global airi_hello_elf_start, airi_hello_elf_end /* 编译期固定映像边界。 */
airi_hello_elf_start: /* 文件头也包含在输入字节中。 */
.incbin AIRI_HELLO_ELF /* Make传入当前配置自己的ELF路径，不复制另一配置旧产物。 */
airi_hello_elf_end: /* 解析器只允许读取边界之内。 */
.section .note.GNU-stack,"",@progbits /* 包装对象不需要可执行栈。 */
.section .rodata.user_elf,"a" /* 第二个独立ELF同样只是内核装载输入。 */
.balign 8 /* 边界按八字节对齐，解析器仍安全按字节复制。 */
.global airi_process_elf_start, airi_process_elf_end /* 多进程用户工作负载的完整文件边界。 */
airi_process_elf_start: /* 包含独立文件头与程序表。 */
.incbin AIRI_PROCESS_ELF /* 路径由本配置的Make依赖提供。 */
airi_process_elf_end: /* 内核不能读过此边界。 */
.section .rodata.user_elf,"a" /* 第三个独立ELF作为exec目标。 */
.balign 8 /* 原始输入边界固定。 */
.global airi_worker_elf_start, airi_worker_elf_end /* 新用户映像范围。 */
airi_worker_elf_start: /* 解析从文件头开始。 */
.incbin AIRI_WORKER_ELF /* Make提供同一配置刚生成的文件。 */
airi_worker_elf_end: /* 输入长度不包括后续损坏夹具。 */
.global airi_broken_elf_start, airi_broken_elf_end /* 受控截断ELF不需要外部文件。 */
airi_broken_elf_start: /* 只有魔数与少量标识，不足64字节头。 */
.byte 0x7f, 0x45, 0x4c, 0x46, 2, 1, 1, 0 /* 解析器必须拒绝，不能改掉旧进程。 */
airi_broken_elf_end: /* 失败输入恰好八字节。 */

.section .rodata /* 文件工作负载只在内核中保留映像字节。 */
.global airi_files_elf_start, airi_files_elf_end /* 用户空间由装载器另建。 */
airi_files_elf_start: /* 映像首地址。 */
.incbin AIRI_FILES_ELF /* 使用本配置实际链接的用户ELF。 */
airi_files_elf_end: /* 映像右端，不把后续字节算入文件。 */

.section .rodata /* 存储用户映像属于只读内核初始目录。 */
.global airi_storage_elf_start, airi_storage_elf_end /* 同一映像按argv选择storage或recover。 */
airi_storage_elf_start: /* 文件首。 */
.incbin AIRI_STORAGE_ELF /* 本配置真实链接的用户ELF。 */
airi_storage_elf_end: /* 文件半开右端。 */

.section .rodata /* 独立管道用户映像。 */
.global airi_pipes_elf_start, airi_pipes_elf_end /* 只发布文件边界。 */
airi_pipes_elf_start: /* 文件起点。 */
.incbin AIRI_PIPES_ELF /* 当前配置的真实ELF。 */
airi_pipes_elf_end: /* 文件右端。 */

.section .rodata /* 阻塞用户程序映像。 */
.global airi_waits_elf_start, airi_waits_elf_end /* 边界只供内核装载。 */
airi_waits_elf_start: /* 文件开始。 */
.incbin AIRI_WAITS_ELF /* 本配置独立ELF。 */
airi_waits_elf_end: /* 半开右端。 */

.section .rodata /* 用户异步工作负载与exec别名共用同一ELF。 */
.global airi_async_elf_start, airi_async_elf_end /* 初始映像边界。 */
airi_async_elf_start: /* 文件首字节。 */
.incbin AIRI_ASYNC_ELF /* 本配置真实链接的用户程序。 */
airi_async_elf_end: /* 文件半开右端。 */

.section .rodata /* 新程序仍只是内核只读段中的输入文件。 */
.global airi_shell_elf_start, airi_shell_elf_end /* shell装载边界。 */
airi_shell_elf_start: /* 完整文件头起点。 */
.incbin AIRI_SHELL_ELF /* 当前配置的真实用户shell。 */
airi_shell_elf_end: /* 半开右端。 */
.global airi_demo_elf_start, airi_demo_elf_end /* 贯通演示装载边界。 */
airi_demo_elf_start: /* 完整ELF起点。 */
.incbin AIRI_DEMO_ELF /* 当前配置的真实异步用户程序。 */
airi_demo_elf_end: /* 半开右端。 */

.section .rodata /* 孤儿回收使用真正用户ELF，不在内核中直接执行。 */
.global airi_orphans_elf_start, airi_orphans_elf_end /* 初始目录只持有完整输入字节。 */
airi_orphans_elf_start: /* 文件头起点。 */
.incbin AIRI_ORPHANS_ELF /* 当前配置的独立ELF。 */
airi_orphans_elf_end: /* 文件半开右端。 */

.section .rodata /* 页释放程序由同一静态ELF装载器处理。 */
.global airi_memory_elf_start, airi_memory_elf_end /* 内核只保留输入字节边界。 */
airi_memory_elf_start: /* 文件首。 */
.incbin AIRI_MEMORY_ELF /* 本配置的新用户映像。 */
airi_memory_elf_end: /* 文件右端。 */

.section .rodata /* 常驻init仍由普通用户ELF装载路径启动。 */
.global airi_init_elf_start, airi_init_elf_end /* 发布文件边界，不在内核直接调用main。 */
airi_init_elf_start: /* 文件首。 */
.incbin AIRI_INIT_ELF /* 当前配置的独立静态ELF。 */
airi_init_elf_end: /* 文件右端。 */

.section .rodata /* 大BSS程序仍只嵌入实际文件，不嵌入运行时零填充页。 */
.global airi_capacity_elf_start, airi_capacity_elf_end /* 新初始映像的输入边界。 */
airi_capacity_elf_start: /* ELF文件首。 */
.incbin AIRI_CAPACITY_ELF /* 当前配置刚链接完成的独立用户程序。 */
airi_capacity_elf_end: /* 文件半开右端。 */

.section .rodata /* 浮点工作负载仍经过同一个静态ELF装载器。 */
.global airi_simd_elf_start, airi_simd_elf_end /* 主程序输入边界。 */
airi_simd_elf_start: /* 文件首。 */
.incbin AIRI_SIMD_ELF /* 当前配置带真实SSE/x87指令的ELF。 */
airi_simd_elf_end: /* 文件右端。 */
.global airi_simd_reset_elf_start, airi_simd_reset_elf_end /* exec新入口观察映像。 */
airi_simd_reset_elf_start: /* 文件首。 */
.incbin AIRI_SIMD_RESET_ELF /* 当前配置的状态快照程序。 */
airi_simd_reset_elf_end: /* 半开右端。 */

.balign 8 /* 两份FS观察程序仍是独立ELF字节，不是内核函数。 */
.global airi_tls_elf_start, airi_tls_elf_end /* 注册目录使用真实起止范围。 */
airi_tls_elf_start: /* 本配置的用户程序。 */
.incbin AIRI_TLS_ELF /* 路径由Makefile传入，不借宿主ABI。 */
airi_tls_elf_end: /* 当前ELF结束位置。 */
.balign 8 /* reset映像单独对齐。 */
.global airi_tls_reset_elf_start, airi_tls_reset_elf_end /* 成功exec的新目标。 */
airi_tls_reset_elf_start: /* 新映像独立入口。 */
.incbin AIRI_TLS_RESET_ELF /* main只查询新基址并输出。 */
airi_tls_reset_elf_end: /* 新ELF的右端。 */

.balign 8 /* C本地变量探针的真实静态ELF。 */
.global airi_ctls_elf_start, airi_ctls_elf_end /* 一个映像供普通与reset两个目录名。 */
airi_ctls_elf_start: /* 完整程序头含PT_TLS。 */
.incbin AIRI_CTLS_ELF /* 来自本配置交叉链接产物。 */
airi_ctls_elf_end: /* 用户文件右端。 */
.balign 8 /* 单字节TLS布局单独编译。 */
.global airi_ctls_char_elf_start, airi_ctls_char_elf_end /* 不混入普通探针的64字节对齐要求。 */
airi_ctls_char_elf_start: /* 唯一TLS符号的ELF。 */
.incbin AIRI_CTLS_CHAR_ELF /* 文件大小与PT_TLS长度并不相等。 */
airi_ctls_char_elf_end: /* 映像范围结束。 */
.balign 8 /* 错误ELF仍作为只读字节输入校验器。 */
.global airi_tls_big_elf_start, airi_tls_big_elf_end /* 装载应拒绝这份超限模板。 */
airi_tls_big_elf_start: /* 不把该程序当内核故障执行。 */
.incbin AIRI_TLS_BIG_ELF /* 明确含4097字节TLS对象。 */
airi_tls_big_elf_end: /* 输入边界供ELF校验。 */

.balign 8 /* 最大TLS模板的纯零ELF。 */
.global airi_ctls_page_elf_start, airi_ctls_page_elf_end /* 独立运行页与TCB页的实际程序。 */
airi_ctls_page_elf_start: /* 没有TLS文件初值，但有4096字节内存模板。 */
.incbin AIRI_CTLS_PAGE_ELF /* 真实链接器产生的PT_TLS，而非手工标记。 */
airi_ctls_page_elf_end: /* 校验器可读输入的右端。 */

.balign 8 /* POSIX风格包装仍在用户ELF中，不进内核ABI。 */
.global airi_posix_elf_start, airi_posix_elf_end /* 普通和reset目录项共用映像字节。 */
airi_posix_elf_start: /* 含errno静态TLS的新ELF。 */
.incbin AIRI_POSIX_ELF /* 只有该显式目标链接posix.o。 */
airi_posix_elf_end: /* 完整用户文件边界。 */

.balign 8 /* 向量用户程序同样经过独立ELF装载。 */
.global airi_vector_elf_start, airi_vector_elf_end /* 真实用户映像范围。 */
airi_vector_elf_start: /* 含POSIX包装的观察程序。 */
.incbin AIRI_VECTOR_ELF /* 本配置交叉链接产物。 */
airi_vector_elf_end: /* 只读输入的右端。 */

.balign 8 /* 对象分配器只进入选择它的用户ELF。 */
.global airi_malloc_elf_start, airi_malloc_elf_end /* 两个目录名共用这一完整映像。 */
airi_malloc_elf_start: /* 用户页与对象借用的真实观察程序。 */
.incbin AIRI_MALLOC_ELF /* 由本配置静态链接，不借宿主malloc。 */
airi_malloc_elf_end: /* 输入范围的排他右端。 */

.balign 8 /* 共享线程观察仍作为独立用户ELF装载。 */
.global airi_threads_elf_start, airi_threads_elf_end /* 目录只保存这份只读映像范围。 */
airi_threads_elf_start: /* 自有运行库含C11 TLS和共享执行桥接器。 */
.incbin AIRI_THREADS_ELF /* 当前配置真正交叉链接的文件。 */
airi_threads_elf_end: /* 完整输入文件的排他右端。 */

.balign 8 /* TLS零尾部观察仍经真实静态ELF装载。 */
.global airi_ctls_tail_elf_start, airi_ctls_tail_elf_end /* 输入范围供真实初始程序目录使用。 */
airi_ctls_tail_elf_start: /* 普通BSS小于tbss的最小程序。 */
.incbin AIRI_CTLS_TAIL_ELF /* 当前配置真正链接的模板与LOAD描述。 */
airi_ctls_tail_elf_end: /* 完整文件的排他右端。 */

.balign 8 /* 私有内存等待探针仍经静态ELF装载。 */
.global airi_futex_elf_start, airi_futex_elf_end /* 完整只读输入范围。 */
airi_futex_elf_start: /* 含自有线程库的用户程序。 */
.incbin AIRI_FUTEX_ELF /* 当前配置交叉链接产物。 */
airi_futex_elf_end: /* 输入文件的排他右端。 */

.balign 8 /* 互斥观察在真实用户地址空间运行。 */
.global airi_mutex_elf_start, airi_mutex_elf_end /* 初始程序目录使用完整只读文件范围。 */
airi_mutex_elf_start: /* 独立C用户映像的文件首。 */
.incbin AIRI_MUTEX_ELF /* 本配置链接好的锁与工作者程序。 */
airi_mutex_elf_end: /* 文件排他右端。 */

.balign 8 /* 四个执行者共享分配器的真实用户ELF。 */
.global airi_heap_threads_elf_start, airi_heap_threads_elf_end /* 初始目录不保存宿主进程指针。 */
airi_heap_threads_elf_start: /* 本配置完整静态程序文件。 */
.incbin AIRI_HEAP_THREADS_ELF /* 明确选择带持锁yield的观察版本。 */
airi_heap_threads_elf_end: /* 输入的排他右端。 */

.balign 8 /* TCB身份和生命周期观察的真实用户ELF。 */
.global airi_identity_elf_start, airi_identity_elf_end /* 文件范围由初始程序目录借用。 */
airi_identity_elf_start: /* 同一文件支持普通入口与exec重建观察。 */
.incbin AIRI_IDENTITY_ELF /* 当前配置的静态程序，不是宿主模拟查询。 */
airi_identity_elf_end: /* 输入的排他右端。 */
.balign 16 /* 新的观察ELF保持独立只读范围。 */
.global airi_mutex_races_elf_start, airi_mutex_races_elf_end /* 内核目录只借实际文件字节。 */
airi_mutex_races_elf_start: /* 包含完整ELF头与段。 */
.incbin AIRI_MUTEX_RACES_ELF /* 不调用宿主程序冒充观察。 */
airi_mutex_races_elf_end: /* 范围右端不含字节。 */
.balign 16 /* 独立变化通知观察映像。 */
.global airi_events_elf_start, airi_events_elf_end /* 目录借用实际ELF范围。 */
airi_events_elf_start: /* 从文件头开始校验。 */
.incbin AIRI_EVENTS_ELF /* 完整静态用户程序，不执行宿主程序。 */
airi_events_elf_end: /* 范围右端不含字节。 */
.balign 16 /* 有界生产消费的独立用户映像。 */
.global airi_queue_elf_start, airi_queue_elf_end /* 初始目录只借只读ELF范围。 */
airi_queue_elf_start: /* 从完整文件头开始。 */
.incbin AIRI_QUEUE_ELF /* 真实链接的自有队列程序。 */
airi_queue_elf_end: /* 右端不含字节。 */
.balign 16 /* 线程源码兼容入口的独立观察ELF。 */
.global airi_pthread_elf_start, airi_pthread_elf_end /* 内核只借完整文件范围。 */
airi_pthread_elf_start: /* 从ELF头开始装载。 */
.incbin AIRI_PTHREAD_ELF /* 自有适配器与真实用户观察。 */
airi_pthread_elf_end: /* 范围右端不包含字节。 */
.balign 16 /* 对等收取与闭环拒绝的实际用户映像。 */
.global airi_join_elf_start, airi_join_elf_end /* 目录借用完整ELF边界。 */
airi_join_elf_start: /* 包含文件头和所有用户段。 */
.incbin AIRI_JOIN_ELF /* 不借宿主线程演示。 */
airi_join_elf_end: /* 右端不包含字节。 */
.balign 16 /* 公开互斥源码接口的真实观察ELF。 */
.global airi_pthread_mutex_elf_start, airi_pthread_mutex_elf_end /* 初始目录借用完整边界。 */
airi_pthread_mutex_elf_start: /* 从ELF文件头开始。 */
.incbin AIRI_PTHREAD_MUTEX_ELF /* 自有线程、锁与包装，不执行主机例子。 */
airi_pthread_mutex_elf_end: /* 范围右端不含字节。 */
.balign 16 /* 八字节私有等待的真实用户观察。 */
.global airi_futex64_elf_start, airi_futex64_elf_end /* 目录借完整文件边界。 */
airi_futex64_elf_start: /* 从静态ELF头校验。 */
.incbin AIRI_FUTEX64_ELF /* 不使用宿主futex替身。 */
airi_futex64_elf_end: /* 右端不含字节。 */
.balign 16 /* 完整64位序号接口的实际用户观察。 */
.global airi_events64_elf_start, airi_events64_elf_end /* 目录借用只读文件范围。 */
airi_events64_elf_start: /* 从静态ELF头开始。 */
.incbin AIRI_EVENTS64_ELF /* 不以宿主计算结果替代guest。 */
airi_events64_elf_end: /* 右端不含字节。 */
