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

## HTTP 反向代理

在启动前注册单上游代理，前缀本身和其子路径都会匹配，普通 HTTP 中间件仍会执行：

```cpp
server.SetReverseProxy("/api", "http://127.0.0.1:8080/v1");
// /api/users?q=1 -> /v1/users?q=1
```

`ReverseProxyConfig` 默认移除匹配前缀并换成上游 URL 的路径；`strip_prefix = false` 保留原路径。改写保留百分号编码和原始查询字符串。`preserve_host = true` 保留客户端 Host，否则使用上游地址。支持 GET、HEAD、POST、PUT、PATCH、DELETE、OPTIONS、TRACE，固定路径路由可覆盖代理的通配路由。

请求与响应以 64 KiB 缓冲区逐段转发，支持固定长度、分块上传、大文件和 SSE。逐跳字段、所有 Connection 指定的字段及 trailers 不会转发，传输分帧在每一跳重新生成；重复的 Set-Cookie 等端到端字段会保留。X-Forwarded-For 使用 `GetClientIp()` 的可信代理规则，X-Forwarded-Host、Proto、Port 由当前请求重新生成，来路的 Forwarded 字段移除。

每条代理配置持有独立的上游连接池。`max_connections` 限制每个上游 origin 的连接数，`acquire_timeout` 限制容量等待，`timeout` 限制每步上游 I/O；持续有数据的流可以长时间保持。`header_limit` / `body_limit` 限制上游响应，客户端请求仍受服务器自身限制。代理保留上游状态和重定向响应，不自动重放请求或跟随重定向。

连接失败在响应头发出前返回 502，容量等待或上游 I/O 超时返回 504；响应已开始后发生错误则中断连接，避免把截断内容当作成功响应。下游关闭或服务器停机会取消上游操作，停机会等待代理会话排空。当前上游 URL 仅支持 HTTP，不能带查询或片段；WebSocket 升级通过 `SetWsForward` 单独注册。

## WebSocket 反向代理

启动前用 `HttpServer::SetWsForward` 注册字面量前缀及其子路径：

```cpp
server.SetWsForward("/socket", "ws://127.0.0.1:8080/chat");
// /socket/room?q=1 -> /chat/room?q=1
```

`WsForwardConfig` 的 `strip_prefix`、`preserve_host` 与 HTTP 代理含义相同，路径改写保留编码及原始查询。普通 HTTP 和本地 WebSocket 路由可与转发共存；更具体的本地 WebSocket 路由优先。匹配到的升级请求与本地 WebSocket 一样绕过 HTTP 中间件，需要在上游完成认证。

上游握手成功后才向下游发送 101。连接、握手拒绝和非法子协议协商返回 502，`connect_timeout`（默认 30 秒，涵盖 DNS、TCP、握手）超时返回 504；下游非法握手返回 400，版本不支持返回带版本提示的 426。保留 Origin、Authorization、Cookie、上游选择的子协议及重复 Set-Cookie；逐跳头移除，X-Forwarded-* 按 HTTP 代理规则重新生成。

每个方向一次读取一条完整消息，并等待目标发送完成后继续读取，保留 text / binary 类型；控制帧由每一跳各自处理 Ping / Pong，关闭帧的状态码和原因会传递。`message_limit` 默认 16 MiB，必须为正，限制两个方向；下游接收还受服务器 `SetBodyLimit` 限制。会话没有空闲超时；下游断开或服务器停机时取消上游，并等待转发协程排空。

当前仅支持 `ws://` 上游，不支持 URL 查询、片段、WSS、扩展协商或 WebSocket 压缩。

## 多上游负载均衡

`SetReverseProxy` 和 `SetWsForward` 都接受 `std::vector<UpstreamBackend>`；每项包含 `url` 和正整数 `weight`（默认 1），每个上游可以配置自己的基础路径。例如：

```cpp
mhttp::ReverseProxyConfig config;
config.balancing.strategy = mhttp::UpstreamStrategy::WeightedRoundRobin;
config.balancing.failure_cooldown = std::chrono::seconds{ 5 };
server.SetReverseProxy("/api", std::vector<mhttp::UpstreamBackend>{
    { "http://127.0.0.1:8081/v1", 3 },
    { "http://127.0.0.1:8082/v2", 1 }
}, config);

mhttp::WsForwardConfig ws_config;
ws_config.balancing.strategy = mhttp::UpstreamStrategy::LeastConnections;
server.SetWsForward("/socket", std::vector<mhttp::UpstreamBackend>{
    { "ws://127.0.0.1:8081/chat" },
    { "ws://127.0.0.1:8082/chat" }
}, ws_config);
```

