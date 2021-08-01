#include <iostream>
#include <string>
#include <asio.hpp>
#include <asio/experimental/awaitable_operators.hpp>

using asio::awaitable;
using asio::buffer;
using asio::co_spawn;
using asio::detached;
using asio::use_awaitable;
namespace this_coro = asio::this_coro;
using namespace asio::experimental::awaitable_operators;
using std::chrono::steady_clock;
using namespace std::literals::chrono_literals;

template <typename CompletionToken>
auto async_wait_for_signal(
    asio::signal_set& sigset,
    CompletionToken&& token)
{
  return asio::async_initiate<CompletionToken,
    void(std::error_code, std::string)>(
      [&sigset](auto handler)
      {
        auto cancellation_slot =
          asio::get_associated_cancellation_slot(
              handler,
              asio::cancellation_slot()
            );

        if (cancellation_slot.is_connected())
        {
          cancellation_slot.assign(
              [&sigset](asio::cancellation_type /*type*/)
              {
                sigset.cancel();
              }
            );
        }

        auto executor =
          asio::get_associated_executor(
              handler,
              sigset.get_executor()
            );

        auto intermediate_handler =
          [handler = std::move(handler)](
              std::error_code error,
              int signo
            ) mutable
          {
            std::string signame;
            switch (signo)
            {
            case SIGABRT: signame = "SIGABRT"; break;
            case SIGFPE: signame = "SIGFPE"; break;
            case SIGILL: signame = "SIGILL"; break;
            case SIGINT: signame = "SIGINT"; break;
            case SIGSEGV: signame = "SIGSEGV"; break;
            case SIGTERM: signame = "SIGTERM"; break;
            default: signame = "<other>"; break;
            }

            std::move(handler)(error, signame);
          };

        sigset.async_wait(
            asio::bind_executor(
              executor,
              std::move(intermediate_handler)
            )
          );
      },
      token
    );
}

awaitable<void> timed_wait_for_signal()
{
  asio::signal_set sigset(co_await this_coro::executor, SIGINT, SIGTERM);
  asio::steady_timer timer(co_await this_coro::executor, 5s);

  auto result = co_await (
      async_wait_for_signal(sigset, use_awaitable) ||
      timer.async_wait(use_awaitable)
    );

  switch (result.index())
  {
  case 0:
    std::cout << "signal finished first: " << std::get<0>(result) << "\n";
    break;
  case 1:
    std::cout << "timer finished first\n";
    break;
  }
}

int main()
{
  asio::io_context ctx;
  co_spawn(ctx, timed_wait_for_signal(), detached);
  ctx.run();
}
