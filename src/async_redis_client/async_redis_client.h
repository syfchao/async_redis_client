
#pragma once

#include <memory>
#include <vector>
#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <iostream>

#include <hiredis/hiredis.h>
#include <uv.h>


/**
 * AsyncRedisClient 异步 Redis 客户端.
 *
 * AsyncRedisClient 会启动 thread_num 个线程, 每个线程具有 conn_per_thread 个到指定 redis 实例的连接.
 * 当通过 AsyncRedisClient::Execute() 来执行请求时, AsyncRedisClient 会(通过 round-robin 算法)选择一个线程,
 * 然后将请求交给该线程来进行处理, 线程内部会(通过 round-robin 算法)选择一个连接来处理该请求, 并且得到响应之后调用指定
 * 的回调函数.
 *
 * 从上看来, AsyncRedisClient 不支持事务这类与连接相关的命令, 虽然可以提供一个重载形式的 Execute(), 如下:
 *
 *      Execute(AsyncRedisClient::Connection conn, request, callback()
 *
 * 表示着在指定的连接上执行 request. 但是通过 redis.io 得知, 事务完全可以用 lua 脚本然后在一个请求中实现. 因此就没有
 * 提供这种形式的 Execute(). (毕竟一般形式的 Execute() 能不能被很好的实现都是一会事呢 @_@).
 */
struct AsyncRedisClient {

    // 调用 Start() 之后, 这些值将只读.
    std::string host;
    in_port_t port = 6379;
    size_t thread_num = 1;
    size_t conn_per_thread = 3;

public:
    using req_callback_t = std::function<void(redisReply *reply)/* noexcept */>;

public:
    ~AsyncRedisClient() noexcept;

    /**
     * 启动 AsyncRedisClient. 在此之后可以通过 AsyncRedisClient::Execute() 来执行请求.
     *
     * Start() 不是线程安全的(因为认为 Start() 相当于初始化函数, 没必要线程安全).
     *
     * Start() 只应该调用一次, 多次调用行为未定义.
     */
    void Start();

    /* 只有这里的方法才是线程安全的.
     * 意味着可以在不同的线程同时调用 `Stop()`, 或者 `Execute()`. 但是不能在一个线程中调用 `Stop()`, 另外一个线程
     * 调用 `~AsyncRedisClient()`.
     */
public:
    /**
     * 停止 AsyncRedisClient.
     * 此后当前 client 不再接受新的请求. 正在处理的请求回调会继续执行, 尚未处理的请求回调会接受 nullptr reply.
     *
     * Stop() 之后的 AsyncRedisClient 恢复到初始状态, 此时可以修改参数再一次 Start().
     */
    void Stop() {
        DoStopOrJoin(ClientStatus::kStop);
        return ;
    }

    /**
     * 停止 AsyncRedisClient.
     * 此后当前 client 不再接受新的请求. 正在处理的请求回调会继续执行, 尚未处理的请求回调仍会正常执行.
     *
     * Join() 之后的 AsyncRedisClient 恢复到初始状态, 此时可以修改参数再一次 Start().
     */
    void Join() {
        DoStopOrJoin(ClientStatus::kJoin);
        return ;
    }

    /**
     * 执行一个 redis 请求.
     *
     * 若该函数抛出异常, 则表明 request 不会被当前 Client 执行. 否则
     *
     * 若 request 成功处理, callback(reply) 中的 reply 指向着响应, reply 指向着的响应会在 callback() 返回之后
     * 释放. 若 request 未被成功处理, 执行 callback(nullptr).
     *
     * callback() MUST noexcept, 若 callback() 抛出了异常, 则会直接 std::terminate().
     *
     * TODO(ppqq): 增加 host, port 参数, 表明在指定的 redis 实例上执行请求.
     * TODO(ppqq): 增加超时参数. 当超时时, 以 nullptr reply 调用回调.
     * TODO(ppqq): 移动语义.
     */
    void Execute(const std::shared_ptr<std::vector<std::string>> &request,
                 const std::shared_ptr<req_callback_t> &callback);

