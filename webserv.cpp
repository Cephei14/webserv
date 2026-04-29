#include "webserv.hpp"

static volatile sig_atomic_t signaled = 0;
static const size_t MAX_REQUEST_LINE_BYTES = 8192;
static const size_t MAX_URI_BYTES = 8192;
static const size_t MAX_HEADER_BLOCK_BYTES = 65536;

static long now_ms()
{
	struct timeval tv;
	if (gettimeofday(&tv, NULL) == -1)
		return 0;
	return static_cast<long>(tv.tv_sec) * 1000L + static_cast<long>(tv.tv_usec / 1000L);
}

static bool reap_child_blocking(pid_t pid)
{
	for (int attempt = 0; attempt < 3; ++attempt)
	{
		if (waitpid(pid, NULL, 0) == pid)
			return true;
	}
	return false;
}

static std::string to_lower_copy(const std::string& s)
{
	std::string out = s;
	for (size_t i = 0; i < out.size(); ++i)
		out[i] = static_cast<char>(tolower(out[i]));
	return out;
}

static void log_post_upload_success(int client_fd, const HTTPRequest& req, const std::string& raw_response)
{
	if (req.getMethod() == "POST" &&
		(raw_response.find("HTTP/1.1 200 ") == 0 || raw_response.find("HTTP/1.1 201 ") == 0))
		std::cout << "client fd " << client_fd << " uploaded successfully" << std::endl;
}

static std::string trim_copy(const std::string& s)
{
	size_t start = s.find_first_not_of(" \t");
	if (start == std::string::npos)
		return "";
	size_t end = s.find_last_not_of(" \t");
	return s.substr(start, end - start + 1);
}

static std::string normalize_host_header_value(const std::string& host_value)
{
	std::string host = trim_copy(host_value);
	if (host.empty())
		return "";
	if (host[0] == '[')
	{
		size_t bracket_end = host.find(']');
		if (bracket_end != std::string::npos)
			host = host.substr(1, bracket_end - 1);
		return to_lower_copy(host);
	}
	size_t colon = host.find(':');
	if (colon != std::string::npos)
		host = host.substr(0, colon);
	return to_lower_copy(host);
}

static std::string uri_path_without_query(const std::string& uri)
{
	size_t q = uri.find('?');
	if (q == std::string::npos) {
		return uri;
	}
	return uri.substr(0, q);
}

static size_t location_match_prefix_length(const std::string& path, const std::string& location_path)
{
	if (location_path.empty())
		return 0;
	std::string prefix = location_path;
	if (prefix.size() > 1 && prefix[prefix.size() - 1] == '/') {
		prefix.erase(prefix.size() - 1);
	}
	if (prefix.empty() || path.compare(0, prefix.size(), prefix) != 0) {
		return 0;
	}
	if (path.size() > prefix.size() && prefix[prefix.size() - 1] != '/' && path[prefix.size()] != '/') {
		return 0;
	}
	return prefix.size();
}

static LocationConfig* find_best_location_for_path(ServerConfig& srv, const std::string& uri)
{
	std::string path = uri;
	size_t q = path.find('?');
	if (q != std::string::npos) {
		path = path.substr(0, q);
	}

	LocationConfig* best_loc = NULL;
	size_t best_len = 0;
	for (size_t j = 0; j < srv._locations.size(); ++j)
	{
		LocationConfig& loc = srv._locations[j];
		size_t match_len = location_match_prefix_length(path, loc._path);
		if (match_len == 0) {
			continue;
		}
		if (match_len >= best_len) {
			best_len = match_len;
			best_loc = &loc;
		}
	}
	if (best_loc == NULL)
	{
		for (size_t j = 0; j < srv._locations.size(); ++j)
		{
			if (srv._locations[j]._path == "/")
			{
				best_loc = &srv._locations[j];
				break;
			}
		}
	}
	return best_loc;
}

static bool load_custom_error_page_body(const ServerConfig& srv, int status_code, std::string& body_out)
{
	std::map<int, std::string>::const_iterator it = srv._error_pages.find(status_code);
	if (it == srv._error_pages.end())
		return false;
	std::string filepath;
	bool root_has_slash = (!srv._root.empty() && srv._root[srv._root.size() - 1] == '/');
	bool page_has_slash = (!it->second.empty() && it->second[0] == '/');
	if (root_has_slash && page_has_slash)
		filepath = srv._root + it->second.substr(1);
	else if (!root_has_slash && !page_has_slash)
		filepath = srv._root + "/" + it->second;
	else
		filepath = srv._root + it->second;

	std::ifstream file(filepath.c_str());
	if (!file.is_open())
		return false;
	std::stringstream buffer;
	buffer << file.rdbuf();
	body_out = buffer.str();
	return true;
}

static std::string reason_phrase_from_status(int status)
{
	switch (status)
	{
		case 200: return "OK";
		case 201: return "Created";
		case 204: return "No Content";
		case 301: return "Moved Permanently";
		case 302: return "Found";
		case 307: return "Temporary Redirect";
		case 308: return "Permanent Redirect";
		case 400: return "Bad Request";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 411: return "Length Required";
		case 413: return "Payload Too Large";
		case 414: return "URI Too Long";
		case 500: return "Internal Server Error";
		case 501: return "Not Implemented";
		case 505: return "HTTP Version Not Supported";
		default: return "Error";
	}
}

static void parse_cgi_output_block(const std::string& cgi_output, int& status, std::string& content_type, std::string& body)
{
	status = 200;
	content_type = "text/html";
	body = cgi_output;

	size_t header_end = cgi_output.find("\r\n\r\n");
	size_t body_offset = 4;
	if (header_end == std::string::npos)
	{
		header_end = cgi_output.find("\n\n");
		body_offset = 2;
	}
	if (header_end == std::string::npos)
		return;

	std::string header_block = cgi_output.substr(0, header_end);
	body = cgi_output.substr(header_end + body_offset);

	std::stringstream ss(header_block);
	std::string line;
	while (std::getline(ss, line))
	{
		if (!line.empty() && line[line.size() - 1] == '\r')
			line.erase(line.size() - 1);
		size_t sep = line.find(':');
		if (sep == std::string::npos)
			continue;
		std::string key = to_lower_copy(trim_copy(line.substr(0, sep)));
		std::string value = trim_copy(line.substr(sep + 1));
		if (key == "content-type" && !value.empty())
			content_type = value;
		else if (key == "status")
		{
			int parsed = std::atoi(value.c_str());
			if (parsed >= 100 && parsed <= 599)
				status = parsed;
		}
	}
}

static void build_raw_http_response(const HTTPRequest& req, int status, const std::string& content_type, const std::string& body, bool keep_alive, std::string& out)
{
	std::stringstream response_stream;
	response_stream << req.getVersion() << " " << status << " " << reason_phrase_from_status(status) << "\r\n";
	response_stream << "Content-Type: " << (content_type.empty() ? "text/html" : content_type) << "\r\n";
	response_stream << "Content-Length: " << body.length() << "\r\n";
	if (keep_alive)
		response_stream << "Connection: keep-alive\r\n";
	else
		response_stream << "Connection: close\r\n";
	response_stream << "\r\n";
	response_stream << body;
	out = response_stream.str();
}

HTTPRequest::HTTPRequest() : _isParsed(false), _headersParsed(false), _parsed_request_end(0)
{}

void HTTPRequest::reset_parse_state() 
{
	_method.clear();
	_uri.clear();
	_version.clear();
	_body.clear();
	_headers.clear();
	_headersParsed = false;
	_isParsed = false;
	 _parsed_request_end = 0;
}

void HTTPRequest::clearBody()
{
	_body.clear();
}

void HTTPRequest::consume_parsed_request()
{
	_raw_buf.erase(0, _parsed_request_end);
	reset_parse_state();
	if(!_raw_buf.empty())
		Validate(_raw_buf);
}

void *get_addr_type(struct sockaddr *the_addr)
{
	if (the_addr->sa_family == AF_INET)
	{
		return &((reinterpret_cast<struct sockaddr_in*>(the_addr))->sin_addr);
	}
	return &((reinterpret_cast<struct sockaddr_in6*>(the_addr))->sin6_addr);
}

void server::start_listening(Config& servers)
{
	bool has_listener = false;
	for (size_t idx = 0; idx < servers._servers.size(); ++idx)
	{
		ServerConfig& srv = servers._servers[idx];
		if (_port_socket.find(srv._port) != _port_socket.end())
		{
			has_listener = true;
			continue;
		}
		struct addrinfo hints, *res, *p;
		int ra;
		int yes = 1;
		int listen_fd = -1;
		std::stringstream ss;
		ss << srv._port;
		std::string port = ss.str();
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE;
		if ((ra = getaddrinfo(srv._host.c_str(), port.c_str(), &hints, &res)) != 0)
		{
			std::cerr << "srv[ERROR]: getaddrinfo " << gai_strerror(ra) <<std::endl;
			continue;
		}
		for(p = res; p != NULL; p = p->ai_next)
		{
			listen_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
			if(listen_fd == -1)
			{
				std::cerr << "srv [socket] " << std::endl;
				continue;
			}
			if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes ,sizeof(int)) == -1)
			{
				std::cerr << "srv [setsockopt]" << std::endl;
				close(listen_fd);
				listen_fd = -1;
				continue;
			}
			if (bind(listen_fd, p->ai_addr, p->ai_addrlen) == -1)
			{
				close(listen_fd);
				listen_fd = -1;
				std::cerr << "srv [Bind]" << std::endl;
				continue;
			}
			break;
		}
		freeaddrinfo(res);

		if (p == NULL || listen_fd == -1)
		{
			std::cerr << "server failed to bind" << std::endl;
			continue;
		}
		if (listen(listen_fd, 256) == -1)
		{
			std::cerr << "Listen" << std::endl;
			close(listen_fd);
			continue;
		}
		if (fcntl(listen_fd, F_SETFL, O_NONBLOCK) == -1)
		{
			close(listen_fd);
			continue;
		}
		_port_socket[srv._port] = listen_fd;
		poll_setup(listen_fd);
		_listener_to_server[listen_fd] = static_cast<int>(idx);
		has_listener = true;
	}
	if (!has_listener)
		throw std::runtime_error("No valid listening sockets");
}

