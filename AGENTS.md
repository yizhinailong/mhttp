# 项目约定

## 项目方向

`mhttp` 是基于 standalone Asio 的异步 HTTP/1.1 库：协议核心负责有界的增量解析与序列化，
服务端提供监听器、会话、路由、中间件、静态文件、流式响应和 `mhttp::app()` 应用入口，
客户端提供连接复用、重定向和连接池；JSON 与 SOCI 是可选模块，TLS 和压缩尚未实现。
功能概览见 `README.md`，各组件的契约见 `docs/`。
实现新功能时保持传输、协议处理和应用接口的职责清晰，按实际需求逐步增加组件。

## 技术栈与构建

- 使用 **mcpp** 管理项目、依赖、构建和测试；构建清单是根目录的 `mcpp.toml`。
- 使用 **LLVM**；具体工具链版本由 `mcpp.toml` 和 CI 配置管理。
- 使用 **C++23 Modules**，标准库通过 `import std;` 引入。
- 使用 **`chriskohlhoff.asio`**，通过 `import asio;` 引入，具体依赖版本由 `mcpp.toml` 管理。
- 文档不重复记录依赖和工具链的具体版本号，版本信息以构建配置为准。
- 通过 mcpp 管理第三方模块和平台链接配置，避免手工复制依赖或引用本机缓存路径。
- 所有 `mcpp.lock`（根目录、`examples/` 及 `tests/` 下的消费者工程）由 mcpp 在本地生成
  并被 `.gitignore` 忽略，任何情况下都不要提交；CI 和检查脚本不得依赖已提交的锁文件。
  不要手动编辑锁文件或构建产物。

常用命令（在仓库根目录执行）：

```text
mcpp build
mcpp build --locked
doxygen Doxyfile
```

按 `mcpp.toml` 中指定的版本准备 LLVM 工具链后即可构建；
项目通过 `[toolchain].default` 选择编译器。
使用 `mcpp test --timeout 30` 运行测试；模块边界、私有实现、可选模块和示例的额外检查
由 `tools/check_*.py` 执行，完整列表见 `README.md`。
库本身没有可运行入口；示例程序在 `examples/` 目录中通过 `mcpp run <target>` 运行。

## 目录与模块

- `src/mhttp.cppm`：公共主模块，消费者通过 `import mhttp;` 使用库。
- `src/{core,client,server,utils}/`：按职责划分的库组件，使用 `mhttp:component` 分区
  （如 `mhttp:server.router`），声明和实现放在同一个模块接口文件中，由主模块导出公共分区。
- `src/**/impl/`：依赖 Asio 的实现分区，只由公共分区导入、不重新导出，规则见 `docs/module_boundaries.md`。
- `src/core/json.cppm`、`src/db/soci.cppm`：由 `json`、`soci` feature 启用的独立模块
  `mhttp.json`、`mhttp.soci`，默认 `import mhttp;` 不依赖它们。
- `tests/**/*.cpp`：独立测试程序，每个程序提供 `main()`，以退出码表示成败；
  `tests/json/`、`tests/soci/` 是开启 feature 的独立消费者工程，由对应检查脚本运行，不参与根项目的测试发现。
- `tools/check_*.py`：在临时或独立工程中运行的额外检查，包括私有分区测试、可选模块和示例。
- `examples/`：独立的 mcpp 示例项目，`examples/mcpp.toml` 通过路径依赖使用 mhttp
  并声明 rest_server、rest_client、stress_test 等可执行目标；不在根 `mcpp.toml` 中添加示例目标。
- `target/` 和 `compile_commands.json`（包括 `examples/` 下的同名产物）为生成文件，不提交。
- `Doxyfile`：Doxygen 配置，解析项目说明和 `src/`，包含 `.cppm` 模块文件，
  输出 HTML 到 `target/doxygen/html/`，由现有文档工作流发布至 GitHub Pages。
- `.github/workflows/`：Linux / Windows CI 构建并运行 `mcpp test` 与全部检查脚本；
  文档工作流在 `main` 分支构建并发布 Doxygen；version-tag 工作流在 `mcpp.toml` 版本变化时创建标签。

## C++ 风格

- 使用四空格缩进，左花括号同行，命名空间内部缩进。
- 类型和公共成员函数使用 `PascalCase`；私有成员函数使用 `camelCase`。
- 变量、自由辅助函数和文件名使用 `snake_case`；私有数据成员加 `m_` 前缀。
- 常量使用 `UPPER_SNAKE_CASE`；命名空间使用小写，公共命名空间为 `mhttp`。
- 保留标准库和 Asio API 原名，包括协程协议要求的函数名。
- 优先使用尾置返回类型、花括号初始化和 `T const&`；指针和引用符号贴类型。
- 函数签名单行超过 100 列时，在 `(` 后换行，每个参数独占一行，
  `)` 与声明起始处对齐，限定符和尾置返回类型紧随其后。
- C++ 文件以包含 `@file`、`@brief` 的多行 Doxygen 注释开头。
  类型和函数使用多行 `/** ... */` 注释，说明参数、所有权和必要的生命周期约束。
- 使用 RAII 管理资源，根据所有权选择智能指针；可恢复失败优先使用
  `std::expected`，缺失值使用 `std::optional`。
- 若存在 `.clang-format`，遵循该配置；不创建或修改该文件。

## 异步实现原则

- 网络收发使用 Asio 异步操作，优先使用 `asio::awaitable`、`co_await` 和
  `asio::co_spawn` 组织控制流。
- 不在 I/O 执行线程中执行阻塞网络操作、阻塞等待或耗时业务任务。
- 明确监听器、会话、套接字和协程的所有权；缓冲区及其引用必须存活到异步操作完成。
  避免协程捕获生命周期不足的对象，尤其是临时协程 lambda 的捕获。
- 多线程运行 `io_context` 时，明确共享状态的同步方式；需要串行访问的会话状态
  使用 strand 或其他明确的执行器约束。
- 明确处理断连、超时、取消和正常关闭；协程完成处理必须能接收和处理异常。
- 实现协议时设置明确的输入大小、超时和待发送数据上限，并处理分段收发。

## 验证与修改范围

- 构建相关修改运行 `mcpp build`；有锁文件时通过 `mcpp build --locked` 验证可复现解析。
- 新增实际异步行为时，测试正常通信、断连、取消、超时和资源清理等相关场景。
  网络测试使用回环地址和动态端口，设置明确超时，避免依赖公网服务。
- 修改 Doxygen 配置或文档注释后运行 `doxygen Doxyfile`，确认无警告且生成首页。
- 仅文档修改无需编译；交付时如实说明执行的检查及尚未验证的平台。
- 保留用户已有改动；不因初始化或局部功能开发重写无关工作流或扩大功能范围。
