# 客户端接口

客户端的请求与响应类型通过 `import mhttp;` 使用，公共接口只包含标准库和 mhttp 类型。
`client::HttpClient` 提供惰性连接和 `Send(Request) -> Task<Result<Response>>`。
构造时不进行 DNS 或 socket I/O；连接、发送与读取均异步进行。快捷方法、完整的空闲连接重连和重定向待后续接入。

```cpp
import std;
import mhttp;

auto main() -> int {
    mhttp::Runtime runtime;
    runtime.Start(2).value();
    mhttp::client::HttpClient client{ runtime, "http://127.0.0.1:8080/items" };
    mhttp::client::Request request{ mhttp::Method::Get, "" }; // 使用构造 URL 的路径。
    request.Query().Add("tag", "one two");
    auto result = mhttp::SyncWait(runtime, client.Send(std::move(request)));
    client.Stop();
    runtime.Stop();
    runtime.Join().value();
    return result && result->Status() == mhttp::Status::Ok ? 0 : 1;
}
```

Runtime 必须活过客户端和所有已签发的 Task。Send 的参数在初次挂起前已转移到拥有状态，
客户端析构会请求取消，Task 仍能完成收尾。客户端不能复制或移动。
任务在调用方原来的 scheduler 上恢复；可以让多个客户端共享 Runtime，也可以从服务端 handler 中等待 Send。
非 I/O 线程可以使用 SyncWait；运行时没有启动时，SyncWait 自身会抛出 NotRunning。
InitializationError 可提前检查 URL 和配置；Send 同样以 Result 返回这些错误。
Host / Port 返回不可变的构造地址，HTTPS 返回 UnsupportedScheme，空地址、userinfo 和越界 URL 端口返回 InvalidUrl。

`client::Request(method, target)` 拥有方法、原始目标、有序多值 Headers、追加 QueryParams 和二进制 Body。
既可以使用 Method 枚举，也可以提供扩展方法 token。构造不进行 I/O，发送边界会完整校验方法和目标。
SetMethod 校验后替换；SetTarget 保留原始路径、查询和 fragment，等待发送时规范化。
Query 中的参数独立于 Target 已有的查询，发送时按调用方插入顺序追加。
SetContent 接受拥有数据的 string 和可选 Content-Type；省略类型不推断媒体类型。
非法媒体类型或方法更新保留原有状态，Headers 的修改同样拒绝 CRLF 注入。

`client::Response` 拥有只读的状态、接收到的 reason、版本、重复头部、单独存放的 trailer、
完整二进制 body 及两端 Endpoint 快照。404 等 HTTP 错误状态属于正常响应，不是传输错误。
KeepAlive 表示完整响应分帧允许连接继续使用；之后对端仍可能关闭它。
默认构造的未解析响应状态为 Unknown，不能当作已收到的 HTTP 响应。

两种消息均只能移动，借用的视图在相关修改、赋值或销毁时失效。
Request 交给 Send 后，调用方不能继续使用它的借用数据。Response 可以独立于客户端存活，
在所有权存活期间允许并发只读访问。移动后的源对象仅可销毁或赋值。

`client::ClientOptions` 规定 DNS 与所有端点尝试共用的连接超时、完整响应读取超时、
完整请求写入超时，以及响应头字节数/字段数、响应 body 和完整发送数据的上限。
所有超时和上限必须为正；响应 body 上限可以为零，表示只接受空 body。
连接超时覆盖 DNS 和 IPv4 / IPv6 端点的顺序尝试；读超时覆盖整个最终响应，含中间响应、头部和 body；
写超时覆盖请求头和完整 body 的所有部分写入。超时或失败会关闭连接，下一次请求重新连接。
目前尚未提供可选的 overall / step / never 超时策略。

发送准备保留已有 percent escape 和原始查询的顺序，将 QueryParams 中的键和值分别编码后追加，
移除 fragment；空目标或仅有 fragment 时采用构造 URL 的路径和查询。
发送目标中的空格、CRLF 和 NUL 等字节会编码，非法方法和非 origin-form 目标在发送前被拒绝；
OPTIONS 允许 `*`。请求 body 保留在原 Request 的存储中，准备头部不复制 body。

字段优先级为显式请求字段、default_headers、内置 Host/User-Agent。
同名显式字段会覆盖该名称的所有默认值，未覆盖的重复默认字段按原顺序保留。
Host 在需要时为 IPv6 加方括号，仅非默认端口附带端口号；显式 Host 允许虚拟主机，但必须是单一合法 authority。
内置 User-Agent 使用 `mhttp/<VERSION>`，VERSION 与构建清单中的库版本一致。
空 user_agent 禁用内置值；显式空 User-Agent 也会保留。默认不发送 Accept-Encoding，不推断 Content-Type。
Content-Length 按实际二进制 body 长度生成，调用方填写的旧长度、Transfer-Encoding 和 Trailer 不会用于分帧。

同一客户端只允许一个执行中的 Send；并发请求返回 Busy，不会中断先前的请求。
Keep-alive 在完整分帧且双方均允许时复用同一连接。收到 close 或读到关闭的响应后不复用。
当前只在复用连接的头部发送返回 ConnectionClosed 且已完成字节为零时重试一次，body 尚未交给发送操作。
头部已有完成前缀、body 发送失败、读失败或超时都不会重发；空闲对端关闭的主动检测待补充。
Stop 可由任意线程调用，立即清除 IsOpen 的已知状态，投递取消 DNS / socket 操作；在途请求返回 Canceled。
Stop 前签发但尚未执行的 Task 同样被取消；之后新签发的请求可重新连接。
Runtime 停止会关闭空闲及在途连接，Join 排空后重启 Runtime，可继续使用原客户端。

响应支持 Content-Length、chunked 和读到关闭三种分帧；HEAD、204、304 不读取 body。
Content-Length 或 chunked 尚未完成就断开时返回 ConnectionClosed。
已知 gzip、deflate、br、zstd Content-Encoding 返回 UnsupportedContentEncoding，未知编码原样保留。
接收到的 HTTP 错误状态保持正常 Response；只有传输、语法、上限、取消等失败才作为 Result 错误。

`tests/client/message_test.cpp` 覆盖所有权、二进制与重复字段、修改拒绝、移动和响应只读访问器。
`tests/client/request_policy_test.cpp` 覆盖配置校验、构造 host、查询拼接、默认头优先级、分帧和发送边界。
`tests/client/client_basic_test.cpp` 通过真实服务端验证常用方法、默认字段、端点、4 MiB 二进制往返、
Busy、四个客户端的跨线程 Stop、读超时、空闲/在途 Runtime 取消与重启，以及消息在网络状态销毁后的所有权。
