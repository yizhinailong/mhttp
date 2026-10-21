# 服务端接口基础

`server::Request`、`server::Response`、`server::RequestData` 和 `server::Router`
通过 `import mhttp;` 使用。HttpServer 在借用的 Runtime 上提供异步监听器与 HTTP 会话。

## 路由注册

Router 提供 `Get`、`Head`、`Post`、`Put`、`Patch`、`Delete`、`Options`
和 `Route(Method, path, handler, aspects...)`。例如：

```cpp
mhttp::server::Router router;
auto registered = router.Get("/users/:id", [](auto& request, auto& response) {
    auto id = request.template PathParam<unsigned>("id");
    if (!id) {
        static_cast<void>(response.SetError(mhttp::Status::BadRequest));
        return;
    }
    static_cast<void>(response.SetContent(std::to_string(*id), "text/plain"));
});
```

处理器与可选 aspect 被复制或移动到稳定存储，支持只可移动的对象。
同一模式与方法再次注册会覆盖旧处理器并及时释放其资源，不改变匹配优先级，
也不影响相同路径的其他方法。非法模式或方法返回 InvalidRequest，保留已有注册。
构造参数 RouterOptions 可以限制正则表达式源码与被匹配段的字节数；
路径模式和解码规则见[路由匹配核心](routing_core.md)。

路径命中但方法未注册时生成带 Allow 的 405，路径未命中时生成 404。
Allow 排序去重，注册 GET 自动包含 HEAD。HEAD 优先选择显式处理器，再回退到 GET；
内部响应保留表示内容，协议序列化器抑制 HEAD body 的发送。

Router 不可复制或移动，配置具有唯一身份。服务器启动前冻结配置，
冻结期间注册返回 AlreadyRunning。内部独立分派会在 Task 初始挂起前持有配置使用权，
未完成分派期间注册返回 Busy；放弃未开始的 Task、完成或取消都会释放使用权。
会话应持有准备结果直到响应写完；所有使用权释放后才能解冻。
已发出的操作独立拥有内部状态，不依赖 Router 包装对象继续存活；
Request、Response 和任何引用捕获仍必须活到操作结束。
配置同步不保护业务状态，同一处理器可以在多个请求上并发运行。

## 请求

Request 独立拥有请求头、target、查询、端点、路径参数、body 和 trailer，
不借用解析器输入缓冲或 socket。它只可移动，复制被禁用。
会话传给 handler 的 Request 引用必须保持有效到整个异步 handler 链结束，
不要移走这个借用对象，也不要把它的引用或视图留到请求结束之后。

`Method()` 返回已知方法的 optional，未知扩展方法返回空值；
`MethodString()` 始终保留原始、区分大小写的方法 token。
`Target()` 是解析器规范化后的编码 target，包含原始查询；绝对形式输入由解析器转为 origin-form。
`Path()` 是不含查询的解码路径，路径中的 `+` 是字面量。
路由匹配使用内部保留的编码路径先切分再解码，不能重新切分 `Path()`，否则会丢失 `%2F` 的边界。
`Query()` 保持多值顺序，并把查询中的 `+` 解码为空格。

`Headers()` 只读，`Header(name, fallback)` 大小写无关，`ContentType()` 保留完整字段值及参数。
`Body()` 返回拥有数据的二进制字符串；`Trailers()` 单独提供解析器已过滤的 trailer，
不会把它们合并进普通头部。`RemoteEndpoint()`、`LocalEndpoint()` 返回拥有地址与端口的值。
`Version()` 和 `KeepAlive()` 描述原始请求语义；服务器停止、响应头及最大请求次数还会影响实际复用。

