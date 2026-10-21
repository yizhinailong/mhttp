# 客户端接口

客户端的请求与响应类型通过 `import mhttp;` 使用，公共接口只包含标准库和 mhttp 类型。
`client::HttpClient` 提供惰性连接和 `Send(Request) -> Task<Result<Response>>`。
构造时不进行 DNS 或 socket I/O；连接、发送与读取均异步进行。

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

Get / Head / Post / Put / Patch / Delete / Options 均提供两组快捷重载：
`(path, QueryParams = {}, Headers = {})` 和
`(path, body, content_type = {}, QueryParams = {}, Headers = {})`。
省略 path 时使用构造 URL 的路径和查询；所有参数在签发 Task 前即转入拥有数据的 Request。
content_type 只在显式 Headers 没有 Content-Type 时作为后备值，显式空值也会保留。
HEAD 可以发送请求 body，但仍不读取响应 body。

`client::ClientOptions` 规定 DNS 与所有端点尝试共用的连接超时、完整响应读取超时、
完整请求写入超时，以及响应头字节数/字段数、响应 body 和完整发送数据的上限。
所有超时和上限必须为正；响应 body 上限可以为零，表示只接受空 body。
连接超时覆盖 DNS 和 IPv4 / IPv6 端点的顺序尝试；读超时覆盖整个最终响应，含中间响应、头部和 body；
写超时覆盖请求头和完整 body 的所有部分写入。超时或失败会关闭连接，下一次请求重新连接。
timeout_policy 默认为 TimeoutPolicy::Phase，使用上述三个独立预算。
TimeoutPolicy::Overall 使用 timeout，为一次 Send 的连接、写入、读取、透明重试和全部重定向
共用一个单调时钟截止时间；新数据到达或切换连接均不重置预算。
TimeoutPolicy::Step 为连接建立（DNS 和所有端点尝试合计）、每次完整请求头写入、完整 body 写入、
每次底层读取分别重新设置 timeout，因此持续收到数据可以超过单次预算。
TimeoutPolicy::Never 关闭请求失败期限，Stop、Runtime 停止和 Task 取消仍有效。
Overall / Step 忽略独立阶段超时，Phase 忽略 timeout；所有配置的时长仍必须为正且可安全表示。
超时按发生阶段返回 ConnectionTimeout、WriteTimeout 或 ReadTimeout，并关闭当前连接。

发送准备保留已有 percent escape 和原始查询的顺序，将 QueryParams 中的键和值分别编码后追加，
移除 fragment；空目标或仅有 fragment 时采用构造 URL 的路径和查询。
发送目标中的空格、CRLF 和 NUL 等字节会编码，非法方法和非 origin-form 目标在发送前被拒绝；
OPTIONS 允许 `*`。请求 body 保留在原 Request 的存储中，准备头部不复制 body。

字段优先级为显式请求字段、default_headers、内置 Host/User-Agent。
同名显式字段会覆盖该名称的所有默认值，未覆盖的重复默认字段按原顺序保留。
SetDefaultHeaders 替换完整默认字段集合，传空集合可同时清除已配置的认证。
SetBasicAuth 设置 Basic Authorization，用户名不能含冒号，密码可以含冒号；
SetBearerToken 设置 Bearer Authorization，拒绝空值、空白、控制字节和不合法的 token 字符。
这三个配置接口可从任意线程调用，在失败时保留原配置。Send 和快捷方法在签发 Task 时
复制默认字段快照，后续配置更新只影响之后签发的 Task；显式 Authorization 仍优先。
Host 在需要时为 IPv6 加方括号，仅非默认端口附带端口号；显式 Host 允许虚拟主机，但必须是单一合法 authority。
内置 User-Agent 使用 `mhttp/<VERSION>`，VERSION 与构建清单中的库版本一致。
空 user_agent 禁用内置值；显式空 User-Agent 也会保留。默认不发送 Accept-Encoding，不推断 Content-Type。
Content-Length 按实际二进制 body 长度生成，调用方填写的旧长度、Transfer-Encoding 和 Trailer 不会用于分帧。

同一客户端只允许一个执行中的 Send；并发请求返回 Busy，不会中断先前的请求。
Keep-alive 在完整分帧且双方均允许时复用同一连接。收到 close 或读到关闭的响应后不复用。
空闲连接通过异步可读等待检测 FIN / reset 或非预期数据。下一次请求先取消并排空等待，再使用连接，
等待不会消耗响应字节；已检测到关闭则直接重新连接。
复用连接的头部发送返回 ConnectionClosed 且已完成字节为零时只重试一次，body 尚未交给发送操作。
头部已有完成前缀、body 发送失败、读失败或超时都不会重发。
Stop 可由任意线程调用，立即清除 IsOpen 的已知状态，投递取消 DNS / socket 操作；在途请求返回 Canceled。
Stop 前签发但尚未执行的 Task 同样被取消；之后新签发的请求可重新连接。
Runtime 停止会关闭空闲及在途连接，Join 排空后重启 Runtime，可继续使用原客户端。

