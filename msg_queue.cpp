#include "msg_queue.h"
#include <cstring>
#include <stdexcept>

// ===================== 全局工具函数：深拷贝/释放 TDF_MSG =====================
// 深拷贝 TDF_MSG 及关联数据（内部辅助函数）
static TDF_APP_HEAD* copyAppHead(const TDF_APP_HEAD* src) {
    if (src == nullptr) return nullptr;
    TDF_APP_HEAD* dst = new (std::nothrow) TDF_APP_HEAD();
    if (dst) *dst = *src;
    return dst;
}

static void* copyDataBody(const TDF_MSG* src) {
    if (src->pData == nullptr || src->pAppHead == nullptr) return nullptr;

    const TDF_APP_HEAD* pAppHead = src->pAppHead;
    int itemCount = pAppHead->nItemCount;
    int itemSize = pAppHead->nItemSize;
    if (itemCount <= 0 || itemSize <= 0) return nullptr;

    switch (src->nDataType) {
    case MSG_DATA_MARKET: {
        TDF_MARKET_DATA* pSrc = static_cast<TDF_MARKET_DATA*>(src->pData);
        TDF_MARKET_DATA* pDst = new (std::nothrow) TDF_MARKET_DATA[itemCount];
        if (pDst) memcpy(pDst, pSrc, itemCount * itemSize);
        return pDst;
    }
    case MSG_DATA_ORDER: {
        TDF_ORDER* pSrc = static_cast<TDF_ORDER*>(src->pData);
        TDF_ORDER* pDst = new (std::nothrow) TDF_ORDER[itemCount];
        if (pDst) memcpy(pDst, pSrc, itemCount * itemSize);
        return pDst;
    }
    case MSG_DATA_TRANSACTION: {
        TDF_TRANSACTION* pSrc = static_cast<TDF_TRANSACTION*>(src->pData);
        TDF_TRANSACTION* pDst = new (std::nothrow) TDF_TRANSACTION[itemCount];
        if (pDst) memcpy(pDst, pSrc, itemCount * itemSize);
        return pDst;
    }
    default:
        return nullptr;
    }
}

// 深拷贝 TDF_MSG 及关联数据
TDF_MSG* deepCopyTDF_MSG(const TDF_MSG* src) {
    if (src == nullptr) return nullptr;

    TDF_MSG* dst = new (std::nothrow) TDF_MSG();
    if (!dst) return nullptr;

    // 浅拷贝基础字段
    *dst = *src;
    // 深拷贝应用头和数据体
    dst->pAppHead = copyAppHead(src->pAppHead);
    dst->pData = copyDataBody(src);

    return dst;
}

// 释放 TDF_MSG 及关联数据
void freeTDF_MSG(TDF_MSG* pMsg) {
    if (pMsg == nullptr) return;

    // 释放应用头
    if (pMsg->pAppHead) {
        delete pMsg->pAppHead;
        pMsg->pAppHead = nullptr;
    }

    // 释放数据体
    if (pMsg->pData) {
        switch (pMsg->nDataType) {
        case MSG_DATA_MARKET:
            delete[] static_cast<TDF_MARKET_DATA*>(pMsg->pData);
            break;
        case MSG_DATA_ORDER:
            delete[] static_cast<TDF_ORDER*>(pMsg->pData);
            break;
        case MSG_DATA_TRANSACTION:
            delete[] static_cast<TDF_TRANSACTION*>(pMsg->pData);
            break;
        default:
            break;
        }
        pMsg->pData = nullptr;
    }

    delete pMsg;
}
#include <iostream>
// ===================== TdfMsgData 成员函数实现 =====================
// 构造函数：深拷贝原始 TDF_MSG 及关联数据（兼容原有接口）
TdfMsgData::TdfMsgData(THANDLE h, const TDF_MSG* src) : hTdf(h)
{
    // 初始化 msg 为空
    memset(&msg, 0, sizeof(TDF_MSG));

    // 空指针直接返回（不抛异常，避免程序崩溃）
    if (src == nullptr)
    {
        return;
    }

    // 1. 浅拷贝 TDF_MSG 基础字段
    msg = *src;

    // 2. 深拷贝关联数据
    copyMsgData(src);
}

// 移动构造：转移内存所有权
TdfMsgData::TdfMsgData(TdfMsgData&& other) noexcept {
    hTdf = other.hTdf;
    msg = other.msg;

    // 清空原对象，避免重复释放
    other.hTdf = nullptr;
    memset(&other.msg, 0, sizeof(TDF_MSG));
}

// 移动赋值：转移内存所有权
TdfMsgData& TdfMsgData::operator=(TdfMsgData&& other) noexcept
{
    if (this != &other)
    {
        // 释放当前对象的内存
        freeMsgData();

        // 转移所有权
        hTdf = other.hTdf;
        msg = other.msg;

        // 清空原对象
        other.hTdf = nullptr;
        memset(&other.msg, 0, sizeof(TDF_MSG));
    }
    return *this;
}

