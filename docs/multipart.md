# 内存 multipart 表单

`import mhttp;` 提供 FormData、FormPart 和 MultipartLimits，无需可选依赖。
Request::Form() 继续解析 urlencoded；Request::Multipart() 按需解析 multipart/form-data。
两者返回独立拥有的数据，不缓存、不改写 Body，结果可以在请求销毁后使用。

```cpp
auto registered = server.Post("/upload", [](auto& request, auto& response) {
    auto form = request.Multipart();
    if (!form) {
        static_cast<void>(response.SetError(form.error() == mhttp::Error::BodyTooLarge
            ? mhttp::Status::ContentTooLarge : mhttp::Status::BadRequest));
        return;
    }
    for (auto const& part : form->Parts()) {
        // part.name / content / filename / content_type 均由 form 拥有。
    }
    static_cast<void>(response.SetEmpty(mhttp::Status::NoContent));
});
if (!registered) { throw std::runtime_error{ "cannot register upload route" }; }
```

Parts() 保留字段与文件的原始顺序，包括重复名字。Get(name) 返回第一个同名分段的借用指针，
Count(name) 统计全部同名分段；名字大小写敏感。filename 为 nullopt 表示普通字段，
存在的空字符串表示文件名为空的上传。文件名只是元数据，不决定任何本地文件路径。
解析保留二进制内容与完整 Content-Type，不进行 charset、Content-Transfer-Encoding 或嵌套 multipart 转换。

默认最多 128 个分段，普通字段和文件都计数。单段内容与完整 body 的默认上限均为 8 MiB；
每段头部最多 8 KiB、32 个字段，包含被忽略的字段。传入 MultipartLimits 可独立调整这些上限。
HTTP 服务端仍先执行 ServerOptions::body_bytes；提高 multipart 上限不会跳过 HTTP 层的限制。

FormData::Parse(body, content_type, limits) 可独立解析已缓冲数据；ParseBoundary(content_type)
可单独校验媒体声明。缺少/无效 boundary 或不完整分隔符返回 InvalidRequest；重复或错误 MIME 参数、
非法分段头部与超量分段头部返回 InvalidHeaders；body、单段或分段数超限返回 BodyTooLarge。
Request::Multipart() 还拒绝重复 Content-Type。分配异常继续传播。

boundary 为 1 至 70 个 ASCII 字节，可在媒体参数中加引号，不能以空格结尾。
解析器接受前导内容、尾随内容和分隔行上的传输空白；只把完整分隔行识别为边界，
类似 `--boundaryX` 的内容前缀不会截断数据。搜索只向前推进，缺少初始边界时不会重复复制剩余 body。
Content-Disposition 必须是 form-data 并含 name；支持 quoted-pair，转义引号不会结束参数。
百分号序列保持字面量，不隐式解码文件名或字段名。

格式依据 [RFC 7578](https://www.rfc-editor.org/rfc/rfc7578.html) 和
[RFC 2046 §5.1.1](https://www.rfc-editor.org/rfc/rfc2046.html#section-5.1.1)。

客户端通过 FormData::Add(FormPart) 按顺序构造字段和文件，再同步调用 Request::SetMultipart：

```cpp
mhttp::FormData form;
form.Add({ .name = "title", .content = "example" });
form.Add({ .name = "upload", .content = file_bytes, .filename = "data.bin" });
mhttp::client::Request request{ mhttp::Method::Post, "/upload" };
if (auto encoded = request.SetMultipart(form); !encoded) {
    throw std::runtime_error{ "cannot encode multipart request" };
}
auto response = co_await client.Send(std::move(request));
```

SetMultipart 完成后，原表单和文件字节无需存活到 Send 完成。文件分段缺少媒体类型时使用
application/octet-stream，普通字段不推断 Content-Type。构造失败时保留原请求 body 和字段。
也可调用 FormData::Encode(boundary, limits)，直接取得独立拥有的 body / content_type。
编码器执行同样的字段数、单段、整体输出和分段头部限制，整体限制包含分隔符与头部。

空 boundary 参数生成独立候选并检查实际内容，无需阻塞随机源；显式 boundary 无效或出现在内容行开头
时返回 InvalidRequest。名称和文件名的双引号、CR/LF 及其他控制字节编码为 %HH，
反斜杠使用 quoted-pair；现有百分号序列保持原样。接收端不自动反解这些百分号序列。
每个 MIME 参数声明还最多允许 32 个参数、64 KiB 文本，避免歧义与不受限的参数分配。

`mcpp test multipart --timeout 30` 运行解析、构造和真实 HTTP 验收，包含 20 轮随机二进制表单、
约 4 MiB 上传、默认/可配置上限及错误映射。`python tools/check_server.py "listener multipart"`
使用独立原始 TCP 对端逐字节发送 multipart，验证前导内容、重复文件、连接复用、中途 EOF、
请求超时及 Stop 后的资源清理，不依赖公网。
