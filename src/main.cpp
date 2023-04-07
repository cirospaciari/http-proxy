#include <stdlib.h>
#include <stdio.h>
#include "libusockets.h"
#include <string>
#include <string_view>
#include <queue>
#include "App.h"

/**
 *
 * Helper Functions
 *
 */

// split host and port from address
static std::pair<std::string_view, int> splitHostAndPort(std::string_view address)
{
	std::string_view host = address.substr(0, address.find(":"));
	std::string_view port = address.substr(address.find(":") + 1, address.length());
	int _port = 443;

	auto result = std::from_chars(port.data(), port.data() + port.size(), _port);
	if (result.ec == std::errc::invalid_argument)
	{
		_port = 443;
	}

	return std::make_pair(host, _port);
}

// par url protocol, hostname, port, path
static void parse_url(std::string_view url, std::string &protocol, std::string &hostname, int &port, std::string &path)
{
	protocol = "http";
	hostname = "";
	port = 80;
	path = "/";

	auto pos = url.find("://");
	if (pos != std::string::npos)
	{
		protocol = url.substr(0, pos);
		url = url.substr(pos + 3);
	}

	pos = url.find("/");
	if (pos != std::string::npos)
	{
		path = url.substr(pos);
		url = url.substr(0, pos);
	}

	pos = url.find(":");
	if (pos != std::string::npos)
	{
		hostname = url.substr(0, pos);
		auto _port = url.substr(pos + 1);
		auto result = std::from_chars(_port.data(), _port.data() + _port.size(), port);
		if (result.ec == std::errc::invalid_argument)
		{
			port = 80;
		}
	}
	else
	{
		hostname = url;
	}
}

static std::string base64_encode(const std::string &in) {

    std::string out;

    int val = 0, valb = -6;
    for (auto c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[(val>>valb)&0x3F]);
            valb -= 6;
        }
    }
    if (valb>-6) out.push_back("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[((val<<8)>>(valb+8))&0x3F]);
    while (out.size()%4) out.push_back('=');
    return out;
}

