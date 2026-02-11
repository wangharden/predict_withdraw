#ifndef MSG_QUEUE_H
#define MSG_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <utility>

#include "TDFAPI.h"
#include "TDCore.h"
#include "TDFAPIInner.h"
#include "TDNonMarketStruct.h"

// 深拷贝/释放 TDF_MSG 的函数声明
TDF_MSG* deepCopyTDF_MSG(const TDF_MSG* src);
void freeTDF_MSG(TDF_MSG* pMsg);

// 封装带所有权的 TDF_MSG 数据（兼容原有处理接口）
struct TdfMsgData {
    THANDLE hTdf;          // TDF句柄
    TDF_MSG msg;           // 拷贝后的 TDF_MSG（非指针，兼容原有接口）

    // 构造函数：接收原始 TDF_MSG 指针，内部完成深拷贝
    TdfMsgData(THANDLE h, const TDF_MSG* src);

    // 移动构造：转移内存所有权，避免重复拷贝
    TdfMsgData(TdfMsgData&& other) noexcept;

    // 移动赋值：转移内存所有权
    TdfMsgData& operator=(TdfMsgData&& other) noexcept;

    // 禁用拷贝构造/赋值（防止浅拷贝导致重复释放）
    TdfMsgData(const TdfMsgData&) = delete;
    TdfMsgData& operator=(const TdfMsgData&) = delete;

    // 析构函数：自动释放所有深拷贝的内存
    ~TdfMsgData();

private:
    // 深拷贝 TDF_MSG 关联数据（内部调用）
    void copyMsgData(const TDF_MSG* src);
    // 释放 TDF_MSG 关联数据（内部调用）
    void freeMsgData();
};

// 线程安全的消息队列（单例模式）
class MsgQueue {
public:
    // 获取单例实例
    static MsgQueue& getInstance() {
        static MsgQueue instance;
        return instance;
    }

    // 禁用拷贝/赋值
    MsgQueue(const MsgQueue&) = delete;
    MsgQueue& operator=(const MsgQueue&) = delete;

    // 入队：接收原始 TDF_MSG 指针，内部完成深拷贝
    void push(THANDLE hTdf, const TDF_MSG* pMsgHead);

    // 出队：阻塞直到有数据/队列停止（返回 false 表示队列停止）
    bool pop(TdfMsgData& data);

    // 停止队列（唤醒所有阻塞的 pop 操作）
    void stop();

    // 清空队列（释放所有未处理数据）
    void clear();

    // 检查队列是否停止
    bool isStopped() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_isStopped;
    }

private:
    // 私有构造函数（单例）
    MsgQueue() : m_isStopped(false) {}

    std::queue<TdfMsgData> m_queue;          // 底层队列
    mutable std::mutex m_mutex;              // 互斥锁（mutable 支持 const 方法加锁）
    std::condition_variable m_cv;            // 条件变量
    bool m_isStopped;                        // 停止标记
};

#endif // MSG_QUEUE_H
