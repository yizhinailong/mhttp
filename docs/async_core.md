# 异步基础

消费者通过 `import mhttp;` 使用 `Task<T>`、`Runtime`、`Sleep`、`Offload` 和
`SyncWait`。公共接口使用标准库类型；Asio 执行器和桥接函数位于实现分区。
这些组件尚不包含 TCP 连接、监听器或 HTTP 会话。

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
Runtime 必须活过使用它的 Task 和后续服务器、客户端对象。
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

## 验证

`tests/core/task_test.cpp` 覆盖值、异常、深层对称转移及帧所有权；
`runtime_test.cpp` 覆盖启停、并发停止和等待、重启以及析构；
`async_test.cpp` 覆盖定时器、取消竞争、I/O 同步等待拒绝和同 strand 串行性；
`offload_test.cpp` 覆盖工作线程、结果与异常恢复、I/O 响应和停止清理。
当前本地验证平台为 Windows；Linux 尚未本地验证。
