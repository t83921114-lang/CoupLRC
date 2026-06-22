/**
 * Two-node ASIO TCP write bandwidth microbenchmark.
 *
 * Server (node A):
 *   asio_write_bench server [bind_ip] port
 *
 * Client (node B):
 *   asio_write_bench client host port chunk_size_bytes iters [warmup_iters]
 *
 * Example (64 MiB chunks, 100 timed writes after 10 warmup):
 *   ./asio_write_bench server 0.0.0.0 55000
 *   ./asio_write_bench client 172.16.0.234 55000 67108864 100 10
 */
#include <asio.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace
{
  using clock = std::chrono::steady_clock;

  void die(const std::string &msg)
  {
    std::cerr << msg << std::endl;
    std::exit(1);
  }

  void set_common_socket_options(asio::ip::tcp::socket &socket)
  {
    socket.set_option(asio::ip::tcp::no_delay(true));
  }

  void run_server(const std::string &bind_ip, unsigned short port)
  {
    asio::io_context io_context;
    asio::ip::tcp::acceptor acceptor(
        io_context,
        asio::ip::tcp::endpoint(asio::ip::make_address(bind_ip), port));
    acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));

    std::cout << "Listening on " << bind_ip << ":" << port << std::endl;
    asio::ip::tcp::socket socket(io_context);
    acceptor.accept(socket);
    set_common_socket_options(socket);

    asio::error_code ec;
    std::vector<char> buf(8 * 1024 * 1024);
    std::uint64_t total_bytes = 0;
    while (true)
    {
      std::size_t n = asio::read(
          socket,
          asio::buffer(buf),
          asio::transfer_at_least(1),
          ec);
      if (ec == asio::error::eof)
      {
        break;
      }
      if (ec)
      {
        die("server read failed: " + ec.message());
      }
      total_bytes += n;
    }

    asio::error_code ignore_ec;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
    socket.close(ignore_ec);

    std::cout << "Received " << total_bytes << " bytes ("
              << (static_cast<double>(total_bytes) / 1024.0 / 1024.0)
              << " MiB)" << std::endl;
  }

  void run_client(const std::string &host,
                  unsigned short port,
                  std::size_t chunk_size,
                  int iters,
                  int warmup_iters)
  {
    if (chunk_size == 0 || iters <= 0 || warmup_iters < 0)
    {
      die("invalid client arguments: chunk_size > 0, iters > 0, warmup_iters >= 0");
    }

    std::vector<char> buf(chunk_size);
    for (std::size_t i = 0; i < chunk_size; ++i)
    {
      buf[i] = static_cast<char>(0x5A ^ (i & 0xFF));
    }

    asio::io_context io_context;
    asio::ip::tcp::resolver resolver(io_context);
    asio::ip::tcp::socket socket(io_context);
    asio::connect(socket, resolver.resolve(host, std::to_string(port)));
    set_common_socket_options(socket);

    std::cout << "Connected to " << host << ":" << port << std::endl;
    std::cout << "chunk_size=" << chunk_size
              << " warmup_iters=" << warmup_iters
              << " measure_iters=" << iters << std::endl;

    auto write_once = [&](asio::error_code &error) {
      asio::write(socket, asio::buffer(buf.data(), buf.size()), error);
    };

    asio::error_code ec;
    for (int i = 0; i < warmup_iters; ++i)
    {
      write_once(ec);
      if (ec)
      {
        die("warmup write failed: " + ec.message());
      }
    }

    std::vector<double> per_iter_seconds;
    per_iter_seconds.reserve(static_cast<size_t>(iters));

    const auto total_t0 = clock::now();
    for (int i = 0; i < iters; ++i)
    {
      const auto t0 = clock::now();
      write_once(ec);
      const auto t1 = clock::now();
      if (ec)
      {
        die("measure write failed: " + ec.message());
      }
      per_iter_seconds.push_back(
          std::chrono::duration<double>(t1 - t0).count());
    }
    const auto total_t1 = clock::now();

    asio::error_code ignore_ec;
    socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignore_ec);
    socket.close(ignore_ec);

    const double total_seconds =
        std::chrono::duration<double>(total_t1 - total_t0).count();
    const double chunk_mb =
        static_cast<double>(chunk_size) / 1024.0 / 1024.0;
    const double total_mb = chunk_mb * static_cast<double>(iters);

    std::sort(per_iter_seconds.begin(), per_iter_seconds.end());
    const double sum = std::accumulate(
        per_iter_seconds.begin(), per_iter_seconds.end(), 0.0);
    const double avg = sum / static_cast<double>(iters);
    const double median =
        per_iter_seconds[static_cast<size_t>(iters / 2)];

    std::cout << "\n=== ASIO write benchmark result ===" << std::endl;
    std::cout << "chunk_size_bytes: " << chunk_size << std::endl;
    std::cout << "measure_iters: " << iters << std::endl;
    std::cout << "write_time_total_s: " << total_seconds << std::endl;
    std::cout << "write_time_avg_s: " << avg << std::endl;
    std::cout << "write_time_median_s: " << median << std::endl;
    std::cout << "write_time_min_s: " << per_iter_seconds.front() << std::endl;
    std::cout << "write_time_max_s: " << per_iter_seconds.back() << std::endl;
    std::cout << "write_throughput_total_MBps: "
              << (total_mb / total_seconds) << std::endl;
    std::cout << "write_throughput_avg_MBps: " << (chunk_mb / avg) << std::endl;
    std::cout << "write_throughput_median_MBps: "
              << (chunk_mb / median) << std::endl;
  }

  void print_usage(const char *argv0)
  {
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " server [bind_ip=0.0.0.0] port\n"
        << "  " << argv0
        << " client host port chunk_size_bytes iters [warmup_iters=10]\n"
        << "\nExample:\n"
        << "  " << argv0 << " server 0.0.0.0 55000\n"
        << "  " << argv0 << " client 172.16.0.234 55000 67108864 100 10\n";
  }
} // namespace

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    print_usage(argv[0]);
    return 1;
  }

  const std::string mode = argv[1];
  if (mode == "server")
  {
    if (argc < 3)
    {
      print_usage(argv[0]);
      return 1;
    }
    std::string bind_ip = "0.0.0.0";
    unsigned short port = 0;
    if (argc == 3)
    {
      port = static_cast<unsigned short>(std::stoi(argv[2]));
    }
    else
    {
      bind_ip = argv[2];
      port = static_cast<unsigned short>(std::stoi(argv[3]));
    }
    run_server(bind_ip, port);
    return 0;
  }

  if (mode == "client")
  {
    if (argc < 6)
    {
      print_usage(argv[0]);
      return 1;
    }
    const std::string host = argv[2];
    const unsigned short port = static_cast<unsigned short>(std::stoi(argv[3]));
    const std::size_t chunk_size = static_cast<std::size_t>(std::stoull(argv[4]));
    const int iters = std::stoi(argv[5]);
    const int warmup_iters = (argc > 6) ? std::stoi(argv[6]) : 10;
    run_client(host, port, chunk_size, iters, warmup_iters);
    return 0;
  }

  print_usage(argv[0]);
  return 1;
}
