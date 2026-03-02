m_whitelist 目前使用的容器和查找方式在极速行情处理（如 MsgQueue::push
  这样的热点代码路径）中，性能开销是非常大的。


  虽然在 msg_queue.h 中它实际上是 std::unordered_set<std::string>（与 unordered_map
  底层同为哈希表），但主要的性能瓶颈并不在 O(1) 的时间复杂度上，而在于以下几个方面：


  1. 临时对象的构造与析构开销（最大元凶）



   1 bool MsgQueue::isWhitelisted(const char* szWindCode) const
   2 {
   3     // 每次查询都会触发 std::string(szWindCode)
   4     return m_whitelist.count(std::string(szWindCode)) > 0;
   5 }
  虽然代码注释里提到了 SSO（小字符串优化）不会触发堆内存分配，但这仅仅是省去了 new 操作。每次构造 std::string
  依然会隐式调用类似 strlen() 的操作来计算长度，并将字符串复制到栈上的对象内存中，函数结束时再析构。
  在 push 函数内的 for (int i = 0; i < item_count; ++i)
  循环中，每一条数据都会无谓地执行一次字符串的计算和拷贝，这在高频场景下极其昂贵。


  2. 重复的哈希计算
  构造出临时的 std::string 后，unordered_set::count() 需要计算这个字符串的哈希值。这意味着 CPU
  每次还要再遍历一遍这个字符串（例如 9 个字符的 "600000.SH"）来计算一次 Hash Code。


  3. CPU 缓存极其不友好 (Cache Miss)
  std::unordered_set 是基于节点（Node-based）的哈希表结构。即便它在理论上的查找时间复杂度是
  O(1)，但它每次查询访问大概率是指向一块非连续堆内存的指针。在紧凑的 push 循环中，这种内存不连续导致的 CPU
  缓存未命中（Cache Miss） 会使 CPU 停顿几百个时钟周期等待内存加载。

  ---


  优化方案：替换为排序的 std::vector + 二分查找


  由于行情白名单在 setWhitelist 初始化设置后，在运行期是完全只读的（不会增删元素），这非常适合使用内存连续的数据结构。


  我们可以将其改为 std::vector<std::string>。虽然二分查找的时间复杂度是 O(log
  N)，但只要白名单数量不是几十万级别（通常股票就几千只），由于避免了所有内存分配，并且对 CPU
  缓存极为友好，它的实际运行速度会远超现有的 unordered_set。