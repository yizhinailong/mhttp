# mhttp

基于 standalone Asio 的异步 HTTP 库，使用 mcpp、LLVM 和 C++23 Modules。
提供 HTTP/1.1 服务端与客户端，公共接口通过 `import mhttp;` 使用。

服务端支持同步/协程 handler、路径与查询参数、正则和通配路由、HEAD 回落、
404 / 405、路由 aspect、Basic / Bearer 认证和 CORS。请求与响应拥有数据，
支持 URL 编码表单、multipart 文件与字段，以及可选的 JSON 适配。
协议处理支持有界增量解析、分块、trailer、keep-alive、超时、取消和错误响应后关闭连接。

客户端提供 Get / Head / Post / Put / Patch / Delete / Options、连接复用、
有界重定向、默认头部与认证、分阶段或整体超时。`Task<T>`、`Runtime`、`Sleep`、
`Offload` 和 `SyncWait` 负责异步执行与停止排空。普通请求与默认客户端响应完整缓冲；
服务端支持基于 Task 的 StreamWriter 与内容 provider，以有界分块或已知长度发送响应。
静态目录挂载支持 MIME、条件请求、单段与多段 Range，并拒绝目录穿越和根目录以下的链接。
服务端支持 RouteLazy 按需读取请求体；客户端支持流式响应接收、进度回调和有界 provider 上传；TLS 和压缩尚未实现。

## 构建

工具链和 Asio 依赖的具体版本由 `mcpp.toml` 管理。
准备好配置中指定的 LLVM 工具链后，在项目根目录执行：

```text
mcpp build
mcpp build --locked
```

保留自动生成的 `mcpp.lock`；锁文件由 mcpp 管理。

## 最小用法

服务端先启动 Runtime，再注册和启动监听器。此示例的主线程等待回车，
I/O 线程继续处理请求；停止时先排空服务端，再销毁 Runtime。

```cpp
import std;
import mhttp;

auto main() -> int {
    mhttp::Runtime runtime;
    if (!runtime.Start(2)) { return 1; }
    mhttp::server::HttpServer server{ runtime };
    if (!server.Get("/hello/:name", [](auto& request, auto& response) -> mhttp::Task<void> {
        co_await mhttp::Sleep(std::chrono::milliseconds{ 1 });
        auto result = response.SetContent(
            "Hello, " + std::string{ request.PathParam("name") } + "\n", "text/plain");
        if (!result) { throw std::runtime_error{ std::string{ mhttp::ToString(result.error()) } }; }
    })) { return 1; }
    if (!server.Bind("127.0.0.1", 8080) || !server.Start() || !server.WaitUntilReady()) { return 1; }
    std::cout << "Listening on http://127.0.0.1:8080; press Enter to stop.\n";
    std::string line;
    std::getline(std::cin, line);
    return server.Stop() ? 0 : 1;
}
```

客户端在非 I/O 线程上用 `SyncWait` 等待结果。HTTP 的 4xx / 5xx 仍然是成功收到的
Response；传输、解析或取消失败才通过 `Result` 的错误分支返回。

```cpp
import std;
import mhttp;

auto main() -> int {
    mhttp::Runtime runtime;
    if (!runtime.Start(2)) { return 1; }
    mhttp::client::HttpClient client{ runtime, "http://127.0.0.1:8080" };
    auto result = mhttp::SyncWait(runtime, client.Get("/hello/world"));
    if (!result) {
        std::cerr << mhttp::ToString(result.error()) << '\n';
        return 1;
    }
    std::cout << static_cast<unsigned>(result->Status()) << ' ' << result->Body();
    return result->Status() == mhttp::Status::Ok ? 0 : 1;
}
```

Runtime 必须活得比服务端、客户端及其 Task 更久。一个 HttpClient 同时只接收一个 Send；
并发请求使用多个客户端。不同服务端会话可以并发调用 handler，共享业务状态需自行同步；
不要在 handler 中阻塞等待或执行耗时任务，阻塞业务可通过 `Offload` 移出 I/O 线程。

## REST 示例

在两个终端的仓库根目录分别执行：

```text
mcpp run rest_server -- 8080
```

```text
mcpp run rest_client -- http://127.0.0.1:8080
```

[rest_server](examples/rest_server/main.cpp) 提供内存中的笔记资源：

| 方法与路径 | 行为 |
| --- | --- |
| `GET /health` | 协程 handler 返回就绪状态 |
| `GET /notes?limit=10` | 按 ID 列出笔记，每行是 `ID\t内容`，limit 为 1–100 |
| `POST /notes` | 以纯文本 body 创建笔记，返回 201 和 Location |
| `GET /notes/:id` | 读取笔记，HEAD 自动回落到 GET |
| `PUT /notes/:id`、`PATCH /notes/:id` | 替换已有笔记的内容 |
| `DELETE /notes/:id` | 删除笔记，返回 204 |
| `OPTIONS /notes/:id` | 显式返回 204 和 Allow |

示例最多保存 128 条、每条 1–256 字节且不含控制字符的笔记，不持久化。
资源满时返回 409，不存在时返回 404；不合法的 ID、内容或 limit 返回 400。
[rest_client](examples/rest_client/main.cpp) 演示完整 CRUD、查询参数、Location、HEAD、OPTIONS
和错误状态处理。服务端在主线程收到回车或 stdin EOF 后停止；传入端口 0 可分配动态端口。

JSON 的 feature、独立 `import mhttp.json;` 与读写接口见 [JSON 适配](docs/json.md)；
文件和普通字段的构造、解析与大小限制见 [multipart](docs/multipart.md)。

## 测试

每个 `tests/**/*.cpp` 是独立测试程序。用例在本地运行，不依赖公网服务：

```text
mcpp test --timeout 30
mcpp test url --timeout 30
mcpp test request_parser --timeout 30
mcpp test smoke --timeout 30 -- public
```

模块隔离和测试框架的额外检查：

```text
python tools/check_asio_boundary.py
python tools/check_module_boundary.py
python tools/check_transport.py
python tools/check_session.py
python tools/check_server.py
python tools/check_client.py
python tools/check_json.py
python tools/check_examples.py
python tools/check_test_support.py
```

边界规则与检查方法见 [模块边界](docs/module_boundaries.md)。
协议层的输入、借用生命周期和分帧契约见 [HTTP 协议核心](docs/protocol_core.md)。
协程所有权、运行时生命周期和阻塞任务约束见 [异步基础](docs/async_core.md)。
路径解码、匹配优先级与正则限制见 [路由匹配核心](docs/routing_core.md)。
请求、响应、上下文和 handler 生命周期见 [服务端接口基础](docs/server_api.md)。
客户端消息所有权与访问器见 [客户端接口](docs/client_api.md)。
依赖 Asio 的库代码限定在 `src/**/impl/` 的实现分区内。
上述检查在 Windows 本地执行；Linux 验证通过相应 CI 工作流执行。

## 文档

安装 Doxygen 后，在项目根目录执行：

```text
doxygen Doxyfile
```

打开 `target/doxygen/html/index.html` 查看生成的文档。
文档包含项目说明和源码中的 Doxygen 注释，支持 `.cppm` 模块文件。
现有文档工作流会在 `master` 分支相关文件变更时构建文档并发布到 GitHub Pages。

开发约定见 [AGENTS.md](AGENTS.md)，构建配置格式见
[mcpp 清单文档](https://github.com/mcpp-community/mcpp/blob/main/docs/04-mcpp-toml.md)。
