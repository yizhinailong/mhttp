# 客户端接口

客户端的请求与响应类型通过 `import mhttp;` 使用，公共接口只包含标准库和 mhttp 类型。
当前已提供数据模型、ClientOptions 和不进行 I/O 的发送准备规则；HttpClient、连接管理和异步 Send 待接入。

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
这些配置会随客户端网络层接入后应用到实际 I/O，目前发送准备层已校验配置和完整发送大小。

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

`tests/client/message_test.cpp` 覆盖所有权、二进制与重复字段、修改拒绝、移动和响应只读访问器。
`tests/client/request_policy_test.cpp` 覆盖配置校验、构造 host、查询拼接、默认头优先级、分帧和发送边界。
