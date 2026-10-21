# 异步基础

消费者通过 `import mhttp;` 使用 `Task<T>`、`Runtime`、`Sleep`、`Offload` 和
`SyncWait`。公共接口使用标准库类型；Asio 执行器和桥接函数位于实现分区。
监听器、HTTP 会话和客户端通过内部桥接运行用户 Task，保持公共接口中的 Asio 名字不可见。

## Task 的所有权

`Task<T>` 是惰性、只可移动、只能消费一次的协程。创建 Task 时不会执行函数体；
用 `co_await std::move(task)` 消费已有 Task，或直接等待返回的临时 Task。
支持 `void` 和拥有所有权的返回值，包括不能默认构造或复制的类型；不支持引用返回值。
异常沿嵌套等待原样传播，子协程结束时使用对称转移，避免深层等待累积调用栈。

Task 拥有协程帧。未开始等待就析构时会释放帧和按值传入的参数；完成后由等待方销毁。
引用参数和 lambda 借用的对象必须活到整个异步操作完成。避免立即调用带捕获的
协程 lambda；优先用普通协程函数的按值参数保存所有权。

## Runtime 的生命周期

`Runtime` 创建时不启动线程。`Start(thread_count)` 启动指定数量的 I/O 线程，
默认一个；零线程返回 `InvalidRequest`。`Run()` 在调用线程运行一个新生命周期，
直到其他线程调用 `Stop()` 并完成清理。`Start` 和 `Run` 是两种启动方式。

`Stop()` 可从任意线程请求取消，重复调用安全，不等待 I/O 线程退出。
它保留完成处理器的执行机会，使定时器取消、Task 恢复和协程析构能够正常完成。
`Join()` 等待运行循环、已接受的任务及其清理结束。先 `Stop()` 再 `Join()`；
未停止的 Runtime 会保留工作守卫，`Join()` 会继续等待。

停止后须先完成 `Join()` 才能再次启动；在此之前启动返回 `AlreadyRunning`。
重复 `Join()` 安全。析构会停止并等待，因此不要在 I/O 线程析构 Runtime。
Runtime 必须活过使用它的 Task、服务端和客户端对象。
`Join()`、`Run()` 在任意 Runtime 的 I/O 线程调用都会返回 `Busy`。

每个独立提交的根 Task 使用自己的 strand；其嵌套 Task、定时器完成和工作线程结果
都回到同一 strand。不同根 Task 可以并发执行，跨根共享的应用状态需要自行同步。
`Runtime::IsIoThread()` 标识任意 mhttp Runtime 的 I/O 执行线程。

## 等待与阻塞工作

`Sleep(duration)` 挂起当前 Task。零或负时长也异步完成；Runtime 停止时，
尚未完成的等待抛出代码为 `Error::Canceled` 的 `std::system_error`。

`Offload(function)` 拥有传入的函数，在 Runtime 的独立工作池调用它，再回到原 strand。
当前工作池每个 Runtime 两个线程，首次使用时创建，在完成停止和重启后复用。
函数和结果均可只移动；返回 `void` 或拥有所有权的值，异常保留原类型和消息。
I/O 线程中应使用 `Offload` 执行阻塞文件访问或耗时业务操作。

取消不会中断正在执行的普通函数。开始执行前已取消时不调用函数；执行期间取消时，
函数先返回，再报告 `Canceled` 并清理捕获。此时 `Stop()` 仍及时返回，
`Join()` 必须等待函数结束。函数自己的异常优先原样传播。

`SyncWait(runtime, task)` 面向 `main` 和测试，在非 I/O 线程等待结果。
它返回前已销毁用户协程帧；异常原样重新抛出。Runtime 未启动或正在停止时抛出
`std::system_error`，代码为 `NotRunning`；任意 I/O 线程调用时抛出 `Busy`，
包括等待另一个 Runtime 的情况。

```cpp
import std;
import mhttp;

auto calculate() -> mhttp::Task<int> {
    co_await mhttp::Sleep(std::chrono::milliseconds{ 10 });
    co_return co_await mhttp::Offload([] { return 42; });
}

auto main() -> int {
    mhttp::Runtime runtime;
    if (!runtime.Start(2)) { return 1; }
    auto const result = mhttp::SyncWait(runtime, calculate());
    runtime.Stop();
    return runtime.Join() && result == 42 ? 0 : 1;
}
```