// 析构函数：释放所有深拷贝的内存
TdfMsgData::~TdfMsgData() {
    freeMsgData();
}

// 深拷贝 TDF_MSG 关联数据
void TdfMsgData::copyMsgData(const TDF_MSG* src) {
    // 深拷贝应用头
    if (src->pAppHead)
    {
        msg.pAppHead = new (std::nothrow) TDF_APP_HEAD();
        if (msg.pAppHead)
        {
            *msg.pAppHead = *src->pAppHead;
        }
    }

    // 深拷贝数据体
    if (src->pData && src->pAppHead)
    {
        const TDF_APP_HEAD* pAppHead = src->pAppHead;
        int itemCount = pAppHead->nItemCount;
        int itemSize = pAppHead->nItemSize;

        if (itemCount > 0 && itemSize > 0)
        {
            switch (src->nDataType)
            {
            case MSG_DATA_MARKET:
            {
                TDF_MARKET_DATA* pSrc = static_cast<TDF_MARKET_DATA*>(src->pData);
                TDF_MARKET_DATA* pDst = new (std::nothrow) TDF_MARKET_DATA[itemCount];
                if (pDst)
                {
                    memcpy(pDst, pSrc, itemCount * itemSize);
                    msg.pData = pDst;
                }
                break;
            }
            case MSG_DATA_ORDER:
            {
                TDF_ORDER* pSrc = static_cast<TDF_ORDER*>(src->pData);
                TDF_ORDER* pDst = new (std::nothrow) TDF_ORDER[itemCount];
                if (pDst)
                {
                    memcpy(pDst, pSrc, itemCount * itemSize);
                    msg.pData = pDst;
                }
                break;
            }
            case MSG_DATA_TRANSACTION:
            {
                TDF_TRANSACTION* pSrc = static_cast<TDF_TRANSACTION*>(src->pData);
                TDF_TRANSACTION* pDst = new (std::nothrow) TDF_TRANSACTION[itemCount];
                if (pDst) {
                    memcpy(pDst, pSrc, itemCount * itemSize);
                    msg.pData = pDst;
                }
                break;
            }
            default:
                msg.pData = nullptr;
                break;
            }
        }
    }
}

// 释放 TDF_MSG 关联数据
void TdfMsgData::freeMsgData() {
    // 释放应用头
    if (msg.pAppHead) {
        delete msg.pAppHead;
        msg.pAppHead = nullptr;
    }

    // 释放数据体
    if (msg.pData) {
        switch (msg.nDataType) {
        case MSG_DATA_MARKET:
            delete[] static_cast<TDF_MARKET_DATA*>(msg.pData);
            break;
        case MSG_DATA_ORDER:
            delete[] static_cast<TDF_ORDER*>(msg.pData);
            break;
        case MSG_DATA_TRANSACTION:
            delete[] static_cast<TDF_TRANSACTION*>(msg.pData);
            break;
        default:
            break;
        }
        msg.pData = nullptr;
    }

    // 清空基础字段
    memset(&msg, 0, sizeof(TDF_MSG));
    hTdf = nullptr;
}

// ===================== MsgQueue 成员函数实现 =====================
// 入队：接收原始 TDF_MSG 指针，内部完成深拷贝
void MsgQueue::push(THANDLE hTdf, const TDF_MSG* pMsgHead) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 队列已停止或空指针，直接返回
    if (m_isStopped || pMsgHead == nullptr) {
        return;
    }

    try {
        // 构造 TdfMsgData（自动深拷贝），移动入队
        m_queue.emplace(hTdf, pMsgHead);
        m_cv.notify_one(); // 通知处理线程有新数据
    } catch (const std::exception& e) {
        // 静默处理异常，避免程序崩溃
        (void)e;
    } catch (...) {}
}

// 出队：阻塞直到有数据/队列停止
bool MsgQueue::pop(TdfMsgData& data) {
    std::unique_lock<std::mutex> lock(m_mutex);

    // 阻塞等待：有数据 或 队列停止
    m_cv.wait(lock, [this]() {
        return !m_queue.empty() || m_isStopped;
    });

    // 队列停止且无数据，返回false
    if (m_isStopped && m_queue.empty()) {
        return false;
    }

    // 移动语义取出数据（兼容原有接口）
    data = std::move(m_queue.front());
    m_queue.pop();

    return true;
}

// 停止队列
void MsgQueue::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_isStopped = true;
    m_cv.notify_all(); // 唤醒所有阻塞的pop线程
}

// 清空队列（释放所有未处理数据）
void MsgQueue::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::queue<TdfMsgData> emptyQueue;
    std::swap(m_queue, emptyQueue); // 清空原队列，自动释放数据
}
