#include "ui.h"     // 引入当前文件记录和服务回调合同，桌面状态尚未建立
#include <stdio.h>  // 主机观察使用标准文件输出与打印接口
#include <airi/base.h>  // 用自己的字节比较核对失败读取没有改动输出数组
int main(void) {  // 在主机准备输入、运行当前观察并以退出码报告结果
    const char *directory = "/home/airi/projects/telemetry";  // 本次查询与创建使用同一明确父目录，不靠查询改变服务隐式状态
    struct ui_backend backend;  // 观察程序持有回调合同副本，初始化后才可调用
    struct ui_file snapshot[UI_FILES];   // 调用者拥有32槽元信息数组，条数决定有效前缀
    char text[UI_DOC_SIZE], error[160];  // text接收main.lua正文；error接收创建失败原因，都是调用者独立数组
    ui_fixture_backend(&backend);  // 重置主机内存文件记录，把读写函数填入本观察程序的 backend 回调表
    unsigned before = backend.files(backend.context, directory, snapshot, UI_FILES);  // 将当前目录直接子项元信息复制给调用者快照
    if (before != 6)  // 固定目录应有六条；先确认有效条数，再访问 snapshot[0]
        return 1;  // 目录状态不符时停止，避免读取无有效记录的数组
    if (!backend.read(backend.context, snapshot[0].id, text, sizeof(text)))  // 用查询第0项保存的服务id读取正文，失败时结束观察，不能把快照下标当id
        return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
    printf("files=%u first=%s bytes=%u\n", before, snapshot[0].name, snapshot[0].bytes);  // 向主机终端报告本次观察的实际状态或输出位置
    char small[4] = "xyz";  // 只能容纳三内容字节及NUL，装不下本服务的main.lua全文
    bool copied = backend.read(backend.context, snapshot[0].id, small, sizeof(small));  // 故意提供不足容量，服务应拒绝整次复制
    printf("small buffer: success=%d preserved=%s\n", copied, small);  // 同时观察失败返回值和调用者原字节
    if (copied || airi_memcmp(small, "xyz", sizeof(small)))  // 任一成功或四字节变化都违反完整读取的合同
        return 1;  // 拒绝把截断或部分写入报告为通过
    bool created = backend.action(backend.context, directory, UI_CREATE_FILE, 0,  // 校验请求后改变服务记录，错误留在输出缓冲
                                  "hello.txt", "", error, sizeof(error));  // 明确创建名称hello.txt，正文为空；错误原因只写160字节调用者数组
    unsigned after = backend.files(backend.context, directory, snapshot, UI_FILES);  // 将当前目录直接子项元信息复制给调用者快照
    printf("after create: files=%u success=%d\n", after, created);  // 向主机终端报告本次观察的实际状态或输出位置
    bool duplicate = backend.action(backend.context, directory, UI_CREATE_FILE, 0,  // 校验请求后改变服务记录，错误留在输出缓冲
                                    "hello.txt", "", error, sizeof(error));  // 明确创建名称hello.txt，正文为空；错误原因只写160字节调用者数组
    printf("duplicate: success=%d error=%s\n", duplicate, error);  // 向主机终端报告本次观察的实际状态或输出位置
    return before != 6 || after != 7 || !created || duplicate;     // 初始六条、新建后七条、创建成功且重名失败才以0退出
}
