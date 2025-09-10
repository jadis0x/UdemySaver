#pragma once
#define _WIN32_WINNT 0x0A00 // For Win10; asio iocp features

#include <boost/asio.hpp>
#include <memory>

class RequestHandler; // forward

class HttpServer : public std::enable_shared_from_this<HttpServer> {
public:
    HttpServer(boost::asio::io_context& ioc,
               unsigned short port,
               std::shared_ptr<RequestHandler> handler);

    void run();

private:
    void do_accept();

    boost::asio::ip::tcp::acceptor acceptor_;
    std::shared_ptr<RequestHandler> handler_;
};