`Form() -> Result<QueryParams>` 按需解析 application/x-www-form-urlencoded body，
媒体类型大小写无关并允许 charset 等参数；参数不会触发字符转码。
每次调用返回独立拥有的字段集合，可以在 Request 销毁后继续使用，不改变 Body 或 URL 的 Query。
缺少或不匹配 Content-Type 返回 InvalidRequest，重复 Content-Type 返回 InvalidHeaders，
应用可以选择返回 400 或 415。空的合法表单返回空集合。
表单沿用 QueryParams 的重复字段、空值、二进制 percent 解码和宽容转义规则，但开头的 `?`
作为字段名保留。独立解析表单可用 QueryParams::ParseForm；QueryParams::Parse 继续接受查询的 `?` 前缀。
QueryParams::Encode 可生成客户端上传的表单字符串，空格编码为 `+`，字面量 `+` 编码为 `%2B`。
网络会话先执行原有 body 大小限制，再向 handler 提供表单访问器。

`PathParam(name, fallback)` 借用解码后的捕获，空串与 NUL 都能保留。
`PathParam<T>(name)` 支持数字、bool、string 和 string_view，返回 optional；
缺失、非法数字、溢出及非有限浮点数返回空值。数字要求消费全部字符，不接受前导 `+`。
string 返回独立副本，string_view 继续借用 Request；空字符串捕获与缺失可以区分。

## 认证中间件

`server::BasicAuth::Create(username, password, realm)` 与
`server::BearerAuth::Create(token, realm)` 返回 Result 中的认证 aspect，复制并拥有凭据及 realm。
也可传入自定义 Validator：Basic 接收解码后的用户名和密码，Bearer 接收 token。
校验器由 aspect 拥有，借用参数只在调用期间有效；校验器必须快速返回，并自行同步共享状态。
创建时拒绝空校验器、Basic 用户名中的冒号、非法 Bearer token，以及会造成头部注入的 realm。
realm 中的引号和反斜杠会正确转义；Basic 的密码允许包含冒号。

将认证 aspect 传给路由注册函数，或通过 `Use` 应用到全部匹配路由。
scheme 大小写无关；缺失、重复、错误 scheme、格式错误或校验失败均短路为 401，
带相应的 WWW-Authenticate 挑战，受保护 handler 不会执行。
Basic 要求规范 Base64 和用户名/密码分隔符；Bearer 验证 RFC 6750 的 token 字符规则。
合法凭据不改变已有响应。校验器抛出的异常走现有异常处理器。
客户端的 SetBasicAuth / SetBearerToken 与这两个中间件配套使用。

```cpp
auto auth = mhttp::server::BasicAuth::Create("user", "password", "REST API");
server.Use(std::move(*auth)).value();
```

## CORS

`server::Cors::Create(CorsOptions)` 验证并拥有配置，再通过 `Use` 安装全局 aspect，
也可作为单路由 aspect。全局 CORS 应放在认证之前，让 OPTIONS 预检先完成。
默认允许任意有效 Origin，支持常用方法及 Origin、Content-Type、Accept、Authorization 请求头，
不允许 credentials，不设置预检缓存时长。

origins 可配置精确序列化 origin 列表（含 `null`），或单独的 `*`；空列表不允许任何 origin。
origin 不接受路径、查询或 fragment。多个允许值只回显请求中匹配的一个值。
credentials 启用后输出 Allow-Credentials: true，并回显允许的 origin；即使配置 `*` 也不输出
Allow-Origin: *。expose_headers 指定可供浏览器脚本读取的响应头；启用 credentials 时必须指定具体名称。

预检需同时满足 OPTIONS、Origin 和 Access-Control-Request-Method。
methods 大小写敏感；headers 大小写无关。有效且被允许的预检返回空的 204，跳过后续 Before 与 handler；
格式错误返回 400，origin、方法或请求头未被允许时返回 403。
Access-Control-Request-Headers 可跨多个字段组合，Authorization 需要显式列在 headers 中。
methods 中的 `*` 允许任意方法；headers 中的 `*` 允许 Authorization 以外的任意请求头，
响应将展开为具体请求的方法或头名。max_age 为非负秒数，正数生成 Access-Control-Max-Age。
普通 OPTIONS 按原有路由处理，未允许的普通跨源请求继续处理但不授予 CORS 权限。

