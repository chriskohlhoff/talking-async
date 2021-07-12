#include <array>
#include <iostream>
#include <memory>
#include <asio.hpp>
#include <asio/experimental/as_tuple.hpp>

using asio::awaitable;
using asio::buffer;
using asio::co_spawn;
using asio::detached;
using asio::ip::tcp;
using std::chrono::steady_clock;
using namespace std::literals::chrono_literals;

constexpr auto use_nothrow_awaitable = asio::experimental::as_tuple(asio::use_awaitable);

struct proxy_state
{
  proxy_state(tcp::socket client)
    : client(std::move(client))
  {
  }

  tcp::socket client;
  tcp::socket server{client.get_executor()};
  steady_clock::time_point deadline;
};

using proxy_state_ptr = std::shared_ptr<proxy_state>;

awaitable<void> client_to_server(proxy_state_ptr state)
{
  std::array<char, 1024> data;

  for (;;)
  {
    state->deadline = std::max(state->deadline, steady_clock::now() + 5s);

    auto [e1, n1] = co_await state->client.async_read_some(buffer(data), use_nothrow_awaitable);
    if (e1)
      break;

    auto [e2, n2] = co_await async_write(state->server, buffer(data, n1), use_nothrow_awaitable);
    if (e2)
      break;
  }

  state->client.close();
  state->server.close();
}

awaitable<void> server_to_client(proxy_state_ptr state)
{
  std::array<char, 1024> data;

  for (;;)
  {
    state->deadline = std::max(state->deadline, steady_clock::now() + 5s);

    auto [e1, n1] = co_await state->server.async_read_some(buffer(data), use_nothrow_awaitable);
    if (e1)
      break;

    auto [e2, n2] = co_await async_write(state->client, buffer(data, n1), use_nothrow_awaitable);
    if (e2)
      break;
  }

  state->client.close();
  state->server.close();
}

awaitable<void> watchdog(proxy_state_ptr state)
{
  asio::steady_timer timer(state->client.get_executor());

  auto now = steady_clock::now();
  while (state->deadline > now)
  {
    timer.expires_at(state->deadline);
    co_await timer.async_wait(use_nothrow_awaitable);
    now = steady_clock::now();
  }

  state->client.close();
  state->server.close();
}

awaitable<void> proxy(tcp::socket client, tcp::endpoint target)
{
  auto state = std::make_shared<proxy_state>(std::move(client));

  auto [e] = co_await state->server.async_connect(target, use_nothrow_awaitable);
  if (!e)
  {
    auto ex = state->client.get_executor();
    co_spawn(ex, client_to_server(state), detached);
    co_spawn(ex, server_to_client(state), detached);

    co_await watchdog(state);
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
