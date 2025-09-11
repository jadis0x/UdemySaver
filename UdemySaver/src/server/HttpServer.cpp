#include "HttpServer.h"
#include "RequestHandler.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <fstream>
#include <sstream>
#include <iostream>

namespace beast = boost::beast;
namespace http = beast::http;
using     tcp = boost::asio::ip::tcp;

static std::string read_file(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return {};
	std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

static const char* guess_type(const std::string& p) {
	if (p.size() >= 3 && p.rfind(".js") == p.size() - 3) return "application/javascript";
	if (p.size() >= 4 && p.rfind(".css") == p.size() - 4) return "text/css; charset=utf-8";
	if (p.size() >= 5 && p.rfind(".html") == p.size() - 5) return "text/html; charset=utf-8";
	if (p.size() >= 5 && p.rfind(".json") == p.size() - 5) return "application/json";
	if (p.size() >= 4 && p.rfind(".png") == p.size() - 4) return "image/png";
	if (p.size() >= 4 && p.rfind(".jpg") == p.size() - 4) return "image/jpeg";
	return "application/octet-stream";
}

static int get_int_param(const std::string& target, const char* key, int defv) {
	auto qpos = target.find('?');
	if (qpos == std::string::npos) return defv;
	std::string qs = target.substr(qpos + 1);
	std::istringstream ss(qs);
	std::string kv;
	while (std::getline(ss, kv, '&')) {
		auto eq = kv.find('=');
		if (eq == std::string::npos) continue;
		auto k = kv.substr(0, eq), v = kv.substr(eq + 1);
		if (k == key) {
			try { return std::stoi(v); }
			catch (...) { return defv; }
		}
	}
	return defv;
}

class Session : public std::enable_shared_from_this<Session> {
public:
	Session(tcp::socket sock, std::shared_ptr<RequestHandler> h)
		: socket_(std::move(sock)), handler_(std::move(h)) {
	}

	void run() {
		auto self = shared_from_this();
		http::async_read(socket_, buffer_, req_,
						 [self](beast::error_code ec, std::size_t) {
							 if (ec) {
								 self->shutdown();
							 }
							 else {
								 self->handle();
							 }
						 });
	}

private:
	void handle() {
		using status = http::status;

		try {
			// GET /
			if (req_.method() == http::verb::get && req_.target() == "/") {
				std::string body = read_file(handler_->webroot() + "/index.html");
				http::response<http::string_body> res{
					body.empty() ? status::not_found : status::ok, req_.version()
				};
				res.set(http::field::server, "beast");
				res.set(http::field::content_type, "text/html; charset=utf-8");
				res.body() = body.empty() ? "index.html yok" : std::move(body);
				res.prepare_payload();
				return write(std::move(res));
			}
			
			// HttpServer.cpp, handle() içinde uygun yere ekle
			if (req_.method() == http::verb::post && req_.target() == "/settings") {
				auto [st, body] = handler_->handleSettingsUpdate(req_.body());
				http::response<http::string_body> res{ st, req_.version() };
				res.set(http::field::server, "beast");
				res.set(http::field::content_type, "application/json");
				res.body() = std::move(body);
				res.prepare_payload();
				return write(std::move(res));
			}

			// GET /courses?page=&page_size=
			if (req_.method() == http::verb::get &&
				std::string(req_.target()).rfind("/courses", 0) == 0) {

				std::string target = std::string(req_.target());
				int page = get_int_param(target, "page", 1);
				int page_size = get_int_param(target, "page_size", 12);

				auto [st, body] = handler_->handleCourses(page, page_size);
				http::response<http::string_body> res{ st, req_.version() };
				res.set(http::field::server, "beast");
				res.set(http::field::content_type, "application/json");
				res.body() = std::move(body);
				res.prepare_payload();
				return write(std::move(res));
			}

			// GET /session  -> RequestHandler tarafı
			if (req_.method() == http::verb::get && req_.target() == "/session") {
				auto [st, body] = handler_->handleSession();
				http::response<http::string_body> res{ st, req_.version() };
				res.set(http::field::server, "beast");
				res.set(http::field::content_type, "application/json");
				res.body() = std::move(body);
				res.prepare_payload();
				return write(std::move(res));
			}

			if (req_.method() == http::verb::post && req_.target() == "/queue") {
				auto [st, body] = handler_->handleQueueAdd(req_.body());
				http::response<http::string_body> res{ st, req_.version() };
				res.set(http::field::server, "beast");
				res.set(http::field::content_type, "application/json");
				res.body() = std::move(body);
				res.prepare_payload();
				return write(std::move(res));
			}

			if (req_.method() == http::verb::post && req_.target() == "/queue/pause") {
				auto [st, body] = handler_->handleQueuePause(req_.body());
				http::response<http::string_body> res{ st, req_.version() };
				res.set(http::field::server, "beast");
				res.set(http::field::content_type, "application/json");
				res.body() = std::move(body);
				res.prepare_payload();
				return write(std::move(res));
			}

			if (req_.method() == http::verb::post && req_.target() == "/queue/resume") {
				auto [st, body] = handler_->handleQueueResume(req_.body());
				http::response<http::string_body> res{ st, req_.version() };
				res.set(http::field::server, "beast");
				res.set(http::field::content_type, "application/json");
				res.body() = std::move(body);
				res.prepare_payload();
				return write(std::move(res));
			}

			// GET /api/lectures?courseId=&page=&page_size=
			if (req_.method() == http::verb::get &&
				(std::string(req_.target()).rfind("/lectures", 0) == 0 ||
				std::string(req_.target()).rfind("/api/lectures", 0) == 0)) {

				std::string target = std::string(req_.target());
				auto get_param = [&](const char* key)->std::string {
					auto qpos = target.find('?');
					if (qpos == std::string::npos) return {};
					std::string qs = target.substr(qpos + 1);
					std::istringstream ss(qs);
					std::string kv;
					while (std::getline(ss, kv, '&')) {
						auto eq = kv.find('=');
						if (eq == std::string::npos) continue;
						auto k = kv.substr(0, eq), v = kv.substr(eq + 1);
						if (k == key) return v;
					}
					return {};
					};

				int course_id = 0;
				{
					std::string cs = get_param("course_id");
					if (cs.empty()) cs = get_param("courseId");
					if (!cs.empty()) try { course_id = std::stoi(cs); }
					catch (...) {}
				}

				int page = 1;
				{
					std::string ps = get_param("page");
					if (!ps.empty()) try { page = std::stoi(ps); }
					catch (...) {}
				}

				int page_size = 100;
				{
					std::string ps = get_param("page_size");
					if (!ps.empty()) try { page_size = std::stoi(ps); }
					catch (...) {}
				}

				auto [st, body] = handler_->handleLectures(course_id, page, page_size);
				http::response<http::string_body> res{ st, req_.version() };
				res.set(http::field::server, "beast");
				res.set(http::field::content_type, "application/json");
				res.body() = std::move(body);
				res.prepare_payload();
				return write(std::move(res));
			}

			// GET /queue
			if (req_.method() == http::verb::get && req_.target() == "/queue") {
				auto [st, body] = handler_->handleQueueList();
				http::response<http::string_body> res{ st, req_.version() };
				res.set(http::field::server, "beast");
				res.set(http::field::content_type, "application/json");
				res.body() = std::move(body);
				res.prepare_payload();
				return write(std::move(res));
			}

			if (req_.method() == http::verb::get &&
				std::string(req_.target()).rfind("/estimate", 0) == 0) {

				auto [st, body] = handler_->handleEstimate(std::string(req_.target()));
				http::response<http::string_body> res{ st, req_.version() };
				res.set(http::field::server, "beast");
				res.set(http::field::content_type, "application/json");
				res.body() = std::move(body);
				res.prepare_payload();
				return write(std::move(res));
			}

			// GET /reconcile?course_id=&title=
			if (req_.method() == http::verb::get &&
				std::string(req_.target()).rfind("/reconcile", 0) == 0) {
				std::string target = std::string(req_.target());
				// paramları çek (course_id ve title)
				// title slug/klasör adı üretimi için lazım olabilir
				auto [st, body] = handler_->handleReconcile(target);
				http::response<http::string_body> res{ st, req_.version() };
				res.set(http::field::server, "beast");
				res.set(http::field::content_type, "application/json");
				res.body() = std::move(body);
				res.prepare_payload();
				return write(std::move(res));
			}

			// Statik dosya: /www/...
			if (req_.method() == http::verb::get &&
				std::string(req_.target()).rfind("/www/", 0) == 0) {

				std::string rel = std::string(req_.target()).substr(4);
				std::string path = handler_->webroot() + rel;
				std::string body = read_file(path);

				http::response<http::string_body> res{
					body.empty() ? status::not_found : status::ok, req_.version()
				};
				res.set(http::field::server, "beast");
				res.set(http::field::content_type, guess_type(path));
				res.body() = body.empty() ? "dosya yok" : std::move(body);
				res.prepare_payload();
				return write(std::move(res));
			}

			// default 404
			http::response<http::string_body> res{ status::not_found, req_.version() };
			res.set(http::field::server, "beast");
			res.set(http::field::content_type, "text/plain; charset=utf-8");
			res.body() = "route yok";
			res.prepare_payload();
			return write(std::move(res));
		}
		catch (const std::exception& e) {
			http::response<http::string_body> res{ http::status::internal_server_error, req_.version() };
			res.set(http::field::server, "beast");
			res.set(http::field::content_type, "application/json");
			res.body() = std::string("{\"ok\":false,\"error\":\"") + e.what() + "\"}";
			res.prepare_payload();
			return write(std::move(res));
		}
	}

	void write(http::response<http::string_body>&& res) {
		auto sp = std::make_shared<http::response<http::string_body>>(std::move(res));
		auto self = shared_from_this();
		http::async_write(socket_, *sp,
						  [self, sp](beast::error_code, std::size_t) {
							  self->shutdown();
						  });
	}

	void shutdown() {
		beast::error_code ec;
		socket_.shutdown(tcp::socket::shutdown_send, ec);
	}

	tcp::socket socket_;
	beast::flat_buffer buffer_;
	http::request<http::string_body> req_;
	std::shared_ptr<RequestHandler> handler_;
};

// ------------ HttpServer ------------
HttpServer::HttpServer(boost::asio::io_context& ioc,
					   unsigned short port,
					   std::shared_ptr<RequestHandler> handler)
	: acceptor_(ioc), handler_(std::move(handler))
{
	beast::error_code ec;

	tcp::endpoint ep{ tcp::v4(), port };
	acceptor_.open(ep.protocol(), ec);
	if (ec) throw std::runtime_error("acceptor.open: " + ec.message());

	acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
	if (ec) throw std::runtime_error("acceptor.set_option: " + ec.message());

	acceptor_.bind(ep, ec);
	if (ec) throw std::runtime_error("acceptor.bind: " + ec.message());

	acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
	if (ec) throw std::runtime_error("acceptor.listen: " + ec.message());
}

void HttpServer::run() {
	do_accept();
}

void HttpServer::do_accept() {
	acceptor_.async_accept(
		[self = shared_from_this()](beast::error_code ec, tcp::socket socket) mutable {
			if (!ec) {
				std::make_shared<Session>(std::move(socket), self->handler_)->run();
			}
			else {
				std::cout << "[accept] " << ec.message() << "\n";
			}
			self->do_accept();
		}
	);
}