`balancing` 使用共用的 `UpstreamGroupConfig`，每次注册拥有独立的选择状态。`RoundRobin`（默认）在健康节点间轮询，`WeightedRoundRobin` 使用平滑加权轮询，`LeastConnections` 选择活动数最少的节点并轮转处理平局。HTTP 每个请求重新选择，活动数包含连接池等待、连接建立和整个响应体；WebSocket 从选中、握手到会话关闭及转发排空都保持计数。空闲的 HTTP keep-alive 连接不计为活动请求。

上游传输或协议失败后，节点进入 `failure_cooldown`（默认 5 秒）；零表示关闭冷却，负数无效。冷却到期后由下一次真实请求探测该节点，每个节点同时最多一个探测，其他请求继续使用健康节点。有效 HTTP 响应头或经过验证的 WebSocket 握手会恢复节点；HTTP 4xx/5xx 作为应用响应原样转发。响应体截断会计入上游故障，下游断开、服务器停机、下游写入失败和连接池容量超时不会。

全部节点正在冷却或探测时返回 503。选中节点发生故障时，当前请求按原有规则返回 502/504 或中断已开始的响应，后续请求跳过该节点。不会自动向其他节点重放请求。取消探测会释放其名额，允许下一次请求立即探测；较早请求的成功结果不会覆盖后来的故障。

列表必须非空，权重必须为正，归一化后重复的 origin + 基础路径会在注册时报错。HTTP 列表仅接受 `http://`，WebSocket 列表仅接受 `ws://`；URL 不能带查询或片段。原有单 URL 重载保留直接尝试上游的行为，冷却仅作用于后端列表重载。

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

`AsyncClose(WebSocketCloseReason{code, reason}, ec)` 支持显式关闭码；保留码返回 `invalid_argument`。`CloseReason()` 返回线程安全的对端关闭状态快照，`code == 0` 表示没有可用的对端关闭码（尚未收到关闭帧或帧中未携带状态码）。

## WebSocket 客户端与 CONNECT 隧道

`mhttp::client::WsClient` 提供 `AsyncConnect`、`AsyncSend`、`AsyncRead`、`AsyncPing`、`AsyncPong`、`AsyncClose` 和 `AsyncAbort`，另有发送、控制帧、关闭的 future API。`WebSocketMessage` 持有消息字节及 text / binary 标志。

`Run` / `AsyncRun` 支持同步或协程消息回调。发送及控制帧排队执行，重叠读取返回 `Error::OperationInProgress`。连接超时涵盖 DNS、TCP 和升级握手；`Abort` 可取消握手及在途读取。正常关闭握手最多等待 5 秒，关闭后可重连。

`HandshakeHeaders()` 返回成功握手响应头的自有副本。`SetMessageLimit` 设置后续连接的接收上限（默认 16 MiB，零表示不限），`AsyncClose(WebSocketCloseReason, ec)` 和 `CloseReason()` 支持关闭状态的发送与线程安全查询。

`mhttp::client::ProxyClient::AsyncConnect("host:port", headers, ec)` 通过 HTTP 代理建立 CONNECT 隧道，支持 Proxy-Authorization。成功后用 `AsyncReadSome` / `AsyncWrite` 转发字节。读取保留与 CONNECT 响应一起收到的隧道数据，并跳过 1xx 中间响应。`SetTimeout` 限制 DNS、TCP 和 CONNECT 交换，`Close` / `AsyncClose` 可以取消握手及在途 I/O。同方向重叠 I/O 或重叠握手返回 `Error::OperationInProgress`。

这两个客户端的操作在各自 strand 上执行，可以从其他 executor 发起；`IsOpen()` 提供线程安全的握手完成状态快照。TLS / WSS、WebSocket 压缩尚未移植，TLS 连接返回 `operation_not_supported`。

## 测试

每个 `tests/test_*.cpp` 是独立程序，返回零表示成功。网络测试使用本机真实 TCP 服务。

- `mcpp test downloader`：下载、续传、重试、缓存、暂停取消。
- `mcpp test disk_cache`：持久化、元数据、活动句柄、过期与淘汰。
- `mcpp test proxy_ws`：CONNECT、WebSocket 消息、排队写入、握手取消与超时、重连。
- `mcpp test websocket`：服务端升级路由、预读首帧、文本与二进制、控制帧、消息限制及停机排空。
- `mcpp test reverse_proxy`：路径与请求头、连接复用、上传与 SSE、HEAD、网关错误和取消排空。
- `mcpp test ws_forward`：升级前准备、子协议与响应头、双向消息与关闭状态、预读帧、消息限制和停机取消。
- `mcpp test load_balance`：HTTP / WebSocket 分配比例、最少活动请求、握手计数、故障冷却与单次探测、取消和并发分配。
- `mcpp test`：全部回归测试。
