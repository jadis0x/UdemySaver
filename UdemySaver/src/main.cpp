#include "server/HttpServer.h"
#include "server/RequestHandler.h"
#include <boost/asio.hpp>
#include <iostream>
#include <memory>

int main() {
    try {
        boost::asio::io_context ioc;

        auto handler = std::make_shared<RequestHandler>("www");
        auto server = std::make_shared<HttpServer>(ioc, 8080, handler);
        server->run();

        std::cout << "Listening on http://127.0.0.1:8080\n";
        ioc.run();
    }
    catch (const std::exception& e) {
        std::cout << "fatal: " << e.what() << "\n";
        return 1;
    }
}