缺少 Origin 的响应不加 CORS 权限字段。所有经过 CORS 的响应均合并 Vary: Origin，
预检另按请求的方法和头名变化；已有 Vary 字段与通配符保留，token 不重复添加。
After 在 handler 或认证短路之后重新应用 origin 权限，因此普通 401/404 仍有对应的 CORS 字段。
行为依据 [Fetch 标准的 CORS 协议](https://fetch.spec.whatwg.org/#cors-protocol)。

```cpp
auto cors = mhttp::server::Cors::Create({
    .origins = { "https://app.example" },
    .credentials = true,
});
auto auth = mhttp::server::BearerAuth::Create("token");
server.Use(std::move(*cors), std::move(*auth)).value();
```

## 响应

Response 默认是空的 200 OK，只可移动，内部拥有状态、按顺序保存的头部和二进制 body。
会话提供的 Response 也应保持为有效借用对象，不能移走或保留到 handler 链之外。
通过 `Headers().Set/Add/Erase` 修改头部，必须处理校验返回的 Result；
`Header()` 提供大小写无关的只读查询。状态及内容辅助函数也返回 Result。

`SetStatus` 接受三位状态码，包括扩展状态，不改变内容。
`SetContent(body, content_type)` 拥有 body，保留当前状态；空 content_type 删除该字段。
`SetEmpty(status)` 清空 body 并删除 Content-Type。
`SetError(status)` 接受 400 至 599，生成如 `404 Not Found\n` 的纯文本，
未知原因短语使用共享的 `Unknown`，不会把异常消息放进响应。
`SetRedirect(url, status)` 默认 302，接受除 304 外的 3xx，复制非空 Location，
再清空 body 和 Content-Type；相对引用与绝对引用均可使用，不解码或改写引用字节。
这些操作保留无关头部；无效状态、空重定向或非法字段值被拒绝时，先前响应保持不变。

body 与状态只描述表示内容。协议序列化器会按实际 body 大小重算分帧头，
忽略 handler 自行填写的 Content-Length、Transfer-Encoding 等旧分帧值。
HEAD 发送与 GET 相同的表示头但不发送 body；1xx、204、304 同样按协议抑制 body 和分帧头。
内部 `ServerResponseAccess::Head` 返回独立的协议元数据快照，后续响应修改不会改变它。

## 服务端配置与响应准备

ServerOptions 定义会话的读写超时、keep-alive 空闲超时和最大请求数，
以及 URI、头部字节数、字段数、请求 body 和整份响应的大小上限。
默认请求限制沿用协议核心；整份缓冲响应默认最多 16 MiB，含实际发送的头部和 body。
超时和除 body 外的上限必须为正，body 上限可以设为零以拒绝有内容的请求。
内部选项校验会在建立服务器资源前拒绝无效值和无法安全转换的超时；监听器接入尚待完成。

default_headers 保存回退响应头，应用显式设置同名字段时，整组默认值被覆盖；
否则保留该默认字段的全部重复值。Date 缺失时自动按 GMT 补齐。
响应准备仅修改独立的头部快照，不向应用 Response 回写自动字段。

内部 `serialize_server_response` 根据请求版本生成一份拥有完整 wire 字节的缓冲，
在连接开始写入前检查输出上限；HEAD、204、304 的 body 不进入这份缓冲。
请求/响应 Connection、未读 body、停机状态、异常关闭策略和最大请求数共同决定是否继续复用；
计数包含当前请求，达到上限的这次响应带关闭策略。
HTTP/1.0 请求显式允许复用且应用未要求关闭时，响应也明确发送 keep-alive。
1xx 不能单独作为最终缓冲响应，临时 100 响应由后续会话逻辑单独发送。

## 上下文与处理器

`Request::Data()` 提供当前请求的 RequestData。`Store(value)` 和 `Store(tag, value)`
复制或移动可复制值；`Fetch<T>(tag)` 返回独立副本或空值，重复读取不消耗存储。
`Has<T>(tag)` 仅检查瞬时存在性，需要值时直接 Fetch；`Erase<T>(tag)` 返回是否删除。
键按去掉 cv/ref 后的类型和区分大小写的 tag 区分，tag 拥有完整字节，包括 NUL。
每次操作独立同步，Fetch 的快照在并发替换或删除后仍有效。
用户对象的复制和析构在容器锁外运行，允许重入；销毁容器前必须结束所有访问。

内部 `make_http_handler` 拥有同步 `void(Request&, Response&)` 或
协程 `Task<void>(Request&, Response&)` handler，也支持只可移动的函数对象和 aspect。
aspect 可提供 Before、After 或两者，返回 bool 或 Task<bool>。
执行顺序为所有 Before、handler、所有 After，Before 和 After 各自都按注册顺序执行。
Before 返回 false 时跳过后续 Before 与 handler，保留已写入的响应，然后执行全部已注册 After，
包括 Before 未被调用的 aspect。After 返回 false 时跳过余下 After。
内部适配器将异常原样向外传播并跳过后续 aspect。
Router 的处理边界生成安全的异常响应并通知会话关闭；等待被取消时抛出的异常继续向上传播。

适配器返回的 Task 在初始挂起前就持有稳定的回调状态，所以可以先销毁适配器再等待 Task。
丢弃未开始的 Task、正常完成或取消都会释放状态；引用捕获仍要求其对象活到整个 Task 结束。
不同请求可能同时调用同一回调对象，应用共享状态需要自行同步。
`Router::Use(aspects...)` 依次追加全局 aspect，对先前和之后注册的路由均生效，
也包围默认 404/405 响应。顺序是全局 Before、路由 Before、handler、路由 After、全局 After。
全局 Before false 跳过余下全局 Before 和整个路由层，再执行全部全局 After；
路由 Before false 仍会执行路由 After 与全局 After。
路由 After false 仅跳过余下路由 After，之后仍执行全局 After。
任意阶段抛异常都会跳过所有后续 aspect，包括外层 After。
Use 遵循与路由注册相同的冻结和配置使用权守卫，传入的一批 aspect 作为一次配置变更发布。

## 路由钩子

`SetPreRoutingHandler` 接受返回 bool 或 Task<bool> 的处理器，在路径匹配和读取 body 前调用，
此时没有路径捕获。返回 true 表示已处理请求，跳过匹配、body 读取及全部全局/路由 aspect，
保留处理器写入的响应；返回 false 继续路由。若提前处理留下未读 body，会话必须关闭连接。
这与 aspect Before 的 true 表示继续有不同含义。

`SetNotFoundHandler(handler, aspects...)` 接受与普通路由相同的处理器和可选 aspect。
正常路由执行结果为 404 时调用一次，包括路径未命中和 handler 自己生成的 404；
它的 aspect 在一次全局 Before/After 之间执行。默认 404 状态已设置，SetContent 保留此状态。
405 和 pre-routing 已处理的请求跳过这个处理器。

`SetErrorHandler` 在最终状态至少为 400 时调用一次，位置在全局 After 之后；
它可以修改状态、头部和 body，修改状态不会递归调用错误处理器。
路由 404/405、SetError、认证拒绝、协议拒绝与异常回退默认使用同一纯文本 code/reason 格式，
不包含输入字节或异常诊断。应用可通过这个钩子统一改写，包括使用[可选 JSON 接口](json.md)
生成错误对象；只改写 body 时仍保留原状态、Allow 和 WWW-Authenticate。
`SetPostRoutingHandler` 在错误处理器之后调用一次，可给正常、错误和提前处理的响应统一加头。
二者支持同步 void 和 Task<void>，序列化在所有响应修改完成后重新计算分帧。

`SetExceptionHandler` 接受同步 void 或 Task<void> 回调，参数为 Request&、Response&
和按值传入的 `std::exception_ptr`。路由、aspect、pre-routing、自定义 404 或错误钩子抛异常时，
Router 清除未完成的响应，先设置纯文本 500，再调用异常钩子。
可以用 `std::rethrow_exception` 取得原异常并自定义响应；改成成功状态也仍须关闭连接。
默认响应不会把异常消息放入头部或 body。

异常钩子每个响应最多调用一次，它自己失败时恢复安全的 500，不会递归调用。
随后错误钩子与 post-routing 各最多运行一次；错误钩子自己的失败同样被隔离，
已经调用过的异常/错误钩子不会重入。post-routing 失败则要求关闭连接而不写回部分响应。
Error::Canceled 保持为取消控制流，不转成 500，也不会继续调用尚未进入的响应钩子。
内部会话执行响应分帧、关闭连接与实际网络写入。

内部准备结果保存配置使用权及准备异常，异常发生时不再读取请求 body；
处理结果 RoutingOutcome 给出是否可以发送响应、是否强制关闭，以及可供记录的异常。
会话必须遵守该关闭策略，即使应用填写了 `Connection: keep-alive`。

以上 setter 都替换先前的对应钩子，旧资源在配置锁外释放，遵循相同的冻结/使用权守卫。
钩子可拥有只可移动的对象；异步调用仍遵循 Request、Response 和引用捕获的借用生命周期约束。

## 内部 HTTP 会话

会话在连接 strand 上串行执行读头部、pre-routing 与匹配、完整 body 读取、handler 与响应钩子，
最后写回有大小上限的拥有数据的响应。同一读取中剩余的流水线字节保留到下一请求，
前一响应写完后才处理下一请求。配置使用权保留到写操作完成，公共 Router 包装器销毁后内部状态仍被持有。

首个头部和每个 body 使用 read_timeout；后续头部使用 keep_alive_timeout。
每个阶段使用整体截止时间，单个字节到达不会重新计时。请求已有字节时，读超时先取消读取，
生成 408，经错误钩子和 post-routing 改写后发送并关闭；不 dispatch handler，也不暴露未完成 body。
没有请求字节的空闲读超时及空闲 EOF 直接关闭，不生成响应；写超时关闭连接。
半关闭导致的截断请求生成 400。
非法协议、URI、头部或 body 分别写回解析器指定的状态码，再关闭连接。
头部失败跳过 pre-routing、匹配与 aspect，错误钩子与 post-routing 仍各执行一次。

未命中的 POST、PUT、PATCH、DELETE 等请求直接生成 404/405，跳过 body 并关闭，
即使声明长度为零也关闭。pre-routing 提前处理且有未读 body 时也关闭。
连接复用还受 HTTP 版本、双方 Connection 字段、最大请求数和停止状态约束。
序列化超限或非法最终 1xx 尝试改发有界 500 并关闭，不重新进入响应钩子；
连安全错误响应也超过输出上限时只关闭。

会话停止可从其他线程请求：在连接 strand 上取消用户 Task 并关闭套接字，
等待所有嵌套 Task 完成清理后报告最终状态。Run 在初始挂起前持有会话，且只允许执行一次。
私有测试通过 `python tools/check_session.py` 在临时项目中运行，不向正式消费者导出 Asio 或会话实现。
HTTP/1.1 的 `Expect: 100-continue` 在 pre-routing 与匹配通过、声明长度未超限且存在 body 时，
先发送无 body 的 100，再读取完整 body。临时响应不调用响应钩子、不计入最大请求数。
未匹配或被 pre-routing 提前处理时直接发送最终响应并关闭，不等待读取 body；
不支持的期望值由解析器返回 417，超限返回 413。HTTP/1.0 忽略 Expect。

## 监听器生命周期

`HttpServer(runtime, options, router_options)` 继承 Router。Runtime 必须活过服务端和它创建的所有 Task。
构造不启动线程；Bind 与 BindToAnyPort 解析地址、绑定并监听，Start 在已启动的 Runtime 上提交 accept 循环。
Bind 返回实际端口，LocalEndpoint 返回拥有地址数据的 optional；停止完成后返回空值。
Start 立即冻结路由并返回，重复启动和运行期修改路由返回 AlreadyRunning。
WaitUntilReady 默认最多等待五秒，未启动或已停止时立即返回 NotRunning。
启动失败会撤销冻结，可修正配置状态后重试。

```cpp
mhttp::Runtime runtime;
if (!runtime.Start(4)) { return 1; }
mhttp::server::HttpServer server{ runtime };
if (!server.Get("/hello", [](auto&, auto& response) {
    static_cast<void>(response.SetContent("hello", "text/plain"));
})) { return 1; }
auto port = server.BindToAnyPort("127.0.0.1");
if (!port || !server.Start() || !server.WaitUntilReady()) { return 1; }
// 在应用自己的非 I/O 控制线程中等待退出事件。
if (!server.Stop()) { return 1; }
```

Stop 关闭监听器并取消全部会话，阻塞到 accept、用户 Task、已开始的 Offload 和会话完成回调全部排空。
AsyncStop 在独立的控制 Task 中等待相同的排空条件；不要在该服务端自己的 handler 中等待它，
因为当前 handler 也是排空的一部分。Bind、Stop 和 WaitUntilReady 在 I/O 线程返回 Busy。
析构也会停止并等待，因此必须在非 I/O 线程销毁服务端。
Runtime::Stop 同样会取消监听器和会话，Join 会等待相关操作排空。
停止保留路由，重新 Bind/Start 无需再次注册；Runtime 停止后须先 Join，再 Start 一个新执行代。

全局 strand 独占 acceptor 与会话表；会话先登记，再启动连接 strand 上的协程。
停止只向连接 strand 投递取消，所有会话根完成后才解冻路由并通知等待方。
只 Bind 而未 Start 的套接字也能在 Runtime 启动前直接 Stop。
Listen(host, port) 组合 Bind、Start 与阻塞等待。Runtime 空闲时，它在当前线程调用 Run，
服务端排空后停止并 Join 这一次执行代；Runtime 已启动时沿用现有执行线程，返回时不停止 Runtime。
其他非 I/O 控制线程可调用 Stop 来唤醒 Listen；也可从独立控制 Task 等待 AsyncStop。
max_connections 默认 1024，必须为正数，统计包括空闲、正在关闭和等待 Offload 排空的已登记会话。
达到上限后接受到的额外套接字直接关闭，不解析请求、不执行 handler，也不发送 HTTP 响应。
会话根完成回调从全局 strand 的注册表移除条目后才释放容量；已有 keep-alive 会话继续工作。

reuse_address 默认 true，仅在 POSIX 上配置 SO_REUSEADDR，允许关闭后的本地地址复用。
Windows 使用 SO_EXCLUSIVEADDRUSE，阻止其他开启复用的监听器抢占正在使用的绑定；
该选项不会强制立即回收仍由操作系统维护的连接状态，详见
[Windows 地址复用与独占绑定](https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse)。
tcp_no_delay 默认 true，在会话启动前给每个接收套接字设置 TCP_NODELAY。
IPv6 绑定显式设置 IPV6_V6ONLY：ipv6_only 默认 false，绑定 `::` 时可同时接收 IPv6 和 IPv4 映射连接；
设为 true 则仅接收 IPv6。IPv4 绑定忽略 ipv6_only。端点保留操作系统报告的地址，包括 IPv4 映射形式。

SetAccessLogger 接受同步的 `void(Request const&, Response const&, steady_clock::duration)` 回调，
可拥有只可移动的捕获对象，传空回调可清除。启动后直到停止完成都禁止替换，返回 AlreadyRunning。
每个完整写完的最终响应回调一次，包括 404、协议错误、异常响应和输出超限后的有界 500；
100 临时响应、post-routing 失败、未成功写完的响应与取消不触发访问日志。
响应是最终应用表示，自动 Content-Length 等字段仅在序列化的线上头部中生成；HEAD 仍能观察表示 body。
耗时从首个请求字节进入解析器开始，到完整写操作完成为止，排除 keep-alive 空闲时间及日志回调自身。
引用只在同步回调期间有效，需要保留的数据必须拷贝。不同会话的回调可并发执行，应用负责同步，
并避免在 I/O 线程阻塞。观察器异常被隔离，不改写响应、不关闭原本可复用的连接。
访问日志抛出的异常通过诊断日志报告；未设置诊断回调时直接隔离。

SetLogger 接受同步的 `void(LogRecord const&)` 回调，同样支持只可移动的捕获、空回调清除，
并在启动后直到停止完成期间禁止替换。LogRecord 可拷贝并保留，拥有本地/远端 Endpoint、
可选的原始 exception_ptr，以及 Error 和可用时的原始 std::error_code。
当前传输层返回可移植 Error，因此会话记录的 native_error 为空；绑定和 socket 初始化保留原生错误。
LogEvent 区分 BindFailure、AcceptFailure、ConnectionRejected、SessionFailure 和 AccessLoggerFailure，
分别报告绑定配置/解析/监听失败、监听循环失败、容量或初始化拒绝、终止会话的故障以及访问日志异常。
BindFailure 的 local 是请求的绑定目标，其他记录尽可能携带已建立的端点。
正常空闲 EOF、显式停止取消及成功响应不产生故障诊断；空闲读取超时仍记录 ReadTimeout。
日志不改变 HTTP 错误策略，诊断回调自身抛出的异常被隔离且不递归记录。

绑定失败的回调在 Bind 调用线程执行，连接拒绝在服务端 strand，会话与访问日志在连接 strand。
所有用户日志回调均在配置锁之外调用，停止完成时监听器和会话的回调已排空；不同会话的调用可以重叠。
Bind 的同步回调由调用 Bind 的线程完成，其生命周期不依赖运行中的服务端代次。
回调应短小且避免阻塞 I/O 线程，共享状态需自行同步，借用的记录引用只在回调内有效。

对应独立测试为 `tests/server/request_test.cpp`、`response_test.cpp`、
`request_data_test.cpp`、`handler_test.cpp`、`middleware_test.cpp`、`router_test.cpp`、
`routing_hooks_test.cpp`、`exception_test.cpp` 和 `response_policy_test.cpp`，覆盖数据所有权、错误拒绝、分帧衔接、
类型化参数、并发上下文、调用顺序、短路、异常、注册替换、配置冻结、惰性所有权与取消清理。
`tools/check_server.py` 在隔离模块中运行原始回环客户端，覆盖监听器生命周期、容量、双栈、日志，
以及路由参数、编码分隔符、正则限制、404/405 与 HEAD 的完整 HTTP 响应；支持按用例名过滤。
handler 验收还覆盖全局/路由 aspect 顺序、403 短路、带 aspect 的自定义 404、pre/post-routing，
以及 10 个并发客户端各 100 次异常请求在异常处理器、错误处理器和序列化之间的数据一致性。
请求验收覆盖原始 target、有序重复查询参数、查询与二进制 body 共存、空 body 分帧、非法百分号转义，
以及与真实 TCP socket 对比的端点信息；客户端提供的地址字段仍作为普通输入保留，不能覆盖端点元数据。
响应验收覆盖空内容、标准错误、301/302/303/307 重定向、CRLF 拒绝后的状态保留、权威分帧，
以及应用明确要求关闭连接时，已缓冲的流水线后续请求不会执行。
分帧验收覆盖混合 keep-alive 请求、请求数量上限、无 Content-Length 的 DELETE 流水线、HTTP/1.0，
以及 URI/头部/body 上限和非法输入后的关闭；错误输入之后或 body 中的 HTTP 请求字节不会被重新分派。
并发验收让 50 个连接同时执行各 20 个流水线请求，并确认 500 ms 的 Offload 阻塞工作期间，
其他连接的响应仍能在 250 ms 内完成。
慢速客户端验收让请求头每 100 ms 到达一个字节，确认 1 秒整头期限不会被进度重置；
另用小接收窗口且不读取响应的客户端，确认 100 ms 写期限关闭未写完的响应，并报告 WriteTimeout。