## 内部 TCP 连接

`impl::Connection` 通过共享所有权持有 socket，每个连接有独立 strand。
创建时校验读缓冲和单次发送上限：读缓冲默认 16 KiB、最大 64 KiB，
单次发送默认最多 16 MiB，两者必须为正数。HTTP 消息的大小限制由协议层另外管理。
接受或建立连接期间才可借用初始化 socket；开始读写后，所有 socket 访问均由连接管理。

`ReadSome` 返回独立拥有数据的字符串，后续读取不会使其失效。
`WaitForPeerData` 异步观察空闲连接的可读状态，不消耗数据或 EOF，与 ReadSome 共享接收 Busy 检查。
可传可选期限；默认无限等待，非正期限立即返回 ReadTimeout。超时和仅取消等待均保留
套接字及所有未读字节，完成排空后可以继续写入和读取，适用于有超时回退的协议握手。
调用方必须等等待操作完成后再开始后续 I/O。
客户端可据此监控空闲对端关闭，而不占用下一次响应的字节。
`WriteAll` 拥有传入的字符串，处理部分写入，成功表示全部字节已发送。
`WriteAllWithProgress` 使用相同的发送、截止时间和 Busy 检查，同时返回完成字节数及可选错误。
完整成功计数等于 payload 长度；上限拒绝、已关闭或立即超时计数为零，
中途断开、超时或取消则保留底层报告的已完成前缀。计数表示本地发送操作的完成量，不能证明对端应用已消费。
客户端重试策略必须同时检查错误类型及头部进度；已报告正数前缀时不能透明重发，零计数的取消或超时也不能直接重试。
发送超过上限时在发送任何字节前返回 `BodyTooLarge`。
同一方向只允许一个操作在途，重复操作返回 `Busy`；读取与发送可以同时挂起。
返回的惰性操作在初始挂起前已经持有连接，调用方可以释放自己的连接引用。

读写默认超时 30 秒，可传 `nullopt` 禁用；非正时长立即超时。
一次 `WriteAll` 的所有部分写入共享截止时间；`ReadSome` 的截止时间针对这一次读取，
HTTP 会话在多次读取之上维护整个请求头或正文阶段的绝对截止时间，分段到达不会延长预算。
超时会关闭连接，触发超时的操作返回 `ReadTimeout` 或 `WriteTimeout`，
其他在途操作返回 `Canceled`。失败发送可能已发出前缀，连接不能继续复用。
已完成操作的定时器即使排队等待回调，也不会关闭后续操作。

对端 EOF 和 reset 映射为 `ConnectionClosed`，其中接收 EOF 仍允许本地发送响应。
`Shutdown` 在发送完成后半关闭发送方向，读取仍可继续；发送在途时返回 `Busy`。
`Close` 可从任意线程请求，实际关闭投递到连接 strand，重复调用安全。
关闭后的新操作返回 `ConnectionClosed`。取消在途 I/O 返回 `Canceled`；
桥接尚未启动的操作时，取消也可能以代码为 `Canceled` 的 `std::system_error` 传播。
HttpClient 的公共请求接口将这一边界统一为返回 `Result` 中的 `Error::Canceled`。

Runtime 必须活过连接和异步操作，并持续执行到关闭、取消回调及定时器清理完成。
`core.impl.io_error` 按连接、解析地址、绑定、读取和发送的上下文集中映射传输错误，
区分操作截止时间、主动取消和对端断开。

## 验证

`tests/core/task_test.cpp` 覆盖值、异常、深层对称转移及帧所有权；
`runtime_test.cpp` 覆盖启停、并发停止和等待、重启以及析构；
`async_test.cpp` 覆盖定时器、取消竞争、I/O 同步等待拒绝和同 strand 串行性；
`offload_test.cpp` 覆盖工作线程、结果与异常恢复、I/O 响应和停止清理。
`python tools/check_transport.py` 运行内部传输测试，覆盖回环分段收发、读写上限、
整体写超时、定时器复用、半关闭、跨线程关闭和取消、连接所有权与对端重置，
并检查 Task 根、Asio 调用方和操作使用独立 strand 时的结果、异常与恢复位置。
当前本地验证平台为 Windows；Linux 尚未本地验证。
