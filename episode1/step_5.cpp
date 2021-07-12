#include <array>
#include <iostream>
#include <memory>
#include <asio.hpp>
#include <asio/experimental/as_tuple.hpp>
#include <asio/experimental/awaitable_operators.hpp>

using asio::awaitable;
using asio::buffer;
using asio::co_spawn;
using asio::detached;
using asio::ip::tcp;
namespace this_coro = asio::this_coro;
using namespace asio::experimental::awaitable_operators;
using std::chrono::steady_clock;
using namespace std::literals::chrono_literals;

constexpr auto use_nothrow_awaitable = asio::experimental::as_tuple(asio::use_awaitable);

awaitable<void> transfer(tcp::socket& from, tcp::socket& to, steady_clock::time_point& deadline)
{
  std::array<char, 1024> data;

  for (;;)
  {
    deadline = std::max(deadline, steady_clock::now() + 5s);

    auto [e1, n1] = co_await from.async_read_some(buffer(data), use_nothrow_awaitable);
    if (e1)
      co_return;

    auto [e2, n2] = co_await async_write(to, buffer(data, n1), use_nothrow_awaitable);
    if (e2)
      co_return;
  }
}

awaitable<void> watchdog(steady_clock::time_point& deadline)
{
  asio::steady_timer timer(co_await this_coro::executor);

  auto now = steady_clock::now();
  while (deadline > now)
  {
    timer.expires_at(deadline);
    co_await timer.async_wait(use_nothrow_awaitable);
    now = steady_clock::now();
  }
}

awaitable<void> proxy(tcp::socket client, tcp::endpoint target)
{
  tcp::socket server(client.get_executor());
  steady_clock::time_point deadline{};

  auto [e] = co_await server.async_connect(target, use_nothrow_awaitable);
  if (!e)
  {
    co_await (
        transfer(client, server, deadline) ||
        transfer(server, client, deadline) ||
        watchdog(deadline)
      );
  }
}

awaitable<void> listen(tcp::acceptor& acceptor, tcp::endpoint target)
{
  for (;;)
  {
    auto [e, client] = co_await acceptor.async_accept(use_nothrow_awaitable);
    if (e)
      break;

    auto ex = client.get_executor();
    co_spawn(ex, proxy(std::move(client), target), detached);
  }
}

int main(int argc, char* argv[])
{
  try
  {
    if (argc != 5)
    {
      std::cerr << "Usage: proxy";
      std::cerr << " <listen_address> <listen_port>";
      std::cerr << " <target_address> <target_port>\n";
      return 1;
    }

    asio::io_context ctx;

    auto listen_endpoint =
      *tcp::resolver(ctx).resolve(
          argv[1],
          argv[2],
          tcp::resolver::passive
        );

    auto target_endpoint =
      *tcp::resolver(ctx).resolve(
          argv[3],
          argv[4]
        );

    tcp::acceptor acceptor(ctx, listen_endpoint);

    co_spawn(ctx, listen(acceptor, target_endpoint), detached);

    ctx.run();
  }
  catch (std::exception& e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }
}