void server::poll_setup(int newfd)
{
	struct pollfd pfd;
	pfd.fd = newfd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	_fds.push_back(pfd);
}

void HTTPRequest::Validate(const std::string& buf)
{
	size_t end_of_headers = buf.find("\r\n\r\n");
	if (!_headersParsed)
	{
		size_t n = buf.find("\r\n");
		if(n == std::string::npos)
		{
			if (buf.size() > MAX_REQUEST_LINE_BYTES)
				throw std::runtime_error("414 URI Too Long");
			return;
		}
		if (n > MAX_REQUEST_LINE_BYTES)
			throw std::runtime_error("414 URI Too Long");
		std::string requestLine = buf.substr(0, n);
		size_t pos = requestLine.find(' ');
		if(pos == std::string::npos || pos == 0 || pos + 1 >= requestLine.size() || requestLine[pos + 1] == ' ')
			throw std::runtime_error("400 Bad Request");
		_method = requestLine.substr(0, pos);
		if (_method != "POST" && _method != "GET" && _method != "HEAD" && _method != "DELETE")
			throw std::runtime_error("501 Not Implemented");
		size_t m = requestLine.find(' ', pos + 1);
		if(m == std::string::npos || m <= pos + 1 || m + 1 >= requestLine.size() || requestLine[m + 1] == ' ')
			throw std::runtime_error("400 Bad Request");
		if (requestLine.find(' ', m + 1) != std::string::npos)
			throw std::runtime_error("400 Bad Request");
		_uri = requestLine.substr(pos + 1, m - pos - 1);
		if (_uri.empty() || _uri[0] != '/')
			throw std::runtime_error("400 Bad Request");
		if (_uri.size() > MAX_URI_BYTES)
			throw std::runtime_error("414 URI Too Long");
		_version = requestLine.substr(m + 1);
		if (_version != "HTTP/1.1")
			throw std::runtime_error("505 HTTP Version Not Supported");

		std::string allowed_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";	
		if (end_of_headers == std::string::npos)
		{
			if (buf.size() > MAX_HEADER_BLOCK_BYTES)
				throw std::runtime_error("400 Bad Request");
			return;
		}
		if (end_of_headers + 4 > MAX_HEADER_BLOCK_BYTES)
			throw std::runtime_error("400 Bad Request");
		bool has_content_length_header = false;
		std::string first_content_length_value;
		std::string Header = buf.substr(n + 2, end_of_headers - (n + 2) + 2); 
		while (Header != "\r\n" && !Header.empty()) 
		{
			size_t line_end = Header.find("\r\n");
			std::string Line = Header.substr(0, line_end);
			size_t colon = Line.find(':');
			if (colon == std::string::npos)
				throw std::runtime_error("400 Bad Request");
			std::string Key = Line.substr(0, colon);
			if (Key.empty() || Key.find_first_not_of(allowed_chars) != std::string::npos)
				throw std::runtime_error("400 Bad Request");
			for (size_t i = 0; i < Key.length(); i++)
				Key[i] = (char)tolower(Key[i]);
			std::string Value = Line.substr(colon + 1);
			size_t start = Value.find_first_not_of(" \t");
			if (start != std::string::npos)
				Value = Value.substr(start);
			size_t end = Value.find_last_not_of(" \t");
			if(end != std::string::npos)
				Value = Value.substr(0, end + 1);
			if (Key == "content-length")
			{
				if (Value.empty() || Value.find_first_not_of("0123456789") != std::string::npos)
					throw std::runtime_error("400 Bad Request");
				if (has_content_length_header && first_content_length_value != Value)
					throw std::runtime_error("400 Bad Request");
				unsigned long parsed_len = std::strtoul(Value.c_str(), NULL, 10);
				if (parsed_len > 2147483647UL)
					throw std::runtime_error("400 Bad Request");
				has_content_length_header = true;
				first_content_length_value = Value;
			}
			_headers[Key] = Value;
			Header.erase(0, line_end + 2);
		}
		if (_headers.find("host") == _headers.end())
			throw std::runtime_error("400 Bad Request");
		_headersParsed = true;
		if (_method == "POST")
		{
			std::map<std::string, std::string>::const_iterator cl_it = _headers.find("content-length");
			if (cl_it != _headers.end())
			{
				long cl = std::atol(cl_it->second.c_str());
				if (cl > 0)
					_raw_buf.reserve(end_of_headers + 4 + static_cast<size_t>(cl));
			}
		}
	}
	size_t body_start = end_of_headers + 4;
	bool has_transfer_encoding = (_headers.find("transfer-encoding") != _headers.end());
	bool has_content_length = (_headers.find("content-length") != _headers.end());
	if (_method == "POST")
	{
		if (has_transfer_encoding)
		{
			size_t chunk_end = _raw_buf.find("0\r\n\r\n", body_start);
			if (chunk_end == std::string::npos)
				return;
			_parsed_request_end = chunk_end + 5;
			_isParsed = true;
		}
		else if (has_content_length)
		{
			long content_length = std::atol(_headers["content-length"].c_str());
			size_t required_end = body_start + static_cast<size_t>(content_length);
			if(_raw_buf.size() < required_end)
				return;
			_parsed_request_end = required_end;
			_isParsed = true;
		}
		else
		    throw std::runtime_error("411 Length Required");
	}
	else
	{
		_parsed_request_end = body_start;
		_isParsed = true;
	}

	if (_isParsed)
	{
		if (_method == "POST" && _parsed_request_end > body_start)
			_body.assign(_raw_buf, body_start, _parsed_request_end - body_start);
		else
			_body.clear();
	}
}

void HTTPRequest::AddRawP(const char* line, int nbytes)
{
	_raw_buf.append(line, nbytes);
	if (!_isParsed)
		Validate(_raw_buf);
}

bool HTTPRequest::IsParsed()
{
	return(_isParsed);
}

