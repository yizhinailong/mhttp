# mhttp

基于 standalone Asio 的异步 HTTP 库，使用 mcpp、LLVM 和 C++23 Modules。
目标是提供 HTTP/1.1 服务端与客户端，公共接口通过 `import mhttp;` 使用。

当前已实现不依赖网络的基础组件：HTTP 字符串校验、URL 百分号编码与解码、
Base64、HTTP 日期、错误与 `Result<T>`、`Endpoint`、方法和状态码、
有序多值 `Headers` 与 `QueryParams`，以及 HTTP URL 解析与相对引用解析。
HTTP 协议核心支持有大小限制的增量请求/响应解析、分块与 trailer、头部序列化和分块编码。
异步基础已提供 `Task<T>`、`Runtime`、`Sleep`、`Offload` 和 `SyncWait`，
支持 strand 恢复、停止取消和协程资源清理。
内部 TCP 连接支持有界收发、整次操作超时、半关闭和跨线程关闭。
路由匹配核心支持静态、正则、参数和通配段，以及方法分派与 HEAD 回落。
服务端基础提供拥有数据的 Request / Response、线程安全的 RequestData，
以及公共 Router 注册、同步和协程 handler、有序全局/路由 aspect、路由钩子与异常响应处理。
内部 HTTP 会话已接入完整请求体、顺序流水线、keep-alive、协议错误响应和停止取消。
公共 HttpServer 提供监听、就绪等待、同步/异步停止、会话排空和保留路由的重启。
Listen 支持在调用线程运行空闲 Runtime，并提供连接数上限、安全的监听复用、IPv6 双栈配置，
以及拥有回调的诊断日志与访问日志。客户端已接入惰性 DNS / TCP 连接、异步 Send、
有界响应解析、连接复用、分阶段超时、single-flight 和跨线程取消。

## 构建

工具链和 Asio 依赖的具体版本由 `mcpp.toml` 管理。
准备好配置中指定的 LLVM 工具链后，在项目根目录执行：

```text
mcpp build
mcpp build --locked
```

保留自动生成的 `mcpp.lock`；锁文件由 mcpp 管理。

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
python tools/check_test_support.py
```

边界规则与检查方法见 [模块边界](docs/module_boundaries.md)。
协议层的输入、借用生命周期和分帧契约见 [HTTP 协议核心](docs/protocol_core.md)。
协程所有权、运行时生命周期和阻塞任务约束见 [异步基础](docs/async_core.md)。
路径解码、匹配优先级与正则限制见 [路由匹配核心](docs/routing_core.md)。
请求、响应、上下文和 handler 生命周期见 [服务端接口基础](docs/server_api.md)。
客户端消息所有权与访问器见 [客户端接口](docs/client_api.md)。
依赖 Asio 的库代码限定在 `src/**/impl/` 的实现分区内。
当前检查已在 Windows 本地通过；Linux 由 CI 验证。

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