static std::string base64_decode(const std::string_view &in)
{

	std::string out;

	std::vector<int> T(256, -1);
	for (int i = 0; i < 64; i++)
		T["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = i;

	int val = 0, valb = -8;
	for (auto c : in)
	{
		if (T[c] == -1)
			break;
		val = (val << 6) + T[c];
		valb += 6;
		if (valb >= 0)
		{
			out.push_back(char((val >> valb) & 0xFF));
			valb -= 8;
		}
	}
	return out;
}

static bool handle_auth(std::string_view auth, std::string expected_auth)
{
	// remove "Basic " from auth
	if (auth.length() <= 6 || auth.substr(0, 6) != "Basic ")
	{
		return false;
	}
	auth = auth.substr(6);
	return auth == expected_auth ? true : false;
}

/**
 *
 * End of Helper Functions
 *
 **/

/**
 * Struct for storing data for a single HTTP proxy connection
 *
 */
template <bool T>
struct HttpProxyData
{
	uWS::HttpResponse<T> *res;
	bool aborted;
	bool opened;
	struct us_socket_t *socket;
	std::queue<std::pair<char *, int>> write_queue;
	std::queue<std::pair<char *, int>> read_queue;
	char *hostname;
};

template <bool T>
void cleanHttpProxyData(struct HttpProxyData<T> *ext)
{
	ext->aborted = true;

	while (ext->read_queue.size() > 0)
	{
		auto [buffer, length] = ext->read_queue.front();
		free(buffer);
		ext->read_queue.pop();
	}
	while (ext->write_queue.size() > 0)
	{
		auto [buffer, length] = ext->write_queue.front();
		free(buffer);
		ext->write_queue.pop();
	}
	if (ext->hostname)
	{
		free(ext->hostname);
		ext->hostname = NULL;
	}

	us_socket_context_free(0, us_socket_context(0, ext->socket));
}

/*
 * End of Struct for storing data for a single HTTP proxy connection
 *
 */


/**
 * Pipe events
 * 
*/
template <bool T>
static struct us_socket_t *pipe_on_data(struct us_socket_t *socket, char *data, int length)
{
	auto *ext = (struct HttpProxyData<T> *)us_socket_ext(0, socket);

	ext->res->cork([&ext, &data, length]() {
		if (!ext->opened || (length > 0 && !us_socket_write(T, (struct us_socket_t *)ext->res, data, length, 0) && !ext->aborted))
		{
			// if response is not writable, store data in buffer
			char *buffer = (char *)malloc(sizeof(char) * length);
			memcpy(buffer, data, sizeof(char) * length);
			ext->write_queue.push(std::make_pair(buffer, length));
		}
	});
	

	return socket;
}

template <bool T>
static struct us_socket_t *pipe_on_writable(struct us_socket_t *socket)
{
	auto *ext = (struct HttpProxyData<T> *)us_socket_ext(0, socket);

	// write data from buffer to socket
	while (ext->read_queue.size() > 0 && !ext->aborted)
	{
		auto [buffer, length] = ext->read_queue.front();
		if (us_socket_write(0, ext->socket, buffer, length, 0))
		{
			free(buffer);
			ext->read_queue.pop();
		}
		else
		{
			break;
		}
	}

	return socket;
}

template <bool T>
static struct us_socket_t *pipe_on_close(struct us_socket_t *s, int code, void *reason)
{
	auto *ext = (struct HttpProxyData<T> *)us_socket_ext(0, s);
	ext->res->onAborted(nullptr);
	ext->res->onRawData(nullptr);
	if (!ext->res->hasResponded())
	{
		ext->res->cork([&ext](){
			ext->res->endWithoutBody();
		});
	}
	cleanHttpProxyData(ext);
	return s;
}

template <bool T>
static struct us_socket_t *pipe_on_end(struct us_socket_t *s)
{
	return s;
}

template <bool T>
static struct us_socket_t *pipe_on_timeout(struct us_socket_t *s)
{
	auto *ext = (struct HttpProxyData<T> *)us_socket_ext(0, s);
	// This will actually trigger close
	if (ext->socket && us_socket_is_closed(0, ext->socket) == 0)
	{
		us_socket_close(0, ext->socket, 0, NULL);
	}
	return s;
}

template <bool T>
static struct us_socket_t *pipe_on_connect_error(struct us_socket_t *s, int code)
{
	auto *ext = (struct HttpProxyData<T> *)us_socket_ext(0, s);
	ext->res->onAborted(nullptr);
	ext->res->onRawData(nullptr);
	if (!ext->res->hasResponded())
	{
		ext->res->cork([&ext](){
			ext->res->writeStatus("500 Internal Server Error")->endWithoutBody();
		});
	}
	cleanHttpProxyData(ext);
	return s;
}

template <bool T>
static struct us_socket_t *pipe_connect_on_open(struct us_socket_t *s, int is_client, char *ip, int ip_length)
{
	auto *ext = (struct HttpProxyData<T> *)us_socket_ext(0, s);
	us_socket_timeout(0, s, 0);
	ext->opened = true;

	ext->res->writeStatus("200 Connection established\r\n");

	// write data from buffer to socket
	while (ext->read_queue.size() > 0 && !ext->aborted)
	{
		auto [buffer, length] = ext->read_queue.front();
		if (us_socket_write(0, s, buffer, length, 0))
		{
			free(buffer);
			ext->read_queue.pop();
		}
		else
		{
			break;
		}
	}
	return s;
}

template <bool T>
static struct us_socket_t *pipe_proxy_on_open(struct us_socket_t *s, int is_client, char *ip, int ip_length)
{
	auto *ext = (struct HttpProxyData<T> *)us_socket_ext(0, s);
	us_socket_timeout(0, s, 0);
	ext->opened = true;

	// write data from buffer to socket
	while (ext->read_queue.size() > 0 && !ext->aborted)
	{
		auto [buffer, length] = ext->read_queue.front();

		if (us_socket_write(0, s, buffer, length, 0))
		{
			free(buffer);
			ext->read_queue.pop();
		}
		else
		{
			break;
		}
	}
	return s;
}

/**
 * End of Pipe events
*/

/**
 * Response handlers
*/

template <bool T>
static void register_handlers(uWS::HttpResponse<T> *res, struct HttpProxyData<T>* ext)
{

	res->onRawData([&ext](std::string_view data)
				   { handle_raw_data(ext->res, ext, data); });
	res->onAborted([&ext]()
				   { handle_aborted(ext->res, ext); });

	res->onWritable([&res, &ext](uintmax_t offset) { return handle_on_writable(res, ext); });
}
template <bool T>
static void handle_raw_data(uWS::HttpResponse<T> *res, struct HttpProxyData<T>* ext, std::string_view data)
{
	auto length = data.length();

	// write data to socket
	if (!ext->opened || (length > 0 && !us_socket_write(0, ext->socket, data.data(), data.length(), 0)))
	{
		// if socket is not writable, store data in buffer
		char *buffer = (char *)malloc(sizeof(char) * length);
		memcpy(buffer, data.data(), sizeof(char) * length);
		ext->read_queue.push(std::make_pair(buffer, length));
	}
}

template <bool T>
static void handle_aborted(uWS::HttpResponse<T> *res, struct HttpProxyData<T>* ext)
{
	ext->aborted = true;
	// abort socket
	if (ext->socket && us_socket_is_closed(0, ext->socket) == 0)
	{
		us_socket_close(0, ext->socket, 0, NULL);
	}
}

template <bool T>
static bool handle_on_writable(uWS::HttpResponse<T> *res, struct HttpProxyData<T>* ext)
{
	auto *socket = (us_socket_t *)res;
	res->cork([&socket, &ext](){
		// write data from buffer to response
		while (ext->write_queue.size() > 0 && !ext->aborted)
		{
			auto [buffer, length] = ext->write_queue.front();
			if (us_socket_write(T, socket, buffer, length, 0))
			{
				free(buffer);
				ext->write_queue.pop();
			}
			else
			{
				break;
			}
		}
	});

	
	return false;
}
/**
 * End of Response handlers
*/

/**
 * Proxy Server for HTTP/HTTPS proxy requests and basic Authentication
 * 
*/
template <bool T>
class ProxyServer
{
private:
	std::string expected_auth;
	std::string host;
	std::string cert;
	std::string key;
	std::string passphrase;
	int port;
	uWS::TemplatedApp<T> *app;

	bool isAuthenticated(uWS::HttpResponse<T> *res, uWS::HttpRequest *req)
	{
		if (expected_auth.empty())
			return true;
		std::string_view auth = req->getHeader("proxy-authorization");

		if (auth.empty())
		{

			res->cork([&res](){
				res->writeStatus("407 Proxy Authentication Required\r\n");
				res->writeHeader("Proxy-Authenticate", "Basic realm=\"Proxy\"");
				res->endWithoutBody();
			});
			

			return false;
		}

		if (!handle_auth(auth, this->expected_auth))
		{
			res->cork([&res](){
				res->writeStatus("403 Forbidden\r\n");
				res->writeHeader("Proxy-Authenticate", "Basic realm=\"Proxy\"");
				res->endWithoutBody();
			});
			
			return false;
		}

		return true;
	}
	void connect(uWS::HttpResponse<T> *res, uWS::HttpRequest *req)
	{
		if (!isAuthenticated(res, req))
			return;

		std::string_view address = req->getFullUrl();

		// create socket context
		struct us_socket_context_options_t options;
		auto *loop = (struct us_loop_t *)uWS::Loop::get();
		struct us_socket_context_t *context = us_create_socket_context(0, loop, 0, options);

		// parse address
		auto [host, port] = splitHostAndPort(address);
		auto hostname = (char *)malloc(sizeof(char) * host.length() + 1);
		memcpy(hostname, host.data(), sizeof(char) * host.length());
		hostname[host.length()] = '\0';

		// register socket events
		us_socket_context_on_data(0, context, pipe_on_data<T>);
		us_socket_context_on_writable(0, context, pipe_on_writable<T>);
		us_socket_context_on_close(0, context, pipe_on_close<T>);
		us_socket_context_on_end(0, context, pipe_on_end<T>);
		us_socket_context_on_timeout(0, context, pipe_on_timeout<T>);
		us_socket_context_on_connect_error(0, context, pipe_on_connect_error<T>);

		// register socket open event that procedes with CONNECT request
		us_socket_context_on_open(0, context, pipe_connect_on_open<T>);
		// connect to host
		struct us_socket_t *socket = us_socket_context_connect(0, context, hostname, port, NULL, 0, sizeof(struct HttpProxyData<T>));
		if (!socket)
		{
			return res->writeStatus("500 Internal Server Error")->endWithoutBody();
		}

		// set socket extension
		auto *ext = (struct HttpProxyData<T> *)us_socket_ext(0, socket);
		memset(ext, 0, sizeof(struct HttpProxyData<T>));
		ext->res = res;
		ext->aborted = false;
		ext->opened = false;
		ext->socket = socket;
		ext->hostname = hostname;
		ext->read_queue = std::queue<std::pair<char *, int>>();
		ext->write_queue = std::queue<std::pair<char *, int>>();

		// register request handlers
		register_handlers(res, ext);
	}

	void proxyRequest(uWS::HttpResponse<T> *res, uWS::HttpRequest *req)
	{

		if (!isAuthenticated(res, req))
			return;

		// parse url
		std::string_view address = req->getFullUrl();
		std::string protocol, hostname, path;
		int port;
		parse_url(address, protocol, hostname, port, path);

		std::string_view method = req->getCaseSensitiveMethod();

		// create socket context
		struct us_socket_context_options_t options;
		auto *loop = (struct us_loop_t *)uWS::Loop::get();
		struct us_socket_context_t *context = us_create_socket_context(0, loop, 0, options);

		// register socket events
		us_socket_context_on_data(0, context, pipe_on_data<T>);
		us_socket_context_on_writable(0, context, pipe_on_writable<T>);
		us_socket_context_on_close(0, context, pipe_on_close<T>);
		us_socket_context_on_end(0, context, pipe_on_end<T>);
		us_socket_context_on_timeout(0, context, pipe_on_timeout<T>);
		us_socket_context_on_connect_error(0, context, pipe_on_connect_error<T>);

		// register socket open event that procedes with piped request
		us_socket_context_on_open(0, context, pipe_proxy_on_open<T>);

		// connect to host
		auto _hostname = (char *)malloc(sizeof(char) * hostname.length() + 1);
		_hostname[hostname.length()] = '\0';
		strcpy(_hostname, hostname.data());
		struct us_socket_t *socket = us_socket_context_connect(0, context, _hostname, port, NULL, 0, sizeof(struct HttpProxyData<T>));
		if (!socket)
		{
			res->cork([&res](){
				res->writeStatus("500 Internal Server Error")->endWithoutBody();
			});
			return;
		}
		// set socket extension
		auto *ext = (struct HttpProxyData<T> *)us_socket_ext(0, socket);
		memset(ext, 0, sizeof(struct HttpProxyData<T>));
		ext->res = res;
		ext->aborted = false;
		ext->opened = false;
		ext->socket = socket;
		ext->hostname = _hostname;
		ext->read_queue = std::queue<std::pair<char *, int>>();

		// build request to pipe it when connects
		auto read_queue = std::queue<std::pair<char *, int>>();
		auto method_length = method.length();
		auto path_length = path.length();

		auto length = method_length + 1 + path_length + 11;
		auto request = (char *)malloc(sizeof(char) * length);
		request[length] = '\0';

		memcpy(request, method.data(), method_length);
		memcpy(request + method_length, " ", 1);
		memcpy(request + method_length + 1, path.data(), path_length);
		memcpy(request + method_length + path_length + 1, " HTTP/1.1\r\n", 11);

		ext->read_queue.push(std::make_pair(request, length));

		// build headers
		for (auto [name, value] : *req)
		{

			if (name.substr(0, 6) == "proxy-" || name.empty() || value.empty())
				continue;

			auto name_length = name.length();
			auto value_length = value.length();
			auto length = name_length + value_length + 4;

			auto header = (char *)malloc(sizeof(char) * length + 1);
			header[length] = '\0';

			memcpy(header, name.data(), name_length);

			// the optimal way is not lowercase at first place
			header[0] = toupper(header[0]);
			for (int i = 1; i < name_length; i++)
			{
				if (header[i] == '-' && i + 1 < name_length)
				{
					header[i + 1] = toupper(header[i + 1]);
				}
			}

			header[name_length] = ':';
			memcpy(header + name_length, ": ", 2);
			memcpy(header + name_length + 2, value.data(), value_length);
			memcpy(header + name_length + 2 + value_length, "\r\n", 2);

			ext->read_queue.push(std::make_pair(header, length));
		}
		// build end of request
		auto end = (char *)malloc(sizeof(char) * 3);
		end[0] = '\r';
		end[1] = '\n';
		end[2] = '\0';

		ext->read_queue.push(std::make_pair(end, 2));
		ext->write_queue = std::queue<std::pair<char *, int>>();

		// register request handlers
		register_handlers(res, ext);
	}

public:
	ProxyServer(std::string expected_auth, std::string host, int port, std::string cert, std::string key, std::string passphrase) : expected_auth(expected_auth), host(host), port(port), cert(cert), key(key), passphrase(passphrase)
	{
		app = new uWS::TemplatedApp<T>({
			.key_file_name = key.data(),
			.cert_file_name = cert.data(),
			.passphrase = passphrase.data(),
		});
		app->connect("/*", [this](auto *res, auto *req) { this->connect(res, req); });
		app->any("/*", [this](auto *res, auto *req) { this->proxyRequest(res, req); });

		app->listen(host, port, [this](auto *listen_socket)
					{
			if (listen_socket) {
				this->port = us_socket_local_port(T, (struct us_socket_t*)listen_socket);
				std::cout << "Listening on port " << this->port << std::endl;
			}  else {
				std::cout << "Failed to listen on port " << this->port << std::endl;
			} });
	}

	void run()
	{
		app->run();
	}

	~ProxyServer()
	{
		delete app;
	}
};

int main(int argc, char *argv[]) { 
	// parse --cert --key --port --host --auth --passphrase from argv
	// if --cert and --key are present, run ProxyServer<true>
	std::string cert = "", key = "", host = "0.0.0.0", auth = "", passphrase = "";
	int port = 8080;

	for (int i = 1; i < argc; i++) {
		if (std::string(argv[i]) == "--cert" && i + 1 < argc) {
			cert = argv[++i];
		} else if (std::string(argv[i]) == "--key" && i + 1 < argc) {
			key = argv[++i];
		} else if (std::string(argv[i]) == "--port" && i + 1 < argc) {
			port = std::stoi(argv[++i]);
		} else if (std::string(argv[i]) == "--host" && i + 1 < argc) {
			host = argv[++i];
		} else if (std::string(argv[i]) == "--auth" && i + 1 < argc) {
			auth = argv[++i];
		} else if (std::string(argv[i]) == "--passphrase" && i + 1 < argc) {
			auth = argv[++i];
		}
	}

	if (!auth.empty()) {
		auth = base64_encode(auth);
	}

	if (!cert.empty() && !key.empty()) {
		ProxyServer<true> proxy(auth, host, port, cert, key, passphrase);
		proxy.run();
		return 0;
	}
	
	ProxyServer<false> proxy(auth, host, port, cert, key, passphrase);
	proxy.run();
	return 0;
}
