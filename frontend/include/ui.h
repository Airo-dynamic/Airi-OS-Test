#ifndef AIRI_UI_H  // 当前编译单元尚未包含文件服务接口时才展开
#define AIRI_UI_H  // 标记文件服务接口已经包含
#include <airi/boot.h>    // 取得bool、size_t等本次合同所需基础类型
#define UI_DOC_SIZE 8192  // 每份正文最多8191内容字节，另留一格NUL
#define UI_FILES 32  // 参考服务和调用者目录快照各有32个记录槽

enum ui_mode { UI_REFERENCE };  // 本阶段只有主机内存参考服务
// 目录创建是当前唯一修改能力，编辑保存等操作到实际使用时加入。
enum ui_action {  // 目前服务只接受两种创建请求，编号仅在本进程调用中使用
    UI_CREATE_FILE,  // 明确目录中创建空文字文件
    UI_CREATE_DIR    // 明确目录中创建目录项
};
struct ui_file {  // 目录快照记录，只包含身份与元信息，不包含正文
    int id;  // 本服务会话的文件身份，槽号加1，与查询数组下标区分
    bool directory;  // true为目录，不能当普通正文读取
    char name[64];   // 不含父目录的名称，最多63字节及NUL
    char path[128];  // 完整服务路径，最多127字节及NUL
    unsigned bytes;  // 文档内容字节数，不计终止零
};
struct ui_backend {  // 回调表副本由调用者保存，文件记录仍归提供者拥有
    enum ui_mode mode;  // 标明这份数据属于本主机进程的参考服务
    void *context;      // 每次调用原样传回的私有上下文；本提供者使用静态表
    unsigned (*files)(void *, const char *, struct ui_file *, unsigned);  // 目录和数组容量输入，返回实际复制条数
    bool (*read)(void *, int, char *, size_t);  // 按id完整读取正文，容量含NUL，失败不修改输出
    bool (*action)(void *, const char *, enum ui_action, int, const char *, const char *, char *, size_t);  // 显式目录、创建种类与名称输入，失败原因写给调用者
};
void ui_fixture_backend(struct ui_backend *);  // 重置参考服务并填好本阶段三项回调
#endif  // 结束文件服务接口的重复包含保护