void HTTPResponse::prepare_GET(const HTTPRequest& req, ServerConfig& srv, const LocationConfig& loc)
{
	std::string path = uri_path_without_query(req.getUri());

	if (path.find("..") != std::string::npos)
	{
	    _statusCode = 403;
	    _body = "<h1>403 Forbidden: Invalid path</h1>";
	    _contentType = "text/html";
	    construct_response(req);
	    return;
	}
	std::string root = loc._root.empty() ? srv._root : loc._root;
	std::string rel = path;
	size_t prefix_len = location_match_prefix_length(rel, loc._path);
	if (prefix_len > 0 && loc._path != "/")
	    rel = rel.substr(prefix_len);
	if (rel.empty() || rel[0] != '/')
	    rel = "/" + rel;
	std::string full_path = root + rel;

	struct stat st;
	bool is_dir = (stat(full_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
	if (is_dir)
	{
		bool index_checked = false;
		bool index_found = false;
		const std::vector<std::string>& idx = loc._index.empty() ? srv._index : loc._index;
		if (!idx.empty())
		{
			index_checked = true;
			std::string index_path = full_path;
			if (index_path[index_path.size() - 1] != '/')
				index_path += "/";
			index_path += idx[0];
			_contentType = get_content_type(index_path);
			body_GET(index_path);
			if (_statusCode != 404)
			{
				index_found = true;
				construct_response(req);
				return;
			}
		}
		if (!loc._autoindex)
		{
			if (index_checked && !index_found)
			{
				_statusCode = 404;
				if (!load_custom_error_page_body(srv, 404, _body))
					_body = "<html><body><h1>404 Not Found</h1></body></html>";
			}
			else
			{
				_statusCode = 403;
				_body = "<html><body><h1>403 Forbidden</h1></body></html>";
			}
			_contentType = "text/html";
			construct_response(req);
			return;
		}
		DIR *dir = opendir(full_path.c_str());
		if (dir == NULL)
		{
			_statusCode = 403;
			_body = "<html><body><h1>403 Forbidden</h1></body></html>";
			_contentType = "text/html";
			construct_response(req);
			return;
		}
		std::string base = path;
		if (base.empty())
			base = "/";
		if (base[base.size() - 1] != '/')
			base += "/";
		std::stringstream listing;
		listing << "<html><body><h1>Index of " << base << "</h1><ul>";
		struct dirent *entry;
		while ((entry = readdir(dir)) != NULL)
		{
			std::string name = entry->d_name;
			if (name == "." || name == "..")
				continue;
			std::string href = base + name;
			listing << "<li><a href=\"" << href << "\">" << name << "</a></li>";
		}
		closedir(dir);
		listing << "</ul></body></html>";
		_statusCode = 200;
		_body = listing.str();
		_contentType = "text/html";
		construct_response(req);
		return;
	}
	_contentType = get_content_type(full_path);
	body_GET(full_path);
	if (_statusCode == 404)
	{
		if (!load_custom_error_page_body(srv, 404, _body))
			_body = "<html><body><h1>404 Not Found</h1></body></html>";
		_contentType = "text/html";
	}
	construct_response(req);
}

const std::string& HTTPRequest::getBody() const
{
	return _body;
}

const std::string& HTTPRequest::getUri() const
{
	return(_uri);
}

const std::string& HTTPRequest::getMethod() const
{
	return(_method);
}

const std::string& HTTPRequest::getVersion() const
{
	return(_version);
}


const std::map<std::string, std::string>& HTTPRequest::getMap() const
{
	return(_headers);
}

HTTPResponse::HTTPResponse() 
	: _version("HTTP/1.1"), 
	  _data_size(0),
	  _statusCode(200), 
	  _reason(""), 
	  _body(""), 
	  _contentType("text/plain"),
	  _post_body("") ,
	  _final_response("") 
{}

const std::string& HTTPResponse::get_raw_response() const
{
	return _final_response;
}


std::string HTTPResponse::get_content_type(const std::string& uri)
{
	size_t dot_pos = uri.find_last_of('.');

	if (dot_pos == std::string::npos)
        return "application/octet-stream";
	
	std::string ext = uri.substr(dot_pos);

    if (ext == ".html" || ext == ".htm")
		return "text/html";
    if (ext == ".css")
		return "text/css";
    if (ext == ".js")
		return "application/javascript";
    if (ext == ".png")
		return "image/png";
    if (ext == ".jpg" || ext == ".jpeg")
		return "image/jpeg";
    if (ext == ".txt")
		return "text/plain";
    return "application/octet-stream";
}

void HTTPResponse::construct_response(const HTTPRequest& req)
{
	std::stringstream response_stream;
	const std::map<std::string, std::string>& header_req = req.getMap();

	if (_reason.empty()) 
	{
		if (_statusCode == 200)
			_reason = "OK";
		else if (_statusCode == 201)
			_reason = "Created";
		else if (_statusCode == 204)
			_reason = "No Content";
		else if (_statusCode == 404)
			_reason = "Not Found";
		else if (_statusCode == 400)
			_reason = "Bad Request";
		else if (_statusCode == 403)
			_reason = "Forbidden";
		else if (_statusCode == 405)
			_reason = "Method Not Allowed";
		else if (_statusCode == 411)
			_reason = "Length Required";
		else if (_statusCode == 413)
			_reason = "Payload Too Large";
		else if (_statusCode == 414)
			_reason = "URI Too Long";
		else if (_statusCode == 501)
			_reason = "Not Implemented";
		else if (_statusCode == 505)
			_reason = "HTTP Version Not Supported";
		else
			_reason = "Error";
	}
	response_stream << req.getVersion() << " " << _statusCode << " " << _reason << "\r\n";
	
	if (_contentType.empty() || _contentType == "text/plain")
		_contentType = "text/html"; // Default for error pages or fallbacks

	response_stream << "Content-Type: " << _contentType << "\r\n";
    response_stream << "Content-Length: " << _body.length() << "\r\n";

	std::map<std::string, std::string>::const_iterator it = header_req.find("connection");

	if (it != header_req.end() && it->second == "close")
	    response_stream << "Connection: close\r\n";
	else
		response_stream << "Connection: keep-alive\r\n";
	response_stream << "\r\n";
	if (req.getMethod() != "HEAD")
		response_stream << _body;
	_final_response = response_stream.str();
}

void HTTPResponse::body_GET(const std::string& path)
{
	struct stat st;
	if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
	{
		_statusCode = 404;
		return;
	}

	std::ifstream file(path.c_str(), std::ios::binary);
	if (file.is_open())
	{
		_statusCode = 200;
		_body.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		file.close();
	}
	else
		_statusCode = 404;
}

std::string HTTPResponse::handle_chunks(const HTTPRequest& req)
{
	const std::string& raw_body = req.getBody();
	std::string decoded_body;
    size_t pos = 0;

    while (pos < raw_body.length())
    {
        size_t hex_end = raw_body.find("\r\n", pos);
        if (hex_end == std::string::npos) 
            break;
        std::string hex_str = raw_body.substr(pos, hex_end - pos);
        long chunk_size = std::strtol(hex_str.c_str(), NULL, 16);
        if (chunk_size == 0) 
            break;
        pos = hex_end + 2;
        decoded_body.append(raw_body, pos, chunk_size);
        pos += chunk_size + 2; 
    }
	_data_size = decoded_body.size();
    return decoded_body;
}


void HTTPResponse::body_POST(const std::string& path, const std::string& body, size_t size)
{
	std::ofstream outfile(path.c_str(), std::ios::out | std::ios::binary);
	if(outfile.is_open())
	{
		outfile.write(body.c_str(), size);
		outfile.close();
		_statusCode = 201;
		_body = "<h1>201 Created: File Uploaded Successfully</h1>";
		_contentType = "text/html";
	}
	else
	{
		_statusCode = 403; 
		_body = "<h1>403 Forbidden: Cannot write file</h1>";
		_contentType = "text/html";
	}	
}

static bool decode_chunked_body_for_cgi(const std::string& raw, std::string& out)
{
	out.clear();
	size_t pos = 0;

	while (true)
	{
		size_t line_end = raw.find("\r\n", pos);
		if (line_end == std::string::npos)
			return(false);
		std::string hex = raw.substr(pos, line_end - pos);
		char* endptr = NULL;
		long chunk_size = std::strtol(hex.c_str(), &endptr, 16);
		if (endptr == hex.c_str() || *endptr != '\0' || chunk_size < 0)
			return(false);
		pos = line_end + 2;
		if (chunk_size == 0)
		{
			if (raw.find("\r\n", pos) == std::string::npos)
				return(false);
			return(true);
		}
		if (pos + static_cast<size_t>(chunk_size) + 2 > raw.size())
			return(false);
		out.append(raw, pos, static_cast<size_t>(chunk_size));
		pos += static_cast<size_t>(chunk_size);
		if (raw.compare(pos, 2, "\r\n") != 0)
			return(false);
		pos += 2;
	}
}

static bool get_request_body_size_for_limit(const HTTPRequest& req, size_t& size_out)
{
	const std::map<std::string, std::string>& headers = req.getMap();
	std::map<std::string, std::string>::const_iterator te_it = headers.find("transfer-encoding");
	if (te_it != headers.end())
	{
		std::string te = to_lower_copy(te_it->second);
		if (te.find("chunked") != std::string::npos)
		{
			std::string decoded_body;
			if (!decode_chunked_body_for_cgi(req.getBody(), decoded_body))
				return false;
			size_out = decoded_body.size();
			return true;
		}
	}
	size_out = req.getBody().size();
	return true;
}

static std::string to_cgi_http_header_env_key(const std::string& header_key)
{
	std::string out = "HTTP_";
	for (size_t i = 0; i < header_key.size(); ++i)
	{
		char c = header_key[i];
		if (c == '-')
			out += '_';
		else
			out += static_cast<char>(toupper(static_cast<unsigned char>(c)));
	}
	return out;
}

void HTTPResponse::prepare_POST(const HTTPRequest& req, ServerConfig& srv, const LocationConfig& loc)
{
	const std::string& request_body = req.getBody();
	std::string path = uri_path_without_query(req.getUri());
	if (path.find("..") != std::string::npos)
	{
		_statusCode = 403;
		_body = "<h1>403 Forbidden: Invalid path</h1>";
		_contentType = "text/html";
		construct_response(req);
		return;
	}
	std::string root = loc._root.empty() ? srv._root : loc._root;
	std::string rel = path;
	size_t prefix_len = location_match_prefix_length(rel, loc._path);
	if (prefix_len > 0 && loc._path != "/")
	    rel = rel.substr(prefix_len);
	if (rel.empty() || rel[0] != '/')
		rel = "/" + rel;
	std::string full_path = root + rel;
	if (rel == "/")
	{
	    const std::vector<std::string>& idx = loc._index.empty() ? srv._index : loc._index;
	    if (!idx.empty())
	        full_path += idx[0];
	}
	const std::map<std::string, std::string> &header_req = req.getMap();
	std::map<std::string, std::string>::const_iterator it = header_req.find("content-length");
	if (it != header_req.end()) 
	{
		_data_size = static_cast<size_t>(std::atoi(it->second.c_str()));
		if (_data_size > request_body.size())
			_data_size = request_body.size();
		body_POST(full_path, request_body, _data_size);
	}
	else
	{
		std::string decoded_body = handle_chunks(req);
		body_POST(full_path, decoded_body, _data_size);
	}
	construct_response(req);
}

void HTTPResponse::prepare_DELETE(const HTTPRequest& req, ServerConfig& srv, const LocationConfig& loc)
{
	std::string path = uri_path_without_query(req.getUri());
	if (path.find("..") != std::string::npos)
	{
		_statusCode = 403;
		_body = "<h1>403 Forbidden: Invalid path</h1>";
		_contentType = "text/html";
		construct_response(req);
		return;
	}
	std::string root = loc._root.empty() ? srv._root : loc._root;
	std::string rel = path;
	size_t prefix_len = location_match_prefix_length(rel, loc._path);
	if (prefix_len > 0 && loc._path != "/")
	    rel = rel.substr(prefix_len);
	if (rel.empty() || rel[0] != '/')
	    rel = "/" + rel;
	std::string full_path = root + rel;
	if (rel == "/")
	{
	    const std::vector<std::string>& idx = loc._index.empty() ? srv._index : loc._index;
	    if (!idx.empty())
	        full_path += idx[0];
	}
	if (std::remove(full_path.c_str()) == 0)
	{
		_statusCode = 204;
		_body = "";
		_contentType = "";
	}
	else if (errno == EACCES || errno == EPERM)
	{
		_statusCode = 403;
		_body = "<h1>403 Forbidden: Permission denied</h1>";
		_contentType = "text/html";
	}
	else
	{
		_statusCode = 404;
		_body = "<h1>404 Not Found: File does not exist</h1>";
		_contentType = "text/html";
	}
	construct_response(req);
}

void HTTPResponse::build(const HTTPRequest& req, ServerConfig& srv)
{
	LocationConfig* best_loc = NULL;
	size_t best_len = 0;
	std::string path = uri_path_without_query(req.getUri());
	for(size_t j = 0; j < srv._locations.size(); j++)
	{
		LocationConfig& loc = srv._locations[j];
		size_t match_len = location_match_prefix_length(path, loc._path);
		if (match_len == 0)
			continue;
		if (match_len >= best_len)
		{
			best_len = match_len;
			best_loc = &loc;
		}
	}

	if (best_loc == NULL)
	{
		for (size_t j = 0; j < srv._locations.size(); ++j)
		{
			if (srv._locations[j]._path == "/")
			{
				best_loc = &srv._locations[j];
				break;
			}
		}
	}

	if (best_loc == NULL)
	{
		_statusCode = 404;
		build_error_response(_statusCode, srv);
		return;
	}
	if (best_loc->_return_code > 0 && !best_loc->_return_url.empty())
	{
		std::stringstream redirect;
		std::string reason = "Redirect";
		if (best_loc->_return_code == 301)
    		reason = "Moved Permanently";
		else if (best_loc->_return_code == 302)
		    reason = "Found";
		else if (best_loc->_return_code == 307)
		    reason = "Temporary Redirect";
		else if (best_loc->_return_code == 308)
		    reason = "Permanent Redirect";
		redirect << req.getVersion() << " " << best_loc->_return_code << " " << reason << "\r\n";
		redirect << "Location: " << best_loc->_return_url << "\r\n";
		redirect << "Content-Length: 0\r\n";
		const std::map<std::string, std::string>& header_req = req.getMap();
		std::map<std::string, std::string>::const_iterator it = header_req.find("connection");
		if (it != header_req.end() && it->second == "close")
		    redirect << "Connection: close\r\n";
		else
		    redirect << "Connection: keep-alive\r\n";
		redirect << "\r\n";
		_final_response = redirect.str();
		return;
	}
	bool method_found = false;
	for(size_t k = 0; k < best_loc->_allowed_methods.size(); k++)
	{
		if (req.getMethod() == best_loc->_allowed_methods[k])
		{
			method_found = true;
			break;
		}
	}
	if(!method_found)
	{
		_statusCode = 405;
		build_error_response(_statusCode, srv);
		return;
	}
	bool is_cgi =false;
	if(!best_loc->_cgi_ext.empty() && !best_loc->_cgi_path.empty())
	{
		size_t dot = path.find_last_of('.');
		if(dot != std::string::npos)
		{
			std::string ext = path.substr(dot);
			for(size_t k = 0; k < best_loc->_cgi_ext.size(); k++)
			{
				if (best_loc->_cgi_ext[k] == ext)
				{
					is_cgi = true;
					break;
				}
			}
		}
	}
	if((req.getMethod() == "GET" || req.getMethod() == "HEAD") && !is_cgi)
		prepare_GET(req, srv, *best_loc);
	else if(req.getMethod() == "POST" && !is_cgi)
	{
		size_t limit = (best_loc && best_loc->_client_max_body_size > 0) 
		               ? best_loc->_client_max_body_size 
		               : srv._client_max_body_size;
		size_t body_size = 0;
		if (!get_request_body_size_for_limit(req, body_size))
		{
			_statusCode = 400;
			build_error_response(_statusCode, srv);
			return;
		}

		if(body_size <= limit)
			prepare_POST(req, srv, *best_loc);
		else
		{
			_statusCode = 413;
			build_error_response(_statusCode, srv);
		}
	}
	else if(req.getMethod() == "DELETE")
		prepare_DELETE(req, srv, *best_loc);
	else
	{
		_statusCode = 405;
		build_error_response(_statusCode, srv);
	}
}

bool server::get_keep_alive(const HTTPRequest& req) const
{
	bool keep_alive = true;
	const std::map<std::string, std::string>& headers = req.getMap();
	std::map<std::string, std::string>::const_iterator conn_it = headers.find("connection");
	if (conn_it != headers.end())
	{
		std::string conn_value = to_lower_copy(conn_it->second);
		if (conn_value == "close")
			keep_alive = false;
	}
	return keep_alive;
}

int server::resolve_server_index(int client_fd, const HTTPRequest& req, const Config& servers) const
{
	int default_idx = 0;
	std::map<int, int>::const_iterator c_it = _client_to_server.find(client_fd);
	if (c_it != _client_to_server.end() && c_it->second >= 0 && static_cast<size_t>(c_it->second) < servers._servers.size())
		default_idx = c_it->second;
	int target_port = -1;
	if (default_idx >= 0 && static_cast<size_t>(default_idx) < servers._servers.size())
		target_port = servers._servers[default_idx]._port;
	if (target_port < 0)
	{
		struct sockaddr_in local_addr;
		socklen_t local_len = sizeof(local_addr);
		if (getsockname(client_fd, reinterpret_cast<struct sockaddr*>(&local_addr), &local_len) == 0)
			target_port = ntohs(local_addr.sin_port);
	}
	if (target_port < 0)
		return 0;
	std::string host_name;
	const std::map<std::string, std::string>& headers = req.getMap();
	std::map<std::string, std::string>::const_iterator h_it = headers.find("host");
	if (h_it != headers.end())
		host_name = normalize_host_header_value(h_it->second);
	int first_on_port = -1;
	for (size_t i = 0; i < servers._servers.size(); ++i)
	{
		const ServerConfig& candidate = servers._servers[i];
		if (candidate._port != target_port)
			continue;
		if (first_on_port == -1)
			first_on_port = static_cast<int>(i);
		for (size_t j = 0; j < candidate._server_names.size(); ++j)
		{
			if (to_lower_copy(candidate._server_names[j]) == host_name)
				return static_cast<int>(i);
		}
	}
	if (default_idx >= 0 && static_cast<size_t>(default_idx) < servers._servers.size() && servers._servers[default_idx]._port == target_port)
		return default_idx;
	if (first_on_port != -1)
		return first_on_port;
	return 0;
}

void server::set_client_events(int client_fd, short events)
{
	for (size_t k = 0; k < _fds.size(); ++k)
	{
		if (_fds[k].fd == client_fd)
		{
			_fds[k].events = events;
			_fds[k].revents = 0;
			return;
		}
	}
}

bool server::is_cgi_request(const HTTPRequest& req, ServerConfig& srv, LocationConfig*& script_loc, LocationConfig*& cgi_loc, std::string& uri_path)
{
	script_loc = NULL;
	cgi_loc = NULL;
	uri_path = req.getUri();
	size_t q = uri_path.find('?');
	if (q != std::string::npos)
		uri_path = uri_path.substr(0, q);

	size_t best_len = 0;
	for (size_t j = 0; j < srv._locations.size(); ++j)
	{
		LocationConfig& loc = srv._locations[j];
		size_t match_len = location_match_prefix_length(uri_path, loc._path);
		if (match_len == 0)
			continue;
		if (match_len >= best_len)
		{
			best_len = match_len;
			script_loc = &loc;
		}
	}
	if (script_loc == NULL)
	{
		for (size_t j = 0; j < srv._locations.size(); ++j)
		{
			if (srv._locations[j]._path == "/")
			{
				script_loc = &srv._locations[j];
				break;
			}
		}
	}
	if (script_loc == NULL)
		return false;
	if (script_loc->_return_code > 0 && !script_loc->_return_url.empty())
		return false;//manage redirect somewhere else
	if (req.getMethod() != "GET" && req.getMethod() != "POST")
		return false;
	size_t dot = uri_path.find_last_of('.');
	if (dot == std::string::npos)
		return false;
	std::string ext = uri_path.substr(dot);
	for (size_t j = 0; j < srv._locations.size(); ++j)
	{
		LocationConfig& loc = srv._locations[j];
		if (loc._cgi_path.empty() || loc._cgi_ext.empty())
			continue;
		for (size_t k = 0; k < loc._cgi_ext.size(); ++k)
		{
			if (loc._cgi_ext[k] == ext)
			{
				cgi_loc = &loc;
				return true;
			}
		}
	}
	return false;
}

bool server::start_cgi_job(int client_fd, const HTTPRequest& req, ServerConfig& srv, const LocationConfig& script_loc, const LocationConfig& cgi_loc, const std::string& uri_path, int server_index)
{
	if (_cgi_jobs.find(client_fd) != _cgi_jobs.end())
		return true;

	if (uri_path.find("..") != std::string::npos)
	{
		HTTPResponse err;
		err.build_error_response(403, srv);
		_pending_response[client_fd] = err.get_raw_response();
		_fd_keep_alive[client_fd] = get_keep_alive(req);
		set_client_events(client_fd, POLLIN | POLLOUT);
		return false;
	}

	std::string root = script_loc._root.empty() ? srv._root : script_loc._root;
	std::string rel = uri_path;
	size_t prefix_len = location_match_prefix_length(rel, script_loc._path);
	if (prefix_len > 0 && script_loc._path != "/")
		rel = rel.substr(prefix_len);
	if (rel.empty() || rel[0] != '/')
		rel = "/" + rel;
	std::string script_path = root + rel;
	if (!script_path.empty() && script_path[0] != '/')
	{
		char cwd[4096];
		if (getcwd(cwd, sizeof(cwd)) != NULL)
		{
			std::string base = cwd;
			if (!base.empty() && base[base.size() - 1] != '/')
				base += "/";
			if (script_path.size() > 2 && script_path.substr(0, 2) == "./")
				script_path = base + script_path.substr(2);
			else
				script_path = base + script_path; //absolute path
		}
	}
	bool is_post = (req.getMethod() == "POST");
	if (!is_post)
	{
		struct stat script_stat;
		if (stat(script_path.c_str(), &script_stat) != 0 || S_ISDIR(script_stat.st_mode))
		{
			HTTPResponse err;
			err.build_error_response(404, srv);
			_pending_response[client_fd] = err.get_raw_response();
			_fd_keep_alive[client_fd] = get_keep_alive(req);
			set_client_events(client_fd, POLLIN | POLLOUT);
			return false;
		}
		if (access(script_path.c_str(), R_OK) != 0)
		{
			HTTPResponse err;
			err.build_error_response(403, srv);
			_pending_response[client_fd] = err.get_raw_response();
			_fd_keep_alive[client_fd] = get_keep_alive(req);
			set_client_events(client_fd, POLLIN | POLLOUT);
			return false;
		}
	}
	bool use_decoded_body = false;
	size_t request_body_size = 0;
	std::string decoded_request_body;
	if (is_post)
	{
		const std::map<std::string, std::string>& headers = req.getMap();
		bool is_chunked = false;
		std::map<std::string, std::string>::const_iterator te_it = headers.find("transfer-encoding");
		if (te_it != headers.end())
		{
			std::string te = to_lower_copy(te_it->second);
			if (te.find("chunked") != std::string::npos)
				is_chunked = true;
		}
		if (is_chunked)
		{
			if (!decode_chunked_body_for_cgi(req.getBody(), decoded_request_body))
			{
				HTTPResponse err;
				err.build_error_response(400, srv);
				_pending_response[client_fd] = err.get_raw_response();
				_fd_keep_alive[client_fd] = get_keep_alive(req);
				set_client_events(client_fd, POLLIN | POLLOUT);
				return false;
			}
			use_decoded_body = true;
			request_body_size = decoded_request_body.size();
		}
		else
			request_body_size = req.getBody().size();
	}
	int out_fd[2];
	int in_fd[2];
	in_fd[0] = -1;
	in_fd[1] = -1;
	out_fd[0] = -1;
	out_fd[1] = -1;
	if (pipe(out_fd) == -1)
	{
		HTTPResponse err;
		err.build_error_response(500, srv);
		_pending_response[client_fd] = err.get_raw_response();
		_fd_keep_alive[client_fd] = get_keep_alive(req);
		set_client_events(client_fd, POLLIN | POLLOUT);
		return false;
	}
	if (is_post && pipe(in_fd) == -1)//we never create a in_pipe if it is not a post method
	{
		close(out_fd[0]);
		close(out_fd[1]);
		HTTPResponse err;
		err.build_error_response(500, srv);
		_pending_response[client_fd] = err.get_raw_response();
		_fd_keep_alive[client_fd] = get_keep_alive(req);
		set_client_events(client_fd, POLLIN | POLLOUT);
		return false;
	}
	std::string env_method = "REQUEST_METHOD=" + req.getMethod();
	std::string env_gateway = "GATEWAY_INTERFACE=CGI/1.1";
	std::string env_server_protocol = "SERVER_PROTOCOL=" + req.getVersion();
	std::string env_query = "QUERY_STRING=";
	size_t q = req.getUri().find('?');
	if (q != std::string::npos)
		env_query += req.getUri().substr(q + 1);
	std::string env_request_uri = "REQUEST_URI=" + req.getUri();
	std::string env_script_name = "SCRIPT_NAME=" + uri_path;
	std::string env_path_info = "PATH_INFO=" + uri_path;
	std::string env_script = "SCRIPT_FILENAME=" + script_path;
	std::string env_content_length = "CONTENT_LENGTH=0";
	std::string env_content_type = "CONTENT_TYPE=";
	if (is_post)
	{
		std::stringstream ss;
		ss << request_body_size;
		env_content_length = "CONTENT_LENGTH=" + ss.str();
		const std::map<std::string, std::string>& headers = req.getMap();
		std::map<std::string, std::string>::const_iterator it = headers.find("content-type");
		if (it != headers.end())
			env_content_type = "CONTENT_TYPE=" + it->second;
	}

	std::vector<std::string> extra_http_env;
	{
		const std::map<std::string, std::string>& headers = req.getMap();
		for (std::map<std::string, std::string>::const_iterator it = headers.begin(); it != headers.end(); ++it)
		{
			if (it->first == "content-type" || it->first == "content-length")
				continue;
			extra_http_env.push_back(to_cgi_http_header_env_key(it->first) + "=" + it->second);
		}
	}
	std::vector<char*> envp;
	envp.push_back(const_cast<char*>(env_method.c_str()));
	envp.push_back(const_cast<char*>(env_gateway.c_str()));
	envp.push_back(const_cast<char*>(env_server_protocol.c_str()));
	envp.push_back(const_cast<char*>(env_query.c_str()));
	envp.push_back(const_cast<char*>(env_request_uri.c_str()));
	envp.push_back(const_cast<char*>(env_script_name.c_str()));
	envp.push_back(const_cast<char*>(env_path_info.c_str()));
	envp.push_back(const_cast<char*>(env_script.c_str()));
	envp.push_back(const_cast<char*>(env_content_length.c_str()));
	envp.push_back(const_cast<char*>(env_content_type.c_str()));
	for (size_t i = 0; i < extra_http_env.size(); ++i)
		envp.push_back(const_cast<char*>(extra_http_env[i].c_str()));
	envp.push_back(NULL);
	char* args[3];
	std::string abs_cgi_path = cgi_loc._cgi_path;
	if (!abs_cgi_path.empty() && abs_cgi_path[0] != '/')
	{
		char cwd[4096];
		if (getcwd(cwd, sizeof(cwd)) != NULL)
		{
			std::string base = cwd;
			if (!base.empty() && base[base.size() - 1] != '/')
				base += "/";
			if (abs_cgi_path.size() > 2 && abs_cgi_path.substr(0, 2) == "./")
				abs_cgi_path = base + abs_cgi_path.substr(2);
			else
				abs_cgi_path = base + abs_cgi_path;
		}
	}
	args[0] = const_cast<char*>(abs_cgi_path.c_str());//e.g python
	args[1] = const_cast<char*>(script_path.c_str());//script
	args[2] = NULL;
	pid_t pid = fork();
	if (pid == -1)
	{
		close(out_fd[0]);
		close(out_fd[1]);
		if (is_post)
		{
			close(in_fd[0]);
			close(in_fd[1]);
		}
		HTTPResponse err;
		err.build_error_response(500, srv);
		_pending_response[client_fd] = err.get_raw_response();
		_fd_keep_alive[client_fd] = get_keep_alive(req);
		set_client_events(client_fd, POLLIN | POLLOUT);
		return false;
	}
	//Child -> POST (read & write <=> in_fd[0] & out_fd[1])
	//Child -> GET (write <=> out_fd[1])
	//Parent -> POST (read & write <=> out_fd[0] & in_fd[1])
	//Parent -> GET (read <=> out_fd[0])
	if (pid == 0)
	{
		if (is_post)
		{
			close(in_fd[1]);//child want to read data from parent, save result to the HD and write data to parent using out_fd[1]
			if (dup2(in_fd[0], STDIN_FILENO) == -1)
				_exit(1);
			close(in_fd[0]);
		}
		else
		{
			int null_fd = open("/dev/null", O_RDONLY);
			if (null_fd == -1)
				_exit(1);
			if (dup2(null_fd, STDIN_FILENO) == -1)
				_exit(1);
			close(null_fd);
		}
		close(out_fd[0]);//child will not read anything to the parent using out_fd
		if (dup2(out_fd[1], STDOUT_FILENO) == -1)
			_exit(1);
		close(out_fd[1]);
		size_t slash = script_path.find_last_of('/');
		std::string script_dir = ".";
		if (slash != std::string::npos)
		{
			script_dir = script_path.substr(0, slash);
			if (script_dir.empty())
				script_dir = "/";
		}
		if (chdir(script_dir.c_str()) == -1)
			_exit(1);
		execve(args[0], args, &envp[0]);
		_exit(1);
	} //child ended
	close(out_fd[1]); //closing child ends
	if (is_post)
		close(in_fd[0]);
	if (fcntl(out_fd[0], F_SETFL, O_NONBLOCK) == -1)
	{
		close(out_fd[0]);//closing parent ends
		if (is_post)
			close(in_fd[1]);
		kill(pid, SIGKILL);
		reap_child_blocking(pid);
		HTTPResponse err;
		err.build_error_response(500, srv);
		_pending_response[client_fd] = err.get_raw_response();
		_fd_keep_alive[client_fd] = get_keep_alive(req);
		set_client_events(client_fd, POLLIN | POLLOUT);
		return false;
	}
	if (is_post && fcntl(in_fd[1], F_SETFL, O_NONBLOCK) == -1)
	{
		close(out_fd[0]);
		close(in_fd[1]);
		kill(pid, SIGKILL);
		reap_child_blocking(pid);
		HTTPResponse err;
		err.build_error_response(500, srv);
		_pending_response[client_fd] = err.get_raw_response();
		_fd_keep_alive[client_fd] = get_keep_alive(req);
		set_client_events(client_fd, POLLIN | POLLOUT);
		return false;
	}
	CgiJob job;
	job.client_fd = client_fd;
	job.server_index = server_index;
	job.pid = pid;
	job.in_fd = is_post ? in_fd[1] : -1;
	job.out_fd = out_fd[0];
	job.write_offset = 0;
	job.request_body_size = request_body_size;
	job.use_decoded_body = use_decoded_body;
	if (job.use_decoded_body)
		job.request_body.swap(decoded_request_body);
	job.output.clear();
	job.start_ms = now_ms();
	job.child_done = false;
	job.child_status = 0;
	if (job.in_fd != -1 && job.request_body_size == 0) //POST without a body
	{
		close(job.in_fd);
		job.in_fd = -1;
	}
	_cgi_jobs[client_fd] = job;
	if (job.in_fd != -1)
	{
		struct pollfd in_pfd;
		in_pfd.fd = job.in_fd;
		in_pfd.events = POLLOUT;
		in_pfd.revents = 0;
		_fds.push_back(in_pfd);
		_cgi_in_to_client[job.in_fd] = client_fd;
	}
	struct pollfd out_pfd;
	out_pfd.fd = job.out_fd;
	out_pfd.events = POLLIN;
	out_pfd.revents = 0;
	_fds.push_back(out_pfd);
	_cgi_out_to_client[job.out_fd] = client_fd;
	set_client_events(client_fd, 0);
	return true;
}

void server::cleanup_cgi_job(int client_fd, bool kill_child)
{
	std::map<int, CgiJob>::iterator it = _cgi_jobs.find(client_fd);
	if (it == _cgi_jobs.end())
		return;

	CgiJob& job = it->second;
	if (kill_child && job.pid > 0 && !job.child_done)
	{
		kill(job.pid, SIGKILL);
		job.child_done = reap_child_blocking(job.pid);
	}

	int fds_to_disable[2];
	fds_to_disable[0] = job.in_fd;
	fds_to_disable[1] = job.out_fd;
	for (int n = 0; n < 2; ++n)
	{
		int fd = fds_to_disable[n];
		if (fd == -1)
			continue;
		close(fd);
		_cgi_in_to_client.erase(fd);
		_cgi_out_to_client.erase(fd);
		for (size_t k = 0; k < _fds.size(); ++k)
		{
			if (_fds[k].fd == fd)
			{
				_fds[k].fd = -1;
				_fds[k].events = 0;
				_fds[k].revents = 0;
				break;
			}
		}
	}
	job.in_fd = -1;
	job.out_fd = -1;
}

void server::finalize_cgi_job(int client_fd, Config& servers, bool success, int status_code)
{
	std::map<int, CgiJob>::iterator it = _cgi_jobs.find(client_fd);
	if (it == _cgi_jobs.end())
		return;
	CgiJob& job = it->second;

	std::map<int, HTTPRequest>::iterator req_it = _requests.find(client_fd);
	if (req_it == _requests.end())
	{
		cleanup_cgi_job(client_fd, !job.child_done);
		_cgi_jobs.erase(client_fd);
		return;
	}

	bool keep_alive = get_keep_alive(req_it->second);
	std::string raw;
	if (success)
	{
		int final_status = 200;
		std::string final_content_type = "text/html";
		std::string final_body;
		parse_cgi_output_block(job.output, final_status, final_content_type, final_body);
		build_raw_http_response(req_it->second, final_status, final_content_type, final_body, keep_alive, raw);
		log_post_upload_success(client_fd, req_it->second, raw);
	}
	else
	{
		HTTPResponse err;
		err.build_error_response(status_code, servers._servers[job.server_index]);
		raw = err.get_raw_response();
	}

	_pending_response[client_fd] = raw;
	_fd_keep_alive[client_fd] = keep_alive;
	set_client_events(client_fd, POLLIN | POLLOUT);

	cleanup_cgi_job(client_fd, !job.child_done);
	_cgi_jobs.erase(client_fd);
}

void server::process_cgi_pipe_event(size_t& i, Config& servers)
{
	if (i >= _fds.size())
		return;
	int fd = _fds[i].fd;
	bool is_in_pipe = (_cgi_in_to_client.find(fd) != _cgi_in_to_client.end());
	bool is_out_pipe = (_cgi_out_to_client.find(fd) != _cgi_out_to_client.end());
	if (!is_in_pipe && !is_out_pipe)
		return;
	int client_fd = -1;
	if (is_in_pipe)
		client_fd = _cgi_in_to_client[fd];
	else
		client_fd = _cgi_out_to_client[fd];
	std::map<int, CgiJob>::iterator it = _cgi_jobs.find(client_fd);
	if (it == _cgi_jobs.end())
	{
		close(fd);
		_cgi_in_to_client.erase(fd);
		_cgi_out_to_client.erase(fd);
		_fds[i].fd = -1;
		_fds[i].events = 0;
		_fds[i].revents = 0;
		return;
	}
	CgiJob& job = it->second;
	short revents = _fds[i].revents;
	if (is_in_pipe)
	{
		if (revents & (POLLERR | POLLHUP | POLLNVAL))
		{
			finalize_cgi_job(client_fd, servers, false, 500);
			return;
		}
		if (revents & POLLOUT)
		{
			const char* write_buf = NULL;
			size_t write_size = job.request_body_size;
			if (job.use_decoded_body)
				write_buf = job.request_body.c_str();
			else
			{
				std::map<int, HTTPRequest>::iterator req_it = _requests.find(client_fd);
				if (req_it == _requests.end())
				{
					finalize_cgi_job(client_fd, servers, false, 500);
					return;
				}
				const std::string& req_body = req_it->second.getBody();
				if (req_body.size() < write_size)
					write_size = req_body.size();
				write_buf = req_body.c_str();
			}

			if (job.write_offset < write_size)
			{
				ssize_t w = write(fd, write_buf + job.write_offset, write_size - job.write_offset);
				if (w <= 0)
				{
					finalize_cgi_job(client_fd, servers, false, 500);
					return;
				}
				job.write_offset += static_cast<size_t>(w);
			}
			if (job.write_offset >= write_size)
			{
				close(fd);
				_cgi_in_to_client.erase(fd);
				job.in_fd = -1;
				_fds[i].fd = -1;
				_fds[i].events = 0;
				_fds[i].revents = 0;
			}
		}
	}
	else
	{
		if (revents & POLLIN)
		{
			char buffer[BUFF_SIZE];
			ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
			if (bytes_read > 0)
				job.output.append(buffer, static_cast<size_t>(bytes_read));
			else if (bytes_read == 0)
			{
				close(fd);
				_cgi_out_to_client.erase(fd);
				job.out_fd = -1;
				_fds[i].fd = -1;
				_fds[i].events = 0;
				_fds[i].revents = 0;
			}
			else
			{
				finalize_cgi_job(client_fd, servers, false, 500);
				return;
			}
		}
		if (revents & (POLLERR | POLLNVAL))
		{
			finalize_cgi_job(client_fd, servers, false, 500);
			return;
		}
		if ((revents & POLLHUP) && !(revents & POLLIN))
		{
			close(fd);
			_cgi_out_to_client.erase(fd);
			job.out_fd = -1;
			_fds[i].fd = -1;
			_fds[i].events = 0;
			_fds[i].revents = 0;
		}
	}

	if (!job.child_done)
	{
		pid_t w = waitpid(job.pid, &job.child_status, WNOHANG);
		if (w == -1)
		{
			finalize_cgi_job(client_fd, servers, false, 500);
			return;
		}
		if (w == job.pid)
			job.child_done = true;
	}

	if (now_ms() - job.start_ms > 300000)
	{
		finalize_cgi_job(client_fd, servers, false, 500);
		return;
	}

	if (job.child_done && job.in_fd == -1 && job.out_fd == -1)
	{
		bool success = (WIFEXITED(job.child_status) && WEXITSTATUS(job.child_status) == 0);
		finalize_cgi_job(client_fd, servers, success, 500);
	}
}

void server::close_connection(size_t& i)
{
	if (i >= _fds.size())
		return;

	int fd = _fds[i].fd;
	if (_listener_to_server.find(fd) != _listener_to_server.end())
		return;

	if (_cgi_jobs.find(fd) != _cgi_jobs.end())
	{
		cleanup_cgi_job(fd, true);
		_cgi_jobs.erase(fd);
	}

	close(fd);
	_requests.erase(fd);
	_client_to_server.erase(fd);
	_responses.erase(fd);
	_fds.erase(_fds.begin() + i);
	_pending_response.erase(fd);
	_fd_keep_alive.erase(fd);
	i--;
}

void HTTPResponse::build_error_response(int status_code, ServerConfig& srv)
{
	std::stringstream response;
    std::string reason_phrase;
    std::string body_str;
    bool custom_page_loaded = false;

	switch(status_code)
	{
		case 400: reason_phrase = "Bad Request"; break;
        case 403: reason_phrase = "Forbidden"; break;
        case 404: reason_phrase = "Not Found"; break;
        case 405: reason_phrase = "Method Not Allowed"; break;
        case 411: reason_phrase = "Length Required"; break;
        case 413: reason_phrase = "Payload Too Large"; break;
		case 414: reason_phrase = "URI Too Long"; break;
        case 500: reason_phrase = "Internal Server Error"; break;
        case 501: reason_phrase = "Not Implemented"; break;
        case 505: reason_phrase = "HTTP Version Not Supported"; break;
		default: reason_phrase = "Error"; break;
	}
	std::map<int, std::string>::iterator it = srv._error_pages.find(status_code);
	std::string filepath;
	if (it != srv._error_pages.end())
	{
		bool r_slash = (!srv._root.empty() && srv._root[srv._root.size() - 1] == '/');
		bool p_slash = (!it->second.empty() && it->second[0] == '/');
		if (r_slash && p_slash)
			filepath = srv._root + it->second.substr(1);
		else if (!r_slash && !p_slash)
			filepath = srv._root + "/" + it->second;
		else	
			filepath = srv._root + it->second;
        
        std::ifstream file(filepath.c_str());
        if (file.is_open()) 
        {
            std::stringstream buffer;
            buffer << file.rdbuf();
            body_str = buffer.str();
            custom_page_loaded = true;
            file.close();
        }
    }
	if (!custom_page_loaded)
	{
		std::stringstream ss;
		ss << "<html>\r\n";
        ss << "<head><title>" << status_code << " " << reason_phrase << "</title></head>\r\n";
        ss << "<body>\r\n";
        ss << "<center><h1>" << status_code << " " << reason_phrase << "</h1></center>\r\n";
        ss << "<hr><center>webserv/1.0</center>\r\n";
        ss << "</body>\r\n";
        ss << "</html>\r\n";
        body_str = ss.str();
	}
	response << "HTTP/1.1 " << status_code << " " << reason_phrase << "\r\n";
    response << "Content-Type: text/html\r\n";
    response << "Content-Length: " << body_str.length() << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << body_str;
    this->_final_response = response.str();
}

void server::srv_manage(Config& servers)
{
	while(!signaled)
	{
		if (_fds.empty())
			break;
		if (poll(&_fds[0], _fds.size(), TOUT) == -1)
		{
			if (errno == EINTR)
				continue;
			std::cerr << "Poll" << std::endl;
			continue;
		}
		std::vector<int> timed_out_clients;
		long now = now_ms();
		for (std::map<int, CgiJob>::iterator jt = _cgi_jobs.begin(); jt != _cgi_jobs.end(); ++jt)
		{
			if (now - jt->second.start_ms > 300000)
				timed_out_clients.push_back(jt->first);
		}
		for (size_t t = 0; t < timed_out_clients.size(); ++t)
			finalize_cgi_job(timed_out_clients[t], servers, false, 500);
		size_t i = 0;
		for(i = 0; i < _fds.size(); i++)
		{
			if (_fds[i].fd < 0) //connection closed
				continue;
			if (_cgi_in_to_client.find(_fds[i].fd) != _cgi_in_to_client.end() || _cgi_out_to_client.find(_fds[i].fd) != _cgi_out_to_client.end()) //pipe
			{
				process_cgi_pipe_event(i, servers);
				continue;
			}
			if (_fds[i].revents & POLLIN) //client
			{
				struct sockaddr_storage the_addr;
				socklen_t len = sizeof the_addr;
				char s[INET6_ADDRSTRLEN];
				int client_fd = _fds[i].fd;
				std::map<int, int>::iterator listener_it = _listener_to_server.find(client_fd); //socket-server map
				bool is_listener = (listener_it != _listener_to_server.end());

				if (is_listener) //new client
				{
					int cnxfd = accept(client_fd, reinterpret_cast<sockaddr*>(&the_addr), &len);
					if (cnxfd == -1)
					{
						std::cerr << "Failed to connect " << std::endl;
						continue;
					}
					if (fcntl(cnxfd, F_SETFL, O_NONBLOCK) == -1)
					{
						close(cnxfd);
						continue;
					}
					poll_setup(cnxfd);
					_requests[cnxfd] = HTTPRequest();
					_client_to_server[cnxfd] = listener_it->second;
					inet_ntop(the_addr.ss_family, get_addr_type(reinterpret_cast<sockaddr*>(&the_addr)), s, sizeof s);
					std::cout << "srv: new connection from " << s << " on fd " << cnxfd << " (Total clients: " << _fds.size() - 1 << ")" << std::endl;
				}
				if (!is_listener) //exiting client
				{
					if (_cgi_jobs.find(client_fd) != _cgi_jobs.end())
						continue;
					char buf[BUFF_SIZE];
					int nbytes = recv(client_fd, buf, BUFF_SIZE - 1, 0);
					if (nbytes < 0)
					{
						std::cerr << "srv: recv error (hard failure) on fd " <<  client_fd << std::endl;
						close_connection(i);
						std::cout << "srv: client disconnected. (Total clients: " << _fds.size() - 1 << ")" << std::endl;
						continue;
					}
					if (nbytes == 0)
					{
						std::cout << "srv: socket " <<  client_fd << " hung up." << std::endl;
						close_connection(i);
						std::cout << "srv: client disconnected. (Total clients: " << _fds.size() - 1 << ")" << std::endl;
						continue;
					}
					else
					{
						HTTPRequest& current_req = _requests[client_fd];
						try
						{
							current_req.AddRawP(buf, nbytes);
							if (current_req.getMethod() == "POST" && !current_req.IsParsed())
							{
								const std::map<std::string, std::string>& hdrs = current_req.getMap();
								std::map<std::string, std::string>::const_iterator cl_it = hdrs.find("content-length");
								if (cl_it != hdrs.end())
								{
									unsigned long declared_len = std::strtoul(cl_it->second.c_str(), NULL, 10);
									int s_idx = resolve_server_index(client_fd, current_req, servers);
									ServerConfig& selected_srv = servers._servers[s_idx];
									size_t limit = selected_srv._client_max_body_size;
									LocationConfig* matched_loc = find_best_location_for_path(selected_srv, current_req.getUri());
									LocationConfig* script_loc = NULL;
									LocationConfig* cgi_loc = NULL;
									std::string cgi_uri_path;
									bool is_cgi = is_cgi_request(current_req, selected_srv, script_loc, cgi_loc, cgi_uri_path);
									if (!is_cgi)
									{
										if (matched_loc != NULL && matched_loc->_client_max_body_size > 0)
											limit = matched_loc->_client_max_body_size;
										if (declared_len > limit)
										{
											HTTPResponse too_large_res;
											too_large_res.build_error_response(413, selected_srv);
											_pending_response[client_fd] = too_large_res.get_raw_response();
											_fd_keep_alive[client_fd] = false;
											_fds[i].events = POLLOUT;
											continue;
										}
									}
								}
							}
						}
						catch(const std::exception& e)
						{
						    int status_code = std::atoi(e.what()); 
						
						    if (status_code < 400 || status_code > 599)
						        status_code = 500;
						    std::cerr << "Request Failed: " << e.what() << std::endl;
						
						    HTTPResponse error_res;
							int s_idx = resolve_server_index(client_fd, current_req, servers);
						    error_res.build_error_response(status_code, servers._servers[s_idx]);
							_pending_response[client_fd] = error_res.get_raw_response();
							_fd_keep_alive[client_fd] = false;
							_fds[i].events = POLLOUT;
						}
						if (current_req.IsParsed() && _pending_response.find(client_fd) == _pending_response.end() && _cgi_jobs.find(client_fd) == _cgi_jobs.end())
						{
							int s_idx = resolve_server_index(client_fd, current_req, servers);
							LocationConfig* script_loc = NULL;
							LocationConfig* cgi_loc = NULL;
							std::string uri_path;
							if (is_cgi_request(current_req, servers._servers[s_idx], script_loc, cgi_loc, uri_path))
							{
								size_t body_size = 0;
								if (!get_request_body_size_for_limit(current_req, body_size))
								{
									_responses[client_fd].build_error_response(400, servers._servers[s_idx]);
									_pending_response[client_fd] = _responses[client_fd].get_raw_response();
									set_client_events(client_fd, POLLIN | POLLOUT);
									continue;
								}
								if (cgi_loc != NULL && cgi_loc->_client_max_body_size > 0 && body_size > cgi_loc->_client_max_body_size)
								{
									_responses[client_fd].build_error_response(413, servers._servers[s_idx]);
									_pending_response[client_fd] = _responses[client_fd].get_raw_response();
									set_client_events(client_fd, POLLIN | POLLOUT);
									continue;
								}
								start_cgi_job(client_fd, current_req, servers._servers[s_idx], *script_loc, *cgi_loc, uri_path, s_idx);
								current_req.clearBody();
								continue;
							}
							HTTPResponse new_response;
							new_response.build(current_req, servers._servers[s_idx]);
							_pending_response[client_fd] = new_response.get_raw_response();
							log_post_upload_success(client_fd, current_req, _pending_response[client_fd]);
							_fd_keep_alive[client_fd] = get_keep_alive(current_req);
							_fds[i].events = POLLIN | POLLOUT;
						}
					}
				}
			}
			else if(_fds[i].revents & POLLOUT)
			{
				int client_fd = _fds[i].fd;
				std::map<int, std::string>::iterator p_it = _pending_response.find(client_fd);
				std::map<int, bool>::iterator k_it = _fd_keep_alive.find(client_fd);
				bool keep_alive = (k_it != _fd_keep_alive.end() && k_it->second);
				if (p_it == _pending_response.end())
				{
					_fds[i].events = POLLIN;
					continue;
				}
				if (p_it->second.empty())
				{
					if (keep_alive)
					{
						_pending_response.erase(client_fd);
						_fd_keep_alive.erase(client_fd);
						_requests[client_fd].consume_parsed_request();
						if(_requests[client_fd].IsParsed() == true)
						{
							int s_idx = resolve_server_index(client_fd, _requests[client_fd], servers);
							LocationConfig* script_loc = NULL;
							LocationConfig* cgi_loc = NULL;
							std::string uri_path;
							if (is_cgi_request(_requests[client_fd], servers._servers[s_idx], script_loc, cgi_loc, uri_path))
							{
								start_cgi_job(client_fd, _requests[client_fd], servers._servers[s_idx], *script_loc, *cgi_loc, uri_path, s_idx);
								_requests[client_fd].clearBody();
								continue;
							}
							_responses[client_fd].build(_requests[client_fd], servers._servers[s_idx]);
							_pending_response[client_fd] = _responses[client_fd].get_raw_response();
							log_post_upload_success(client_fd, _requests[client_fd], _pending_response[client_fd]);
							keep_alive = get_keep_alive(_requests[client_fd]);
							_fd_keep_alive[client_fd] = keep_alive;
							_fds[i].events = POLLIN | POLLOUT;
						}
						else
							_fds[i].events = POLLIN;
					}
					else
					{
						close_connection(i);
						std::cout << "srv: response sent and connection closed." << std::endl;
					}
					continue;
				}
				int bytes_sent = send(client_fd, p_it->second.c_str(), p_it->second.size(), 0);
				if (bytes_sent > 0)
					p_it->second.erase(0, static_cast<size_t>(bytes_sent));
				else if (bytes_sent < 0)
				{
					close_connection(i);
					continue;
				}
				if (p_it->second.empty())
				{
					if (keep_alive)
					{
						_pending_response.erase(client_fd);
						_fd_keep_alive.erase(client_fd);
						_requests[client_fd].consume_parsed_request();
						if(_requests[client_fd].IsParsed() == true)
						{
							int s_idx = resolve_server_index(client_fd, _requests[client_fd], servers);
							LocationConfig* script_loc = NULL;
							LocationConfig* cgi_loc = NULL;
							std::string uri_path;
							if (is_cgi_request(_requests[client_fd], servers._servers[s_idx], script_loc, cgi_loc, uri_path))
							{
								start_cgi_job(client_fd, _requests[client_fd], servers._servers[s_idx], *script_loc, *cgi_loc, uri_path, s_idx);
								_requests[client_fd].clearBody();
								continue;
							}
							_responses[client_fd].build(_requests[client_fd], servers._servers[s_idx]);
							_pending_response[client_fd] = _responses[client_fd].get_raw_response();
							log_post_upload_success(client_fd, _requests[client_fd], _pending_response[client_fd]);
							keep_alive = get_keep_alive(_requests[client_fd]);
							_fd_keep_alive[client_fd] = keep_alive;
							_fds[i].events = POLLIN | POLLOUT;
						}
						else
							_fds[i].events = POLLIN;
					}
					else
					{
						close_connection(i);
						std::cout << "srv: response sent and connection closed." << std::endl;
					}
				}
			}
		}
		for (size_t k = 0; k < _fds.size();)
		{
			if (_fds[k].fd < 0)
				_fds.erase(_fds.begin() + k);
			else
				++k;
		}
	}
	for (std::map<int, CgiJob>::iterator it = _cgi_jobs.begin(); it != _cgi_jobs.end(); ++it)
	{
		kill(it->second.pid, SIGKILL);
		reap_child_blocking(it->second.pid);
	}
	for(size_t i = 0; i < _fds.size(); i++)
	{
		int fd = _fds[i].fd;
		if (fd >= 0)
			close(fd);
	}
	_fds.clear();
	_requests.clear();
	_responses.clear();
	_client_to_server.clear();
	_listener_to_server.clear();
	_port_socket.clear();
	_pending_response.clear();
	_fd_keep_alive.clear();
	_cgi_jobs.clear();
	_cgi_in_to_client.clear();
	_cgi_out_to_client.clear();
	return;
}

void AppManager::signal_handler(int s)
{
	(void)s;
	signaled = 1;
}

void AppManager::run(Config& servers)
{
	struct sigaction sa;
	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags=0;
	if (sigaction(SIGTERM, &sa, NULL) == -1)
		throw std::runtime_error("SIGACTION FAILED");
	signal(SIGPIPE, SIG_IGN);
	if (sigaction(SIGINT, &sa, NULL) == -1)
		throw std::runtime_error("SIGACTION FAILED");
	server serv;
	serv.start_listening(servers);
	serv.srv_manage(servers);
}

std::vector<std::string> tokenize(const std::string& config_data)
{
	std::vector<std::string> tokens;
	std::string current;
	for (size_t i = 0; i < config_data.length(); ++i)
	{
		char c = config_data[i];
		if (c == '{' || c == '}' || c == ';')
		{
			if (!current.empty())
			{
				tokens.push_back(current);
				current.clear();
			}
			tokens.push_back(std::string(1, c));
		}
		else if (std::isspace(static_cast<unsigned char>(c)))
		{
			if (!current.empty())
			{
				tokens.push_back(current);
				current.clear();
			}
		}
		else
			current += c;
	}
	if (!current.empty())
		tokens.push_back(current);
	return tokens;
}

ServerConfig::ServerConfig() : _port(8080), _client_max_body_size(1048576)
{}

LocationConfig::LocationConfig() : _autoindex(false), _return_code(0), _client_max_body_size(0)
{}

size_t parse_size(const std::string& s)
{
	if (s.empty())
		return 0;
	char unit = static_cast<char>(toupper(static_cast<unsigned char>(s[s.length() - 1])));
	size_t multiplier = 1;

	if (unit == 'K')
		multiplier = 1024;
	else if (unit == 'M')
		multiplier = 1024 * 1024;
	else if (unit == 'G')
		multiplier = 1024 * 1024 * 1024;
	return (static_cast<size_t>(std::atoll(s.c_str()) * multiplier));
}

void parse_config(const std::vector<std::string>& tokens, Config& final_config)
{
	for (size_t i = 0; i < tokens.size(); ++i)
	{
		if (tokens[i] == "server")
		{
			i++;
			if (i < tokens.size() && tokens[i] == "{")
				i++;
			ServerConfig serv;
			while (i < tokens.size() && tokens[i] != "}")
			{
				if (tokens[i] == "listen")
				{
					if (++i < tokens.size())
						serv._port = std::atoi(tokens[i].c_str());
				}
				else if (tokens[i] == "host")
				{
					if (++i < tokens.size())
						serv._host = tokens[i];
				}
				else if (tokens[i] == "server_name")
				{
					while (++i < tokens.size() && tokens[i] != ";")
						serv._server_names.push_back(tokens[i]);
				}
				else if (tokens[i] == "client_max_body_size")
				{
					if (++i < tokens.size())
						serv._client_max_body_size = parse_size(tokens[i]); 
				}
				else if (tokens[i] == "error_page")
				{
					std::vector<std::string> tmp;
					while (++i < tokens.size() && tokens[i] != ";")
						tmp.push_back(tokens[i]);
					if (tmp.size() >= 2) 
					{
						std::string path = tmp.back();
						for (size_t j = 0; j < tmp.size() - 1; ++j)
							serv._error_pages[std::atoi(tmp[j].c_str())] = path;
					}
				}
				else if (tokens[i] == "root")
				{
					if (++i < tokens.size())
						serv._root = tokens[i];
				}
				else if (tokens[i] == "index")
				{
					while (++i < tokens.size() && tokens[i] != ";")
						serv._index.push_back(tokens[i]);
				}
				else if (tokens[i] == "location")
				{
					LocationConfig loc;
					if (++i < tokens.size())
						loc._path = tokens[i];
					if (++i < tokens.size() && tokens[i] == "{")
						i++;
					while (i < tokens.size() && tokens[i] != "}")
					{
						if (tokens[i] == "root")
						{
							if (++i < tokens.size())
								loc._root = tokens[i];
						}
						else if (tokens[i] == "allow_methods")
						{
							while (++i < tokens.size() && tokens[i] != ";")
								loc._allowed_methods.push_back(tokens[i]);
						}
						else if (tokens[i] == "index")
						{
							while (++i < tokens.size() && tokens[i] != ";")
								loc._index.push_back(tokens[i]);
						}
						else if (tokens[i] == "cgi_path")
						{
							if (++i < tokens.size())
								loc._cgi_path = tokens[i];
						}
						else if (tokens[i] == "client_max_body_size")
						{
							if (++i < tokens.size())
								loc._client_max_body_size = parse_size(tokens[i]);
						}
						else if (tokens[i] == "cgi_extension")
						{
							while (++i < tokens.size() && tokens[i] != ";")
								loc._cgi_ext.push_back(tokens[i]);
						}
						else if (tokens[i] == "autoindex")
						{
							if(i < tokens.size())
							{
								if(++i < tokens.size() && tokens[i] == "on")
									loc._autoindex = true;
								else
									loc._autoindex = false;
							}
						}
						else if (tokens[i] == "return")
						{
							if (++i < tokens.size())
								loc._return_code = std::atoi(tokens[i].c_str());
							if (++i < tokens.size())
								loc._return_url = tokens[i];
						}
						else
							throw std::runtime_error("Unknown directive in location: " + tokens[i]);

						if (i < tokens.size() && tokens[i] == ";")
							i++;
						else if (i + 1 < tokens.size() && tokens[i + 1] == ";")
							i += 2;
						else
							i++;
					}
					serv._locations.push_back(loc);
					if (i < tokens.size() && tokens[i] == "}")
						i++;
					continue;
				}
				else
					throw std::runtime_error("Unknown directive in config: " + tokens[i]);

				if (i < tokens.size() && tokens[i] == ";")
					i++;
				else if (i + 1 < tokens.size() && tokens[i + 1] == ";")
					i += 2;
				else
					i++;
			}
			for (size_t j = 0; j < serv._locations.size(); ++j)
			{
				if (serv._locations[j]._root.empty())
					serv._locations[j]._root = serv._root;
				if (serv._locations[j]._index.empty())
					serv._locations[j]._index = serv._index;
			}
			if (serv._index.empty())
			{
				for (size_t j = 0; j < serv._locations.size(); ++j)
				{
					if (serv._locations[j]._path == "/" && !serv._locations[j]._index.empty())
					{
						serv._index = serv._locations[j]._index;
						break;
					}
				}
			}
			if (serv._host.empty())
				serv._host = "127.0.0.1";
			final_config._servers.push_back(serv);
		}
	}
}

void ConfigManager(char **argv, Config& servers)
{
    std::string filepath = argv[1];
    std::ifstream cnf(filepath.c_str());
    
    if (cnf.is_open())
    {
        std::ostringstream ss;
        ss << cnf.rdbuf();
        std::string config_data = ss.str();
        cnf.close();

        size_t h = config_data.find("#");
        while (h != std::string::npos)
        {
            size_t n = config_data.find("\n", h);
            
            if (n == std::string::npos) 
			{
                config_data.erase(h); 
                break;
			}
            else 
                config_data.erase(h, n - h); 
            h = config_data.find("#", h);
        }
		parse_config(tokenize(config_data), servers);
    }
    else
        throw std::runtime_error("Configuration file Error");
}

int main(int argc, char **argv)
{
	if(argc != 2)
	{
		std::cerr << "Configuration file Error" << std::endl;
		return 1;
	}
	try
	{
		Config servers;
		ConfigManager(argv, servers);
		AppManager start;
		start.run(servers);
	}
	catch(const std::exception& e)
	{
		std::cerr << e.what() << std::endl;
		return 1;
	}
    return 0;
}
