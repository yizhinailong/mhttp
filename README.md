# mhttp

基于 Boost.Beast / Asio 的 C++23 协程 HTTP 模块库，使用 mcpp 构建。

```powershell
mcpp build
mcpp test
```

消费者使用 `import mhttp;`。使用 Boost 的协程、缓冲区等 API 时，在模块导入前包含相应 Boost 头文件；标准库使用 `import std;`。

## 下载器与缓存

`mhttp::client::Downloader` 从共享 `HttpClientPool` 借用连接。`AsyncDownload(url, path, headers)` 返回错误码，`Download(...)` 返回同一结果的 future；运行 future API 时，需要让传入的 executor 持续运行。

下载写入 `<path>.part`，成功后替换目标文件。失败或取消保留 part 文件，下次从其大小继续请求；part 文件应属于同一 URL、同一资源版本。服务器忽略 Range 时重新写入，返回 416 时在重试预算内丢弃旧 part 并重试。非法 206、非 identity 的内容编码会返回 `Error::ProtocolViolation`。调用方应避免多个下载器同时写入同一路径。

`SetConfig` 设置重试次数、每步 I/O 超时、连接池获取超时、重定向上限、下载限速及重试间隔。408、429、5xx 和中断的传输会重试；其他非成功状态返回 `Error::HttpStatus`，可用 `HttpStatus()` 读取响应状态。

`SetProgressCallback` 报告总量、已下载字节、速度和 ETA；`SetStateCallback` 报告阶段变化。`Pause()` 在读取块之间暂停，`Resume()` 唤醒；`Cancel()` 关闭当前连接并唤醒暂停或退避等待。等待连接池时，取消在获取连接后生效。单个下载器只允许一次下载，重叠调用返回 `Error::AlreadyInUse`。回调在下载协程上执行，应简短且不抛异常。`SuggestedFilename()` 返回当前资源的 Content-Disposition 文件名，移除目录部分。

通过 `SetCache(std::make_shared<mhttp::client::DiskCache>(directory))` 启用缓存。缓存保存完整文件，支持 max-age、新鲜缓存直接命中、ETag / Last-Modified 验证和 304 后更新元数据。请求或响应的 no-store 禁止存储，请求 no-cache 强制验证。凭据参与缓存键，跨源重定向移除凭据和 Host。暂不缓存带 Vary 的响应。

`Cache` 是与 HTTP 无关的 body + 不透明 metadata 接口，允许自定义存储。`DiskCache` 支持持久化、大小上限、最近使用顺序淘汰、默认有效期、单项截止时间、清空和清理。已经取出的 `Entry` 在替换或淘汰后仍可读取原内容，包括 Windows。目录由独占锁保护，同一目录无法被第二个缓存实例同时打开；创建或锁定失败抛出 `std::runtime_error`。

## WebSocket 服务端

`Router::SetWsHandler(path, open, message, close)` 注册升级路由，三个回调均支持同步函数或 Asio 协程。回调收到 `WebSocketConn::WeakPtr`，锁定后可读取握手请求、发送消息或关闭连接。例如：

```cpp
server.GetRouter().SetWsHandler(
    "/ws/:room",
    [](mhttp::WebSocketConn::WeakPtr) {},
    [](mhttp::WebSocketConn::WeakPtr weak,
       mhttp::WebSocketMessage message) -> boost::asio::awaitable<void> {
        if (auto connection{ weak.lock() }) {
            boost::system::error_code ec;
            co_await connection->AsyncSend(message, ec);
        }
    },
    [](mhttp::WebSocketConn::WeakPtr) {}
);
```

路由沿用 HTTP 的静态、正则、参数、通配路径及匹配优先级。同一路径可以同时注册 HTTP 和 WebSocket 处理器；没有匹配的升级路由时继续使用 HTTP 路由。匹配成功的升级不经过 HTTP 中间件。非法握手或带请求体的升级会被拒绝，握手失败不调用连接回调。HTTP 阶段预读到的首帧字节会保留。

`HttpRequest()` 保留路径参数、查询、请求头及 `Data()`，只能在连接的 `Executor()` 上访问。所有回调在该 strand 上运行，消息按接收顺序逐个回调；成功建立的会话结束后调用一次 `close`，此时 `IsOpen()` 为 false。

`WebSocketConn` 提供 `AsyncSend`、`AsyncPing`、`AsyncPong`、`AsyncClose`、`AsyncAbort`，以及对应的 future API `Send`、`Ping`、`Pong`、`Close`、`Abort`。这些操作可从其他 executor 发起，发送与控制帧排队执行；不要在 I/O 线程上阻塞等待 future。Ping / Pong 载荷最多 125 字节，关闭原因最多 123 字节，超限返回 `message_size`。正常关闭最多等待对端 5 秒；`Abort` 立即取消 I/O。

`SetBodyLimit` 为新升级连接设置单条接收消息的大小上限，零只允许空消息；`WriteTimeout` 的快照用于升级握手。已建立连接不设置空闲超时。`HttpServer::Stop` 取消连接并等待在途写操作及关闭回调完成，回调本身应及时结束。

## WebSocket 客户端与 CONNECT 隧道

`mhttp::client::WsClient` 提供 `AsyncConnect`、`AsyncSend`、`AsyncRead`、`AsyncPing`、`AsyncPong`、`AsyncClose` 和 `AsyncAbort`，另有发送、控制帧、关闭的 future API。`WebSocketMessage` 持有消息字节及 text / binary 标志。

`Run` / `AsyncRun` 支持同步或协程消息回调。发送及控制帧排队执行，重叠读取返回 `Error::OperationInProgress`。连接超时涵盖 DNS、TCP 和升级握手；`Abort` 可取消握手及在途读取。正常关闭握手最多等待 5 秒，关闭后可重连。

`mhttp::client::ProxyClient::AsyncConnect("host:port", headers, ec)` 通过 HTTP 代理建立 CONNECT 隧道，支持 Proxy-Authorization。成功后用 `AsyncReadSome` / `AsyncWrite` 转发字节。读取保留与 CONNECT 响应一起收到的隧道数据，并跳过 1xx 中间响应。`SetTimeout` 限制 DNS、TCP 和 CONNECT 交换，`Close` / `AsyncClose` 可以取消握手及在途 I/O。同方向重叠 I/O 或重叠握手返回 `Error::OperationInProgress`。

这两个客户端的操作在各自 strand 上执行，可以从其他 executor 发起；`IsOpen()` 提供线程安全的握手完成状态快照。TLS / WSS、WebSocket 压缩尚未移植，TLS 连接返回 `operation_not_supported`。

## 测试

每个 `tests/test_*.cpp` 是独立程序，返回零表示成功。网络测试使用本机真实 TCP 服务。

- `mcpp test downloader`：下载、续传、重试、缓存、暂停取消。
- `mcpp test disk_cache`：持久化、元数据、活动句柄、过期与淘汰。
- `mcpp test proxy_ws`：CONNECT、WebSocket 消息、排队写入、握手取消与超时、重连。
- `mcpp test websocket`：服务端升级路由、预读首帧、文本与二进制、控制帧、消息限制及停机排空。
- `mcpp test`：全部回归测试。