max_redirects 默认为零，直接返回原始 3xx 响应。设置为正数后跟随 301、302、303、307、308，
最多跟随指定次数，仍需跳转时返回 ExceedRedirectCount。304 或缺少非空 Location 的响应原样返回，
跟随时拒绝多个 Location；跳转到 HTTPS 返回 UnsupportedScheme。
Location 按实际发送 URL 解析相对路径，保留 percent escape、路径和查询中的 `+`，不发送 fragment，
不重复追加原请求的 QueryParams。303 改为 GET；301/302 除 HEAD 外改为 GET，并移除 body、
Content-Type、Content-Encoding 和 Expect；307/308 保留方法及完整二进制 body。
跟随时完整读取各跳响应并执行相同的输入上限和阶段超时，最终只返回最后一个响应。
启用重定向时会在发送期间保留请求 body，以便后续跳转重新发送。

同源跳转保留 Cookie 和 Authorization；跨源时关闭原连接，重新生成 Host，
并从显式字段和默认字段快照中移除 Authorization、Proxy-Authorization 和 Cookie。
后续跳回原源也不恢复凭据；这些修改不影响客户端配置或之后独立签发的 Task。
Host / Port 始终报告构造时的目的地，新 Send 从该目的地开始，不会误用最后一跳的跨源连接。

响应支持 Content-Length、chunked 和读到关闭三种分帧；HEAD、204、304 不读取 body。
Content-Length 或 chunked 尚未完成就断开时返回 ConnectionClosed。
已知 gzip、deflate、br、zstd Content-Encoding 返回 UnsupportedContentEncoding，未知编码原样保留。
接收到的 HTTP 错误状态保持正常 Response；只有传输、语法、上限、取消等失败才作为 Result 错误。

## Expect 上传握手

大请求可以先发头部，等待 `100 Continue` 后再上传 body。显式请求字段和 default_headers
中的 `Expect: 100-continue` 均会启用握手；字段名与 token 大小写无关，也接受重复的已知 token。
不支持的 token、空字段或不完整列表返回 InvalidHeaders；对空 body 显式声明此期望返回
InvalidRequest，均在连接前失败。

可用 `ClientOptions::expect_continue_threshold` 自动为达到字节阈值的非空 body 添加该字段。
阈值默认是 0，关闭自动添加；已有显式或默认 Expect 字段优先。自动字段也计入完整请求大小上限。

```cpp
mhttp::client::ClientOptions options;
options.expect_continue_threshold = 1024 * 1024;
options.expect_continue_timeout = std::chrono::milliseconds{ 500 };
mhttp::client::HttpClient client{ runtime, "http://127.0.0.1:8080", options };
auto task = client.Post("/upload", owned_binary_body, "application/octet-stream");
```

`expect_continue_timeout` 是正数，默认值见 ClientOptions 声明。100 的完整头部解析成功后
只发送一次 body；103 等其他中间响应不会放行。对端没有及时返回 100 时，在有限等待后
直接上传。该等待是协商回退，即使 TimeoutPolicy::Never 也生效；到期本身不作为请求失败。
等待使用不消费字节的可读性操作，到期取消后排空，保留所有未读响应字节。
回退后晚到或重复的 100 只作为中间响应处理，不会再次上传。中间响应仍受个数、字段和大小限制，
不支持的 101 协议升级返回 ProtocolError。

如果先收到完整的最终响应头，客户端不上传 body，继续读取完整最终响应并关闭连接。
Response::KeepAlive() 因此为 false，即使原始响应字段声明 keep-alive；下一次请求建立新连接。
417 同样作为普通 HTTP 响应返回，不自动重发业务请求。

握手受 read_timeout / Overall / Step 的较早期限约束。Phase 模式下，握手等待不消耗 body
写入预算：头部写入、握手读取、body 写入和最终响应读取分别有阶段预算；普通请求仍共用头部和
body 的写入预算。Step 模式下，一次握手读取的可读性等待与实际读取共用该次读取预算；
Overall 在上传、回退和重定向之间始终使用同一截止时间。
提前 307 / 308 保留尚未上传的 body，目标连接重新协商；100 后再收到这类重定向时使用保留的
拥有数据副本。303 和相应的 301 / 302 改写会清除 body 与 Expect。

