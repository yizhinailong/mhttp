# 测试设施

默认测试是 `tests/**/*.cpp` 中提供 main 的独立程序。每个程序先 `import std; import mhttp;`，
再包含需要的辅助头。测试之间通过退出码报告成败，不共享全局服务器或固定端口。

| 辅助 | 用途 |
| --- | --- |
| `tests/support/test_support.hpp` | Check / CheckEq 使用 source_location 和线程安全失败计数；RunTests 支持过滤、异常捕获及每用例 Deadline 看门狗 |
| `tests/support/parser_support.hpp` | 拥有解析结果，遍历输入切分位置并限制无进展事件循环 |
| `tests/support/thread_task.hpp` | 在非 I/O 调用线程执行 SyncWait，保留结果/异常并以 jthread 管理线程所有权 |
| `tests/support/test_server.hpp` | 拥有 Runtime 和 HttpServer，在动态回环端口启动，析构时等待全部会话与 Runtime 清理 |

TestServer 在构造时启动 Runtime，调用 `Server()` 注册测试所需的一组路由，
再调用 `Start()` 获取实际端口。`Execution()` 可供同作用域内的客户端和 SyncWait 使用；
这些借用对象与操作必须在 fixture 销毁前结束。验证停机取消时，可将测试调用方放在另一个 Runtime 上，
使其观测 fixture 析构后的断连；`tests/test_server_test.cpp` 覆盖这一所有权顺序。

原始 socket 与脚本化服务端使用测试分区，而非在每个公共消费者中包含 Asio 辅助头。
`check_transport.py`、`check_session.py`、`check_server.py` 和 `check_client.py`
将相关 fixture 复制到临时项目的 impl 目录，显式导入 Asio，仅在该临时主模块中导出测试入口。
正常构建不导出这些入口，不依赖本机缓存路径。

| 原始对端 fixture | 验证行为 |
| --- | --- |
| `tests/transport/transport_tests.cppm` | 独立 strand、动态端口、分段通信、取消、半关闭与资源所有权 |
| `tests/server/session_tests.cppm` | 原始交换、分帧、流水线计数、请求走私防护与停机入口 |
| `tests/server/server_peer.cppm` 及 listener 场景 | 单次交换、持久连接复用、分段正文与请求头、真实 reset 和背压 |
| `tests/client/client_peer.cppm` | ClientPeerPlan 在读取请求头后按延迟脚本发送字节、停顿或断开，独立于库的解析器 |
| `tests/client/client_streaming.cppm` 等场景 | provider、分片接收、读写截止、截断与回调清理 |

网络测试使用回环地址与动态端口；网络操作预算、每用例看门狗和 mcpp 进程超时形成不同层次的上限。
拒绝连接使用已关闭的动态回环端口，DNS 失败使用保留的 `.invalid` 域名。
依赖路由环境的 SYN 丢弃测试默认跳过，仅在设置 `MHTTP_TEST_CONNECT_TIMEOUT_URL` 时运行；
该地址应由测试环境控制。普通验收不连接公网 HTTP 服务。

运行 `python tools/check_test_support.py` 可独立验证故意失败、异常、无匹配过滤和看门狗的退出行为。
`check_examples.py` 在独立的 `examples` 项目中构建并验证两个 REST 示例，
`check_stress.py` 使用动态回环服务检查负载计数、复用、
单 I/O 线程并发和失败处理。Linux 上使用 python3 执行相同脚本；本地 Windows 验证与远端 CI 结果分别报告。
