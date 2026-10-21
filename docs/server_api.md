# 服务端接口基础

`server::Request`、`server::Response`、`server::RequestData` 和 `server::Router`
通过 `import mhttp;` 使用。当前支持内存中的路由配置与分派；监听器和 HTTP 会话仍待接入。

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

`PathParam(name, fallback)` 借用解码后的捕获，空串与 NUL 都能保留。
`PathParam<T>(name)` 支持数字、bool、string 和 string_view，返回 optional；
缺失、非法数字、溢出及非有限浮点数返回空值。数字要求消费全部字符，不接受前导 `+`。
string 返回独立副本，string_view 继续借用 Request；空字符串捕获与缺失可以区分。

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
异常原样传播并跳过后续阶段，包括等待被取消时抛出的异常；500 响应由后续会话层处理。

适配器返回的 Task 在初始挂起前就持有稳定的回调状态，所以可以先销毁适配器再等待 Task。
丢弃未开始的 Task、正常完成或取消都会释放状态；引用捕获仍要求其对象活到整个 Task 结束。
不同请求可能同时调用同一回调对象，应用共享状态需要自行同步。
Router 注册接口已接入路由级 aspect；全局 aspect 和路由钩子待后续组合。

对应独立测试为 `tests/server/request_test.cpp`、`response_test.cpp`、
`request_data_test.cpp`、`handler_test.cpp` 和 `router_test.cpp`，覆盖数据所有权、错误拒绝、分帧衔接、
类型化参数、并发上下文、调用顺序、短路、异常、注册替换、配置冻结、惰性所有权与取消清理。