握手语义参考 [RFC 9110 §10.1.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-10.1.1)。

## 验证

`tests/client/message_test.cpp` 覆盖所有权、二进制与重复字段、修改拒绝、移动和响应只读访问器。
`tests/client/request_policy_test.cpp` 覆盖配置校验、构造 host、查询拼接、默认头优先级、分帧和发送边界。
`tests/client/auth_test.cpp` 通过真实回环通信验证认证格式、默认字段替换、显式覆盖、非法输入拒绝和跨线程快照。
`tests/client/redirect_test.cpp` 覆盖方法/body 改写、关闭跟随、次数上限、相对和绝对 Location、
编码查询、同源 Cookie、跨端口凭据移除、返回原源、跨源 body 上限和读超时。
`tests/client/timeout_test.cpp` 验证跨源重定向共用整体预算，以及禁用计时后的显式/运行时取消和清理。
`tests/client/client_basic_test.cpp` 通过真实服务端验证常用方法、默认字段、端点、4 MiB 二进制往返、
Busy、四个客户端的跨线程 Stop、读超时、空闲/在途 Runtime 取消与重启，以及消息在网络状态销毁后的所有权。
`python tools/check_client.py` 使用隔离工程中的原始 TCP 对端验证 CL / chunked / EOF、trailer、
HEAD / 204 / 304 / 404、截断、字段和 body 上限、编码错误、连接拒绝、真实 body 读超时和上传写超时。
它还在 POST 请求头部分写入后主动 reset，并确认没有额外连接或请求重发。
持续慢速 CL、EOF 和 chunked 响应验证两秒整体截止时间，另覆盖 Step 进度续时、停顿超时、
Never 延迟响应和整体上传超时。
同一隔离测试还先探测本机 IPv6 回环能力，再验证裸地址、方括号地址和 URL 构造的真实 IPv6 通信，
以及带方括号和动态端口的 Host。只有地址族或回环地址不可用时跳过。
`tests/client/error_test.cpp` 验证保留 `.invalid` 域名返回 ResolveFailed；该用例需要系统 DNS 解析器
及时返回保留域名的失败结果，不连接任何 HTTP 服务。
连接超时的 SYN 丢弃场景默认跳过，可设置 MHTTP_TEST_CONNECT_TIMEOUT_URL 指向测试环境管理的
静默丢弃连接尝试的 HTTP 端点后运行；立即拒绝连接的端点不能用于此用例。
`tests/client/keep_alive_test.cpp` 验证空闲关闭后的透明重连、200 次同连接复用、服务端每连接请求上限、
禁用复用、Stop 世代竞争及销毁客户端时的空闲等待排空。
`tests/integration/expect_test.cpp` 验证二进制握手往返、连接复用、404 / 413 的提前拒绝，
以及提前和上传后 307、303 的 body 所有权。隔离客户端验收还用独立 TCP 对端检查 103 与
分段 100 之前没有上传字节、有限等待回退、晚到 100 不重复上传、提前最终响应后的 EOF 与新连接、
读写及整体期限、客户端和 Runtime 取消、异常中间响应，以及握手前断连。

## 流式接收与进度

`Send(Request, TransferCallbacks)` 在同一个单次发送和重定向操作中拥有全部回调，逐个等待完成。
`on_headers(Response const&)` 在最终响应头通过校验后运行，借用的快照仅在该次等待期间有效，
此时 Body/Trailers 为空且 KeepAlive 为 false；最终返回的 Response 才具有完整 trailer 与复用状态。
`on_body(std::string)` 收到独立拥有的非空二进制块，最多 `response_chunk_bytes` 字节。
设置 on_body 后不累计正文，最终 Response::Body() 为空；省略它时保留完整缓冲行为。

```cpp
mhttp::client::TransferCallbacks callbacks;
callbacks.on_body = [](std::string chunk) -> mhttp::Task<mhttp::Result<void>> {
    // 逐块处理；耗时业务可交给 Offload。
    co_return mhttp::Result<void>{};
};
auto response = co_await client.Send(mhttp::client::Request{ mhttp::Method::Get, "/large" }, std::move(callbacks));
```

`on_progress(TransferProgress)` 接收初始、每块成功传输后及完整结束的样本。
`direction` 区分 Upload/Download，`transferred` 只计解码后的应用字节，`total` 对固定长度为已知值，
对 chunked/EOF 为 nullopt，`complete` 表示正文与分帧均已完成。最后一块与完成样本的字节数可以相同。
上传按 `upload_chunk_bytes` 划分进度段；`hop` 是从零开始的重定向序号，每跳重新统计上传。
自动跟随的中间响应不调用 headers/body/download 回调；只有最终响应交给接收器。

