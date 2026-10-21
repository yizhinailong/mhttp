# 客户端接口

客户端的请求与响应类型通过 `import mhttp;` 使用，公共接口只包含标准库和 mhttp 类型。
当前已提供数据模型；HttpClient、连接管理和异步 Send 将在后续接入。

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

`tests/client/message_test.cpp` 覆盖所有权、二进制与重复字段、修改拒绝、移动和响应只读访问器。