    // TODO(ppqq): std::future 形式的同步接口.

private:
    using status_t = unsigned int;

    enum class ClientStatus : status_t {
        kInitial = 0,
        kStop,
        kJoin
    };

    enum class WorkThreadStatus : status_t {
        kUnknown = 0,
        kExiting,
        kRunning
    };

    struct RedisRequest {
        std::shared_ptr<std::vector<std::string>> cmd;
        std::shared_ptr<req_callback_t> callback;

    public:
        RedisRequest() noexcept = default;
        RedisRequest(const RedisRequest &) noexcept = default;

        // TODO(ppqq): 移动语义的构造函数;
        RedisRequest(const std::shared_ptr<std::vector<std::string>> &cmd_arg,
                     const std::shared_ptr<req_callback_t> &callback_arg) noexcept :
            cmd(cmd_arg),
            callback(callback_arg) {
        }

        RedisRequest(RedisRequest &&other) noexcept :
            cmd(std::move(other.cmd)),
            callback(std::move(other.callback)) {
        }

        RedisRequest& operator=(const RedisRequest &) noexcept = default;
        RedisRequest& operator=(RedisRequest &&other) noexcept {
            cmd = std::move(other.cmd);
            callback = std::move(other.callback);
            return *this;
        }

        void Fail() noexcept {
            (*callback)(nullptr);
            return ;
        }

        void Success(redisReply *reply) noexcept {
            (*callback)(reply);
            return ;
        }
    };

    struct WorkThread {
        bool started = false;
        std::thread thread;

        // TODO(ppqq): 后续将锁的粒度调细一点. 这样 status 可以原子化了, 对吧.
        std::mutex mux;
        WorkThreadStatus status{WorkThreadStatus::kUnknown};

        /* 不变量 3: 若 async_handle != nullptr, 则表明 async_handle 指向着的 uv_async_t 已经被初始化, 此时
         * 对其调用 uv_async_send() 不会触发 SIGSEGV.
         *
         * 其实这里可以使用读写锁, 因为 uv_async_send() 是线程安全的, 但是 uv_close(), uv_async_init() 这些
         * 并不是. 也即在执行 uv_async_send() 之前加读锁, 其他操作加写锁.
         *
         * TODO(ppqq): 读写锁.
         */
        uv_async_t *async_handle = nullptr;

        /* request_vec 的内存是由 work thread 来分配.
         *
         * 对于其他线程而言, 其检测到若 request_vec 为 nullptr, 则表明对应的 work thread 不再工作, 此时不能往
         * request_vec 中加入请求. 反之, 则表明 work thread 正常工作, 此时可以压入元素.
         */
        std::unique_ptr<std::vector<std::unique_ptr<RedisRequest>>> request_vec;

    public:
        WorkThreadStatus GetStatus() noexcept {
            WorkThreadStatus s;
            mux.lock(); // 鬼知道这里 lock() 会不会抛出异常...
            s = status;
            mux.unlock();
            return s;
        }

        void AsyncSendUnlock() noexcept {
            if (async_handle) {
                uv_async_send(async_handle); // 当 send() 失败了怎么办???
            }
            return ;
        }

        void AsyncSend() noexcept {
            mux.lock();
            AsyncSendUnlock();
            mux.unlock();
            return ;
        }
    };

private:
    std::atomic<ClientStatus> status_{ClientStatus::kInitial}; // lock-free
    std::atomic_uint seq_num{0};
    std::vector<WorkThread> work_threads_;

private:
    ClientStatus GetStatus() noexcept {
        return status_.load(std::memory_order_relaxed);
    }

private:
    static void WorkThreadMain(AsyncRedisClient *client, size_t idx) noexcept;

    static void OnAsyncHandle(uv_async_t* handle) noexcept;
    static void OnRedisReply(redisAsyncContext *c, void *reply, void *privdata) noexcept;

    static std::ostream& operator<<(std::ostream &out, ClientStatus status) {
        out << static_cast<status_t>(status);
        return out;
    }

    void DoStopOrJoin(ClientStatus op);
};
