# 可选 SOCI 数据库接口

默认 `import mhttp;` 不依赖数据库实现。启用 `soci` feature 后，显式导入 `mhttp.soci`，
可使用 `mhttp::db::SessionPool` 管理一组已连接的 SOCI 会话，并在 Runtime 的阻塞工作线程上执行语句。

消费者通过 mcpp 启用依赖的 feature，例如：

```toml
[dependencies]
mhttp = { path = "../mhttp", features = ["soci"] }
```

```cpp
import std;
import mhttp.soci;

auto main() -> int {
    mhttp::Runtime runtime;
    if (!runtime.Start(2)) { return 1; }
    auto opened = mhttp::db::SessionPool::Open(soci::sqlite3, "db=app.db timeout=5", { .size = 2 });
    if (!opened) { return 1; }
    mhttp::server::HttpServer server{ runtime };
    auto routed = server.Get("/notes/:id", [pool = *opened](
        mhttp::server::Request& request,
        mhttp::server::Response& response
    ) -> mhttp::Task<void> {
        auto id = request.PathParam<long long>("id");
        if (!id) {
            static_cast<void>(response.SetError(mhttp::Status::BadRequest));
            co_return;
        }
        auto text = co_await pool.Execute([id = *id](soci::session& sql) -> std::optional<std::string> {
            std::string text;
            sql << "select text from note where id = :id", soci::use(id), soci::into(text);
            return sql.got_data() ? std::optional{ std::move(text) } : std::nullopt;
        });
        static_cast<void>(text ? response.SetContent(*text, "text/plain")
                               : response.SetError(mhttp::Status::NotFound));
    });
    // 绑定、启动和停止服务端与普通用法相同。
    return routed ? 0 : 1;
}
```

SOCI 依赖由 feature 的清单配置管理，版本信息以构建配置为准。该依赖只编入 SQLite3 后端，
后端通过工厂对象 `soci::sqlite3` 传入；不要使用按名称加载后端的 `session("sqlite3", …)`，
它会尝试动态加载未随包提供的后端共享库。

## 打开连接池

`SessionPool::Open(backend, connect_string, options)` 在调用线程同步连接全部会话。
`SessionPoolOptions::size` 默认 2，`acquire_timeout` 默认 5 秒，两者都必须为正，
等待时长还须能以 int 毫秒表示，否则返回 InvalidRequest。在任意 Runtime 的 I/O 线程调用返回 Busy；
应在启动服务前打开，或放入 `Offload`。后端拒绝连接或选项时抛出 `soci::soci_error`，
保留后端诊断，已打开的会话随之关闭。后端工厂必须活过连接池及其 Task，SQLite 工厂是静态对象。

SQLite 的连接字符串可以只是数据库路径，也可以写成 `db="路径" timeout=秒` 等选项，
其中 `timeout` 设置会话遇到写锁时的忙等待。`:memory:` 下每个会话都是独立数据库，
多个会话需要共享数据时使用文件数据库。

## 执行数据库工作

`Execute(function)` 把函数移入惰性 Task，经 `Offload` 在阻塞工作线程上调用，
函数独占一个会话，完成后回到等待方原来的 strand。函数接受 `soci::session&`，
返回 void 或拥有所有权的值，函数和结果都可以只移动；不支持引用返回值。

工作线程等待空闲会话超过 `acquire_timeout` 时，抛出代码为 AcquireTimeout 的 `std::system_error`，
不调用函数；移动后的空连接池抛出 NotRunning。函数自身的异常，包括 `soci::soci_error`
及 `soci::sqlite3_soci_error` 等后端子类，在会话归还后原样传播。取消与 `Offload` 一致：
执行中的函数不会被中断，返回并归还会话后再报告 Canceled；`Stop()` 及时返回，`Join()` 等待函数结束。

会话只在函数调用期间有效，不要保存会话或绑定到它的 statement、rowset。
多语句写入使用 `soci::transaction`，函数抛出异常时事务在会话归还前回滚。
函数借用的其他对象必须活到 Task 完成。

## 所有权与并发

`SessionPool` 可以复制，副本共享同一组会话。已发出的 Task 持有这组会话，
最后一个副本和 Task 都结束后才关闭连接，因此 handler 可以按值捕获连接池。
除对同一对象赋值或析构外，成员函数可从任意线程调用。

每个 Runtime 的阻塞工作池当前有两个线程，Execute 的并发度同时受会话数和工作线程数限制。
等待会话也占用工作线程，最长为 `acquire_timeout`；会话数不少于共享该连接池的
所有 Runtime 的工作线程总数时，不会出现等待。

## 导出的 SOCI 名称

`mhttp.soci` 重新导出常用的 SOCI 名称：`session`、`backend_factory`、`connection_parameters`、
`into`、`use`、`indicator` 及 `i_ok` / `i_null` / `i_truncated`、`row`、`rowset`、`rowid`、
`column_properties`、`statement`、`transaction`、`values`、`blob`、`type_conversion`、
`soci_error`、`sqlite3` 和 `sqlite3_soci_error`。`std::optional` 可直接作为 into/use 的值类型表示 NULL。
需要其他名称（例如 `soci::db_type` 的枚举值）时，在源文件的所有 `import` 之前
`#include <soci/soci.h>`，头文件和模块中的名称指向同一份 SOCI。

## 验证

`python tools/check_soci.py` 在独立消费者工程中从依赖侧开启 feature，运行有锁文件的测试。
`soci_test` 覆盖 DDL、事务提交、类型化读取、NULL、只移动的函数与结果、后端异常类型和类别、
异常回滚后会话复用、非法选项、I/O 线程拒绝连接、副本共享与 Task 持有会话、
获取超时后恢复、四线程并发写入文件数据库时会话独占、HTTP handler 读写数据库，
以及 Runtime 停止时执行中的函数完成并归还会话。`soci_include_test` 验证先包含 SOCI 头文件再导入模块的用法。
该工程的配置及自动生成锁文件保存在 tests/soci；根目录锁文件保留默认无 SOCI 的解析结果。
依赖变更后使用 `python tools/check_soci.py --update-lock` 由 mcpp 重新生成消费者锁文件。