所有回调返回 `Task<Result<void>>`，可用 Sleep/Offload，在客户端 strand 上串行运行。
返回错误会中止传输，异常转换为 Unknown；未完成的连接关闭，后续 Send 可重连。
同一客户端的回调内再次 Send 返回 Busy。Stop、Runtime 取消及调用者取消会取消等待并清理全部嵌套 Task。
选定的 Phase/Overall/Step/Never 策略也约束回调：下载回调超时为 ReadTimeout，上传进度超时为 WriteTimeout。
已运行的 Offload 函数仍须执行完毕后才报告取消。回调捕获的对象及 Runtime 必须存活至整个 Send 完成。
总响应正文仍受 `body_bytes` 限制，trailer 与初始字段共同受头部限制；流式读取不会放宽协议校验。

## Provider 上传

`Request::SetContentProvider(length, provider, content_type)` 拥有异步上传来源并清空 Body()。
`length` 为解码后的精确长度，nullopt 表示未知长度，使用 HTTP/1.1 chunked 分帧。
`IsStreaming()` 与 `ContentLength()` 可在不调用来源的情况下检查模式与长度；SetContent/SetMultipart/SetJson
会清除 provider。空回调及非法媒体类型被拒绝并保留原状态。

provider 的签名为 `Task<Result<std::string>>(std::uint64_t offset, std::size_t maximum)`，
每次返回独立拥有的二进制块，大小不超过正值 maximum；空字符串表示 EOF。
固定长度来源必须提供精确数量，提前 EOF 返回 InvalidRequest，到达声明长度后不会再调用来源。
未知长度来源持续读取到 EOF，并在全部数据写出后发送终止块。

```cpp
mhttp::client::Request request{ mhttp::Method::Post, "/upload" };
request.SetContentProvider(std::nullopt,
    [owned = std::make_unique<std::string>("payload")](std::uint64_t offset, std::size_t maximum)
        -> mhttp::Task<mhttp::Result<std::string>> {
        co_return owned->substr(static_cast<std::size_t>(offset), maximum);
    });
auto response = co_await client.Send(std::move(request));
```

来源只会在请求头成功写出、且 Expect 允许上传或达到回退时限后执行。
提前最终响应不会调用 provider 或上传进度回调。零字节请求头失败的透明重试也不消耗来源。
307/308 保留来源并从 offset 零重新调用；因此启用自动重定向时，来源必须按偏移量支持重放。
303 及需要改为 GET 的 301/302 会清除来源、正文及 Expect。无法重放的单次来源应关闭自动跟随。

`upload_body_bytes` 限制解码后的上传总量，默认 64 MiB，也适用于缓冲正文；零只允许空上传。
已知长度超限在网络操作前拒绝，未知长度逐块检查，总量恰好达限后仍允许来源返回 EOF。
`upload_chunk_bytes` 限制每次请求的原始块大小；`max_request_bytes` 限制完整缓冲请求，
或 provider 的请求头及单个编码后写入。客户端会为十六进制长度和 CRLF 预留空间，保证每块分帧都能容纳。
这些限制独立生效，来源返回超出请求大小或总量的块时返回 BodyTooLarge，并关闭未完成连接。

来源按顺序等待完成，沿用上传阶段的超时策略和进度语义，可使用 Offload 处理阻塞来源。
返回错误、抛出异常、取消或超时均会清理嵌套 Task 后返回；异常映射为 Unknown，写预算耗尽为 WriteTimeout。
不得捕获已经离开作用域的 handler 局部引用；Runtime 和捕获的外部对象必须存活到 Send 完成。

`tests/integration/client_stream_test.cpp` 覆盖首块提前送达、拥有块、进度、重定向过滤、回调异常/超时，
以及客户端、Runtime 和调用者会话的取消传播。
`python tools/check_client.py streamed` 使用原始响应覆盖固定长度/chunked/EOF、HEAD/204/304、
二进制、trailer、截断、总量限制及逐次回调的超时策略。

`tests/integration/client_provider_test.cpp` 覆盖固定/未知长度、空正文、二进制、精确编码边界、
307/308 重放与 303 清除、Expect 提前拒绝、大小限制、四种超时策略及停止清理。
`python tools/check_client.py provider` 校验请求头失败前不调用来源、单次透明重试预算，
慢接收端的传输背压、不发送 100 的对端触发有界回退，以及正文首块后真实 reset 时停止来源并清理协程。
启用自动 Expect 时，未知长度来源始终进行协商；声明长度零不会自动协商，也不允许显式 Expect。
