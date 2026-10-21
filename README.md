# mhttp

基于 standalone Asio 的异步服务端项目，使用 mcpp、LLVM 和 C++23 Modules。
当前已初始化构建骨架，服务端功能尚未实现。

## 构建

工具链和 Asio 依赖的具体版本由 `mcpp.toml` 管理。
准备好配置中指定的 LLVM 工具链后，在项目根目录执行：

```text
mcpp build
```

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
