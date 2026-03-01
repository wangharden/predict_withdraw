#ifndef STOCK_DATA_MANAGER_FACTORY_H
#define STOCK_DATA_MANAGER_FACTORY_H

#include <unordered_map>
#include <string>
#include <mutex>
#include <memory>
#include <vector>
#include <atomic>

// 前置声明
class StockDataManager;
using StockCode = std::string;

// ===================== 股票管理器工厂类 =====================
class StockDataManagerFactory {
public:
    // 单例获取（线程安全）
    static StockDataManagerFactory& getInstance();

    // 获取/创建股票实例（不存在则创建）
    StockDataManager* getStockManager(const StockCode& stockCode);
    // 重置指定股票数据
    void resetStockManager(const StockCode& stockCode);
    // 重置所有股票数据（如收盘后）
    void resetAll();
    // 删除指定股票实例（释放内存）
    void removeStockManager(const StockCode& stockCode);

    // 禁用拷贝/移动
    StockDataManagerFactory(const StockDataManagerFactory&) = delete;
    StockDataManagerFactory& operator=(const StockDataManagerFactory&) = delete;
    StockDataManagerFactory(StockDataManagerFactory&&) = delete;
    StockDataManagerFactory& operator=(StockDataManagerFactory&&) = delete;

    bool updateLimitupPrice(const std::string & sKhh);
    bool init_factory(const std::vector<std::string>& filename);

private:
    // 私有构造/析构（单例）
    StockDataManagerFactory() = default;
    ~StockDataManagerFactory() = default;

    std::unordered_map<StockCode, std::unique_ptr<StockDataManager>> m_stockManagerMap;
    std::mutex m_mutex; // 实例映射表锁
    std::atomic<bool> m_readOnlyAfterInit{false};
};

#endif // STOCK_DATA_MANAGER_FACTORY_H
