# 可选 JSON 接口

默认 `import mhttp;` 不依赖 JSON 实现。启用 `json` feature 后，显式导入 `mhttp.json`，
可使用 `mhttp::Json` / `OrderedJson`、服务端 Request::Json / Response::SetJson，
以及客户端 Request::SetJson / Response::Json。

消费者通过 mcpp 启用依赖的 feature，例如：

```toml
[dependencies]
mhttp = { path = "../mhttp", features = ["json"] }
```

```cpp
import std;
import mhttp.json;

auto handler(mhttp::server::Request& request, mhttp::server::Response& response) -> void {
    auto value = request.Json();
    if (!value) {
        static_cast<void>(response.SetError(mhttp::Status::BadRequest));
        return;
    }
    static_cast<void>(response.SetJson(*value));
}
```

基础消息类型保留延迟实例化的访问器，其模块不导入 JSON 实现；只有导入 `mhttp.json`
的消费者才能调用这些接口。采用独立模块入口是因为当前 mcpp 禁止条件预处理中的 import。
JSON 依赖由 feature 的清单配置管理，版本信息以构建配置为准。

Json() 接受 application/json 及 application/*+json，媒体类型大小写无关，可带参数，
但不转码 charset。缺少/不匹配 Content-Type、无效 UTF-8、错误 JSON 或非法配置返回
InvalidRequest；重复 Content-Type 返回 InvalidHeaders。原始 NUL 字节被拒绝，JSON 中的
`\u0000` 正常还原。根值可以是对象、数组或标量。

解析前按 JsonLimits::max_depth 检查数组/对象嵌套，默认上限 128，超限返回 BodyTooLarge；
字符串中的括号和转义引号不计入深度。上限必须为正。HTTP 请求和响应仍先执行原有 body 字节数限制。
每次 Json() 返回独立拥有的 JSON 值，不缓存、不改变 Body；结果可在原消息销毁后继续使用。

SetJson 同步生成完整 JSON 字符串，设置 Content-Type: application/json，保留响应状态码。
Json 与 OrderedJson 均可写入；后者保留对象成员顺序。非法 UTF-8 等序列化失败返回 InvalidRequest，
保留原 body、字段和状态。调用完成后不借用 JSON 输入，分配失败等非 JSON 异常继续传播。

`tests/core/json_support_test.cpp` 验证仅导入 mhttp 不会隐式启用 JSON 接口。
`python tools/check_json.py` 在独立消费者工程中从依赖侧开启 feature，运行有锁文件的测试，
覆盖空对象、数组、约 33 KiB 数据、15 轮随机 JSON、媒体类型、深度、无效输入和序列化失败。
该工程的配置及自动生成锁文件保存在 tests/json；根目录锁文件保留默认无 JSON 的解析结果。
依赖变更后使用 `python tools/check_json.py --update-lock` 由 mcpp 重新生成消费者锁文件。
