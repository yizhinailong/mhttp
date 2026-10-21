# mhttp

基于 standalone Asio 的异步 HTTP 库，使用 mcpp、LLVM 和 C++23 Modules。
目标是提供 HTTP/1.1 服务端与客户端，公共接口通过 `import mhttp;` 使用。

当前已实现不依赖网络的基础组件：HTTP 字符串校验、URL 百分号编码与解码、
Base64、HTTP 日期、错误与 `Result<T>`、`Endpoint`、方法和状态码、
有序多值 `Headers` 与 `QueryParams`，以及 HTTP URL 解析与相对引用解析。
HTTP 协议核心支持有大小限制的增量请求/响应解析、分块与 trailer、头部序列化和分块编码。
监听器、连接会话和客户端 I/O 待后续阶段实现。

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
python tools/check_test_support.py
```

边界规则与检查方法见 [模块边界](docs/module_boundaries.md)。
协议层的输入、借用生命周期和分帧契约见 [HTTP 协议核心](docs/protocol_core.md)。
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
