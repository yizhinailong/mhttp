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
