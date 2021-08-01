#include <array>
#include <iostream>
#include <memory>
#include <asio.hpp>
#include <asio/experimental/parallel_group.hpp>

using asio::buffer;
using asio::ip::tcp;
using std::chrono::steady_clock;
using namespace std::literals::chrono_literals;

class proxy
  : public std::enable_shared_from_this<proxy>
{
public:
  proxy(tcp::socket client)
    : client_(std::move(client)),
      server_(client_.get_executor()),
      watchdog_timer_(client_.get_executor()),
      heartbeat_timer_(client_.get_executor())
  {
  }

  void connect_to_server(tcp::endpoint target)
  {
    auto self = shared_from_this();
    server_.async_connect(
        target,
        [self](std::error_code error)
        {
          if (!error)
          {
            self->read_from_client();
            self->read_from_server();
            self->watchdog();
          }
        }
      );
  }

private:
  void stop()
  {
    client_.close();
    server_.close();
    watchdog_timer_.cancel();
  }

  bool is_stopped() const
  {
    return !client_.is_open() && !server_.is_open();
  }

  void read_from_client()
  {
    deadline_ = std::max(deadline_, steady_clock::now() + 5s);

    auto self = shared_from_this();
    client_.async_read_some(
        buffer(data_from_client_),
        [self](std::error_code error, std::size_t n)
        {
          if (!error)
          {
            self->write_to_server(n);
          }
          else
          {
            self->stop();
          }
        }
      );
  }

  void write_to_server(std::size_t n)
  {
    auto self = shared_from_this();
    async_write(
        server_,
        buffer(data_from_client_, n),
        [self](std::error_code error, std::size_t /*n*/)
        {
          if (!error)
          {
            self->read_from_client();
          }
          else
          {
            self->stop();
          }
        }
      );
  }

  void read_from_server()
  {
    heartbeat_timer_.expires_after(1s);

    auto self = shared_from_this();
    asio::experimental::make_parallel_group(
        [this](auto token)
        {
          return server_.async_read_some(
              buffer(data_from_server_),
              token
            );
        },
        [this](auto token)
        {
          return heartbeat_timer_.async_wait(token);
        }
      ).async_wait(
        asio::experimental::wait_for_one(),
        [self](
          std::array<std::size_t, 2> order,
          std::error_code read_error, std::size_t n,
          std::error_code /*timer_error*/)
        {
          switch (order[0])
          {
          case 0: // read
            if (!read_error)
            {
              self->num_heartbeats_ = 0;
              self->write_to_client(n);
            }
            else
            {
              self->stop();
            }
            break;
          case 1: // timer
            ++self->num_heartbeats_;
            self->write_heartbeat_to_client();
            break;
          }
        }
      );
  }

  void write_to_client(std::size_t n)
  {
    auto self = shared_from_this();
    async_write(
        client_,
        buffer(data_from_server_, n),
        [self](std::error_code error, std::size_t /*n*/)
        {
          if (!error)
          {
            self->read_from_server();
          }
          else
          {
            self->stop();
          }
        }
      );
  }

  void write_heartbeat_to_client()
  {
    std::size_t n = asio::buffer_copy(
        buffer(data_from_server_),
        std::array<asio::const_buffer, 3>{
          buffer("<heartbeat "),
          buffer(std::to_string(num_heartbeats_)),
          buffer(">\r\n")
        }
      );

    write_to_client(n);
  }

  void watchdog()
  {
    auto self = shared_from_this();
    watchdog_timer_.expires_at(deadline_);
    watchdog_timer_.async_wait(
        [self](std::error_code /*error*/)
        {
          if (!self->is_stopped())
          {
            auto now = steady_clock::now();
            if (self->deadline_ > now)
            {
              self->watchdog();
            }
            else
            {
              self->stop();
            }
          }
        }
      );
  }

  tcp::socket client_;
  tcp::socket server_;
  std::array<char, 1024> data_from_client_;
  std::array<char, 1024> data_from_server_;
  steady_clock::time_point deadline_;
  asio::steady_timer watchdog_timer_;
  asio::steady_timer heartbeat_timer_;
  std::size_t num_heartbeats_ = 0;
};

void listen(tcp::acceptor& acceptor, tcp::endpoint target)
{
  acceptor.async_accept(
      [&acceptor, target](std::error_code error, tcp::socket client)
      {
        if (!error)
        {
          std::make_shared<proxy>(
              std::move(client)
            )->connect_to_server(target);
        }

        listen(acceptor, target);
      }
    );
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

    listen(acceptor, target_endpoint);

    ctx.run();
  }
  catch (std::exception& e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }
}
