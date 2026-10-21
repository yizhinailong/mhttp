# 模块边界

公共主模块只重新导出公共分区，公共接口使用标准库和 mhttp 类型。
依赖 Asio 的代码放在 `src/**/impl/` 中，用实现分区
（例如 `module mhttp:core.impl.runtime;`）声明，由公共分区通过
`import :core.impl.runtime;` 引入，不重新导出。

公共对象可以用 `std::shared_ptr<impl::XxxImpl>` 持有实现，公共成员函数在
公共分区中定义并转发。消费者只需 `import mhttp;`，不能命名实现类型或
Asio 类型；网络测试可以显式 `import asio;`。

`python tools/check_module_boundary.py` 在临时项目中检查上述结构：
先构建并运行 PIMPL 调用，再分别确认 Asio 名字和实现类型的使用会编译失败，
最后确认显式导入 Asio 能编译运行。该检查已在本地 Windows 工具链通过，
Linux 结果由 CI 验证。工具链版本以构建清单为准。

`python tools/check_transport.py` 把库源码和构建清单复制到临时项目，
仅在该项目导出 `tests/transport/transport_tests.cppm` 测试分区。
测试通过内部接口验证传输错误和回环通信，正常构建不会导出该分区。
依赖、工具链和平台链接仍由 mcpp 管理；可在命令后附加用例名称过滤。

测试程序在导入模块后包含 `tests/support/test_support.hpp`。
`mcpp test --timeout 30` 运行测试，`mcpp test smoke -- public` 按用例名过滤。
`python tools/check_test_support.py` 用独立进程验证失败计数、异常捕获、
过滤和看门狗退出；其中故意失败的进程必须返回非零退出码。
