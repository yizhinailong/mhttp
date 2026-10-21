# HTTP 协议核心

协议核心不依赖 Asio，通过 `import mhttp;` 后可在 `mhttp::detail` 中访问。
这些接口用于库内实现与协议测试，使用 Doxygen `@internal` 标注。
对外的服务端、客户端和网络连接将在后续阶段建立。

## 增量解析

`RequestParser` 和 `ResponseParser` 每次通过 `Feed(std::span<char const>)`
读取一个前缀，并返回 `ParseStep`。调用方必须按 `consumed` 推进输入位置。
解析器拥有起始行、头部和 trailer；body 以借用的 span 返回，解析器不存储完整 body，
也不会根据 Content-Length 预分配 body 空间。

| 事件 | 调用方的处理 |
| --- | --- |
| `NeedMore` | 当前输入已耗尽，读取更多字节；收到 EOF 时调用 `Finish()`。 |
| `HeadersComplete` | 通过 `Head()`、`Framing()` 读取最终头部与分帧信息，再处理剩余输入。 |
| `Body` | 消费 `data`；消费完成前保持原始输入缓冲区存活且内容不变。 |
| `Complete` | 当前消息结束；保留尚未消费的字节供下一条消息使用。 |
| `Failed` | 读取 `failure`，停止使用此连接。服务端可用其中的 `status` 生成错误响应。 |

一次 Feed 最多产出一个事件；处理事件后，需要继续传入剩余输入。
定长 body 的最后一个 `Body` 事件之后，即使没有剩余输入，也要调用 `Feed({})`
以取得 `Complete`。`Complete` 和 `Failed` 均为稳定的终态，后续 Feed 不会消费字节。

成功完成后可调用 `Reset()`，再把保留的字节交给同一个解析器，支持 pipelining。
失败后不得通过 Reset 恢复连接，因为后续字节的消息边界已经不可信。
关闭连接的实际操作由后续会话层负责。

响应解析器默认按 GET 的响应语义工作。解析 HEAD 的响应时，使用
`ResponseParser{ Method::Head, limits }`。HEAD、204 和 304 在头部结束；
1xx 中间响应被跳过，并受 `interim_responses` 次数上限约束。
没有分帧头的普通响应只有在 `Finish()` 收到 EOF 后才会完成，且不能复用连接。
定长或 chunked body 提前 EOF 返回 `Error::ConnectionClosed`。

`ParserLimits` 限制 URI、起始行、单行头部、头部总字节、字段数、解码 body、
chunk 大小行和中间响应次数。具体默认值见类型声明。
头部总字节包含字段行的 CRLF、头部结束的 CRLF 和 trailer；不包含起始行及 chunk 大小行。
被丢弃的禁止 trailer 也计入字段数和字节数。合法 trailer 只存入 `Trailers()`，
不会改变初始头部和分帧决策。

## 生成头部与 body

`serialize_request_head` 和 `serialize_response_head` 返回拥有头部字符串的
`Result<SerializedHead>`，本身没有网络副作用。调用方在取得成功结果之后才能写出头部。
`SerializeOptions::body_bytes` 为实际 body 大小；序列化器忽略调用方的
Content-Length 和 Transfer-Encoding，生成唯一有效的分帧头。

`SerializedHead::send_body` 决定能否写出 body；`body_kind` 指示所声明的分帧方式，
`keep_alive` 给出最终连接策略。HEAD 的头部与 GET 一致，但 `send_body` 为 false。
1xx、204 和 304 不输出 body、Content-Length、Transfer-Encoding 或 Trailer。
空的普通 body 输出 Content-Length: 0，不自动添加 Content-Type。

HTTP/1.1 可以通过 `chunked` 选择分块；此时 body 大小可以在发送时才确定。
HTTP/1.0 始终根据 `body_bytes` 输出 Content-Length，因此调用方必须提前知道完整大小。
未知大小的 HTTP/1.0 流式发送留待后续流式接口设计。

`ChunkEncoder::Encode` 复制输入并返回一个完整的 chunk；零字节输入返回空字符串。
只有 `Finish(trailers)` 会输出终止块。成功 Finish 后，继续 Encode 或 Finish 返回
`Error::InvalidRequest`。禁止的 trailer 返回 `Error::InvalidHeaders`，不会生成部分结果，
编码器仍保持未结束状态。trailer 的顺序和同名多值均保留。

所有头部与 trailer 通过 `Headers` 构造，调用方必须检查 `Add`、`Set` 的结果。
含 CR、LF、NUL 等非法字符的修改返回 `Error::InvalidHeaders`，并保留之前的合法数据。
请求序列化额外校验 method token、版本和 Host；target 中的空格、控制字符、DEL
会被百分号编码，已有转义、查询分隔符和 UTF-8 字节保持原样。
target 必须为 origin-form 或 OPTIONS 的 `*`，不能包含 fragment。

## 验证

以下独立测试不使用网络，覆盖合法输入的每个两段切分位置及逐字节输入：

```text
mcpp test framing --timeout 30
mcpp test request_parser --timeout 30
mcpp test response_parser --timeout 30
mcpp test serializer --timeout 30
mcpp test chunk_encoder --timeout 30
```

用例包含大小上限边界、二进制数据、CRLF 注入、冲突分帧头、chunk 溢出、
trailer 隔离、中间响应上限、pipelining 和截断 EOF。
网络关闭、读写超时与请求走私的端到端验证将在连接和会话层实现后补充。
