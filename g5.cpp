#include "g5.hpp"

static bool starts_with(const std::string& value, const std::string& prefix)
{
	return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

static bool has_path_traversal(const std::string& uri)
{
	return uri.find("..") != std::string::npos;
}

static std::string join_paths(const std::string& left, const std::string& right)
{
	if (left.empty())
		return right;
	if (right.empty())
		return left;
	if (left[left.size() - 1] == '/' && right[0] == '/')
		return left + right.substr(1);
	if (left[left.size() - 1] != '/' && right[0] != '/')
		return left + "/" + right;
	return left + right;
}

static std::string trim_location_prefix(const std::string& uri, const std::string& location_path)
{
	if (location_path.empty() || location_path == "/")
		return uri;
	if (!starts_with(uri, location_path))
		return uri;

	std::string suffix = uri.substr(location_path.size());
	if (suffix.empty())
		return "/";
	return suffix;
}

static std::string resolve_resource_path(const HTTPRequest& req, const LocationConfig& loc)
{
	std::string relative_uri = trim_location_prefix(req.getUri(), loc._path);
	return join_paths(loc._root, relative_uri);
}

static bool is_location_match(const std::string& uri, const std::string& path)
{
	if (path == "/")
		return true;
	if (!starts_with(uri, path))
		return false;
	if (uri.size() == path.size())
		return true;
	return uri[path.size()] == '/';
}

static std::string directory_name(const std::string& path)
{
	size_t pos = path.find_last_of('/');
	if (pos == std::string::npos)
		return ".";
	if (pos == 0)
		return "/";
	return path.substr(0, pos);
}

static bool is_absolute_path(const std::string& path)
{
	return !path.empty() && path[0] == '/';
}

static std::string resolve_from_config_dir(const std::string& config_dir, const std::string& path)
{
	if (path.empty() || is_absolute_path(path))
		return path;
	return join_paths(config_dir, path);
}

static void normalize_config_paths(Config& cfg, const std::string& config_path)
{
	std::string config_dir = directory_name(config_path);
	for (size_t i = 0; i < cfg._servers.size(); ++i)
	{
		ServerConfig& serv = cfg._servers[i];
		serv._root = resolve_from_config_dir(config_dir, serv._root);
		for (size_t j = 0; j < serv._locations.size(); ++j)
		{
			LocationConfig& loc = serv._locations[j];
			loc._root = resolve_from_config_dir(config_dir, loc._root);
			if (!loc._cgi_path.empty() && !is_absolute_path(loc._cgi_path))
				loc._cgi_path = resolve_from_config_dir(config_dir, loc._cgi_path);
		}
	}
}

static std::string reason_phrase(int status_code)
{
	switch (status_code)
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
		case 500: return "Internal Server Error";
		case 501: return "Not Implemented";
		default: return "Error";
	}
}

static void consume_until_semicolon(const std::vector<std::string>& tokens, size_t& i)
{
	while (i < tokens.size() && tokens[i] != ";")
		++i;
	if (i < tokens.size() && tokens[i] == ";")
		++i;
}

static std::string build_autoindex_page(const std::string& full_path, const std::string& uri)
{
	DIR* dir = opendir(full_path.c_str());
	if (dir == NULL)
		return "";

	std::stringstream ss;
	std::string base_uri = uri;
	if (base_uri.empty())
		base_uri = "/";
	if (base_uri[base_uri.size() - 1] != '/')
		base_uri += "/";

	ss << "<html><body><h1>Index of " << base_uri << "</h1><ul>";
	struct dirent* entry;
	while ((entry = readdir(dir)) != NULL)
	{
		std::string name = entry->d_name;
		if (name == "." || name == "..")
			continue;
		ss << "<li><a href=\"" << base_uri << name << "\">" << name << "</a></li>";
	}
	ss << "</ul></body></html>";

	closedir(dir);
	return ss.str();
}

HTTPRequest::HTTPRequest() : _isParsed(false), _headersParsed(false)
{}

void *get_addr_type(struct sockaddr *the_addr)
{
	if (the_addr->sa_family == AF_INET)
	{
		return &((reinterpret_cast<struct sockaddr_in*>(the_addr))->sin_addr);
	}
	return &((reinterpret_cast<struct sockaddr_in6*>(the_addr))->sin6_addr);
}

void server::start_listening(char **argv)
{
	_config = ConfigManager(argv);
	if (_config._servers.empty())
		throw std::runtime_error("Configuration file Error");
	for(size_t i = 0; i < _config._servers.size(); i++)
	{
		struct addrinfo hints, *res, *p;
		int ra;
		int yes = 1;
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE;

		std::stringstream ss;
		ss << _config._servers[i]._port;
		if ((ra = getaddrinfo(_config._servers[i]._host.c_str(), ss.str().c_str(), &hints, &res)) != 0)
		{
			std::stringstream err;
			err << "srv[ERROR]: getaddrinfo " << gai_strerror(ra);
			throw std::runtime_error(err.str());
		}
		for(p = res; p != NULL; p = p->ai_next)
		{
			int fd;
			if((fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
			{
				std::cerr << "srv [socket] " << std::endl;
				continue;
			}
			if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes ,sizeof(int)) == -1)
			{
				std::cerr << "srv [setsockopt]" << std::endl;
				close(fd);
				continue;
			}
			if (bind(fd, p->ai_addr, p->ai_addrlen) == -1)
			{
				close(fd);
				std::cerr << "srv [Bind]" << std::endl;
				continue;
			}
			_listen_socks.push_back(fd);
			break;
		}
		freeaddrinfo(res);

		if (p == NULL)
		{
			throw std::runtime_error("server failed to bind");
		}
		if (listen(_listen_socks.back(), 10) == -1)
		{
			close(_listen_socks.back());
			throw std::runtime_error("Listen");
		}
		if (fcntl(_listen_socks.back(), F_SETFL, O_NONBLOCK) == -1)
			throw std::runtime_error("fcntl listener");
		poll_setup(_listen_socks.back());
	}
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
			throw std::out_of_range("Invalid Method");
		std::string requestLine = buf.substr(0, n);
		size_t pos = requestLine.find(' ');
		if(pos == std::string::npos)
			throw std::out_of_range("Invalid Method");
		_method = requestLine.substr(0, pos);
		if (_method != "POST" && _method != "GET" && _method != "DELETE")
			throw std::runtime_error("Invalid Method");
		size_t m = requestLine.find(' ', pos + 1);
		if(m == std::string::npos)
			throw std::out_of_range("Invalid Method");
		_uri = requestLine.substr(pos + 1, m - pos - 1);
		if (_uri.empty() || _uri[0] != '/')
			throw std::runtime_error("Invalid Method");
		_version = requestLine.substr(m + 1, n - m - 1);
		if (_version != "HTTP/1.1")
			throw std::runtime_error("Invalid Method");

		std::string allowed_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";	
		if (end_of_headers == std::string::npos)
			throw std::runtime_error("400 Bad Request");
		std::string Header = buf.substr(n + 2, end_of_headers - (n + 2) + 2); 
		while (Header != "\r\n" && !Header.empty()) 
		{
			size_t line_end = Header.find("\r\n");
			std::string Line = Header.substr(0, line_end);
			size_t colon = Line.find(':');
			if (colon == std::string::npos)
				throw std::runtime_error("400 Bad Request");
			std::string Key = Line.substr(0, colon);
			if (Key.find_first_not_of(allowed_chars) != std::string::npos)
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
			_headers[Key] = Value;
			Header.erase(0, line_end + 2);
		}
		if (_headers.find("host") == _headers.end())
			throw std::runtime_error("400 Bad Request");
		_headersParsed = true;
	}
	if (_headers.find("content-length") != _headers.end())
	{
	    long content_length = std::atol(_headers["content-length"].c_str());
	    size_t body_received = _raw_buf.size() - (end_of_headers + 4);
	    if (body_received >= static_cast<size_t>(content_length))
	        _isParsed = true;
	}
	else if (_method == "POST" && _headers.find("transfer-encoding") == _headers.end())
	    throw std::runtime_error("411 Length Required");
	else if (_method == "POST" && _headers.find("transfer-encoding") != _headers.end())
	{
		if (_raw_buf.find("0\r\n\r\n") != std::string::npos)
			_isParsed = true;
	}
	else _isParsed = true;
}

void HTTPRequest::AddRawP(const char* line, int nbytes)
{
	_raw_buf.append(line, nbytes);
	if ((_raw_buf.find("\r\n\r\n") != std::string::npos) && !_isParsed)
		Validate(_raw_buf);
}

bool HTTPRequest::IsParsed()
{
	return(_isParsed);
}

void HTTPResponse::prepare_GET(const HTTPRequest& req, const LocationConfig& loc)
{
	if (has_path_traversal(req.getUri()))
	{
		prepare_error(403, "Invalid path");
		return;
	}

	std::string full_path = resolve_resource_path(req, loc);
	
	// If the URI is a directory, search through the index vector
	if (!full_path.empty() && full_path[full_path.length() - 1] == '/')
	{
		bool found = false;
		for (size_t i = 0; i < loc._index.size(); ++i)
		{
			std::string try_path = full_path + loc._index[i];
			if (access(try_path.c_str(), F_OK) == 0)
			{
				full_path = try_path;
				found = true;
				break;
			}
		}
		if (!found && !loc._autoindex)
		{
			prepare_error(404, "Requested resource was not found");
			return;
		}
		if (!found && loc._autoindex)
		{
			_body = build_autoindex_page(full_path, req.getUri());
			if (_body.empty())
			{
				prepare_error(403, "Directory is not accessible");
				return;
			}
			_statusCode = 200;
			_contentType = "text/html";
			return;
		}
	}

	_contentType = get_content_type(full_path);
	body_GET(full_path);
}

std::string HTTPRequest::getBody() const
{
	size_t n = _raw_buf.find("\r\n\r\n");
	if (n == std::string::npos)
		return ("");
	return _raw_buf.substr(n + 4);
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
	  _location(""),
	  _contentType("text/plain"),
	  _post_body("") ,
	  final_response("") 
{}

std::string HTTPResponse::get_raw_response() const
{
	return final_response;
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

void HTTPResponse::construct_response(const HTTPRequest& req, const ServerConfig& serv)
{
	std::stringstream response_stream;
	const std::map<std::string, std::string>& header_req = req.getMap();
	std::string response_version = req.getVersion();
	if (response_version.empty())
		response_version = _version;

	// If it's an error and we have a custom page, try to load it
	if (_statusCode >= 400 && _body.empty())
	{
		std::map<int, std::string>::const_iterator it_err = serv._error_pages.find(_statusCode);
		if (it_err != serv._error_pages.end())
		{
			std::string err_path = serv._root + it_err->second;
			std::ifstream err_file(err_path.c_str());
			if (err_file.is_open())
			{
				_body.assign((std::istreambuf_iterator<char>(err_file)), std::istreambuf_iterator<char>());
				_contentType = "text/html";
			}
		}
	}

	if (_reason.empty()) 
		_reason = reason_phrase(_statusCode);

	if (_statusCode == 204)
		_body.clear();

	response_stream << response_version << " " << _statusCode << " " << _reason << "\r\n";

	if (_contentType.empty() && _statusCode != 204)
		_contentType = "application/octet-stream";
	if (!_contentType.empty())
		response_stream << "Content-Type: " << _contentType << "\r\n";
	response_stream << "Content-Length: " << _body.length() << "\r\n";

	if (!_location.empty())
		response_stream << "Location: " << _location << "\r\n";

	std::map<std::string, std::string>::const_iterator it = header_req.find("connection");

	if (it != header_req.end() && it->second == "close")
	    response_stream << "Connection: close\r\n";
	else
	    response_stream << "Connection: keep-alive\r\n";
	response_stream << "\r\n";
	response_stream << _body;
	final_response = response_stream.str();
}

void HTTPResponse::body_GET(const std::string& path)
{
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

void HTTPResponse::handle_chunks(const HTTPRequest& req)
{
    std::string raw_body = req.getBody();
    std::string decoded_body = "";
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
    _post_body = decoded_body; 
}

void HTTPResponse::body_POST(const std::string& path)
{
	std::ofstream outfile(path.c_str(), std::ios::out | std::ios::binary);
	if(outfile.is_open())
	{
		outfile.write(_post_body.data(), _data_size);
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

void HTTPResponse::prepare_POST(const HTTPRequest& req, const LocationConfig& loc)
{
	if (has_path_traversal(req.getUri()))
	{
		prepare_error(403, "Invalid path");
		return;
	}
	std::string full_path = resolve_resource_path(req, loc);
	const std::map<std::string, std::string> &header_req = req.getMap();
	std::map<std::string, std::string>::const_iterator it = header_req.find("content-length");
	if (it != header_req.end()) 
	{
		_data_size = std::atoi(it->second.c_str());
		_post_body = req.getBody().substr(0, _data_size);
	}
	else
		handle_chunks(req);
	body_POST(full_path);
}

void HTTPResponse::prepare_DELETE(const HTTPRequest& req, const LocationConfig& loc)
{
	if (has_path_traversal(req.getUri()))
	{
		prepare_error(403, "Invalid path");
		return;
	}

	std::string full_path = resolve_resource_path(req, loc);

	if (std::remove(full_path.c_str()) == 0)
	{
		_statusCode = 204;
		_body = "";
		_contentType = "";
	}
	else if (errno == EACCES || errno == EPERM)
	{
		prepare_error(403, "Permission denied");
	}
	else
	{
		prepare_error(404, "File does not exist");
	}
}

void HTTPResponse::prepare_error(int statusCode, const std::string& message)
{
	_statusCode = statusCode;
	std::stringstream ss;
	ss << "<h1>" << statusCode << " " << reason_phrase(statusCode) << "</h1>";
	if (!message.empty())
		ss << "<p>" << message << "</p>";
	_body = ss.str();
	_contentType = "text/html";
}

void HTTPResponse::prepare_else(const HTTPRequest& req)
{
	(void)req;
	prepare_error(501, "Method is not implemented");
}

void HTTPResponse::build(const HTTPRequest& req, const LocationConfig& loc)
{
	if (loc._return_code != 0)
	{
		_statusCode = loc._return_code;
		_location = loc._return_url;
		_body = "";
		_contentType = "";
		return;
	}

	// 1. Check if method is allowed
	bool allowed = false;
	for (size_t i = 0; i < loc._allowed_methods.size(); ++i)
	{
		if (loc._allowed_methods[i] == req.getMethod())
		{
			allowed = true;
			break;
		}

	}
	
	if (!allowed)
	{
		prepare_error(405, "Method is not allowed for this location");
		return;
	}

	if(req.getMethod() == "GET")
		prepare_GET(req, loc);
	else if(req.getMethod() == "POST")
		prepare_POST(req, loc);
	else if(req.getMethod() == "DELETE")
		prepare_DELETE(req, loc);
	else
		prepare_else(req);
}

void server::close_connection(size_t& i)
{
	int fd = _fds[i].fd;
	close(fd);
	_requests.erase(fd);
	_responses.erase(fd);
	_client_to_listener.erase(fd);
	_fds.erase(_fds.begin() + i);
	i--;
}

void server::srv_manage()
{
	while(1)
	{
		if (_fds.empty())
			throw std::runtime_error("No active sockets");
		if (poll(&_fds[0], _fds.size(), TOUT) == -1)
		{
			if (errno == EINTR)
				continue;
			throw std::runtime_error("Poll");
		}
		for(size_t i = 0; i < _fds.size(); i++)
		{
			if (_fds[i].revents & POLLIN)
			{
				struct sockaddr_storage the_addr;
				socklen_t len = sizeof the_addr;
				char s[INET6_ADDRSTRLEN];
				int client_fd = _fds[i].fd;

				bool is_listener = (std::find(_listen_socks.begin(), _listen_socks.end(), client_fd) != _listen_socks.end());

				if(is_listener) //new connection
				{
					int cnxfd = accept(client_fd, reinterpret_cast<sockaddr*>(&the_addr), &len);
					if (cnxfd == -1)
					{
						std::cerr << "Failed to connect " << std::endl;
						continue;
					}
					// Find which server index this listener corresponds to
					std::vector<int>::iterator it = std::find(_listen_socks.begin(), _listen_socks.end(), client_fd);
					size_t srv_idx = static_cast<size_t>(std::distance(_listen_socks.begin(), it));

					if (fcntl(cnxfd, F_SETFL, O_NONBLOCK) == -1)
					{
						close(cnxfd);
						continue;
					}
					poll_setup(cnxfd);
					_client_to_listener[cnxfd] = srv_idx; // Store the index directly
					_requests[cnxfd] = HTTPRequest();
					inet_ntop(the_addr.ss_family, get_addr_type(reinterpret_cast<sockaddr*>(&the_addr)), s, sizeof s);
					std::cout << "srv: new connection from " << s << " on fd " << cnxfd << " (Total clients: " << _fds.size() - 1 << ")" << std::endl;
				}
				else  //existing connection
				{
					char buf[BUFF_SIZE];
					int nbytes = recv(client_fd, buf, BUFF_SIZE - 1, 0);
					
					if (nbytes <= 0)
					{
						if(nbytes == 0)
							std::cout << "srv: socket " <<  client_fd << " hung up." << std::endl;
						else
							std::cerr << "srv: recv error on fd " <<  client_fd << std::endl;
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
						}
						catch(const std::exception& e)
						{
							std::cerr << e.what() << std::endl;
							std::map<int, int>::iterator it_idx = _client_to_listener.find(client_fd);
							if (it_idx == _client_to_listener.end() || it_idx->second < 0 || static_cast<size_t>(it_idx->second) >= _config._servers.size())
							{
								close_connection(i);
								continue;
							}
							const ServerConfig& serv = _config._servers[it_idx->second];

							HTTPResponse error_res;
							std::string err = e.what();
							int status_code = 400;
							if (err.find("411") != std::string::npos)
								status_code = 411;
							error_res.prepare_error(status_code, err);
							error_res.construct_response(current_req, serv);
							std::string raw = error_res.get_raw_response();
							send(client_fd, raw.c_str(), raw.size(), 0);
							close_connection(i);
							continue;
						}
						if (current_req.IsParsed())
						{
							std::map<int, int>::iterator it_idx = _client_to_listener.find(client_fd);
							if (it_idx == _client_to_listener.end() || it_idx->second < 0 || static_cast<size_t>(it_idx->second) >= _config._servers.size())
							{
								close_connection(i);
								continue;
							}
							const ServerConfig& serv = _config._servers[it_idx->second];

							// Check if the body size exceeds the server's limit
							if (current_req.getBody().length() > serv._client_max_body_size)
							{
								HTTPResponse error_res;
								error_res.prepare_error(413, "Request body exceeds client_max_body_size");
								error_res.construct_response(current_req, serv);
								std::string raw = error_res.get_raw_response();
								send(client_fd, raw.c_str(), raw.size(), 0);
								close_connection(i);
								continue;
							}

							// 2. Longest Prefix Match for Location
							size_t best_match = 0;
							size_t loc_idx = 0;
							bool found_location = false;
							for (size_t k = 0; k < serv._locations.size(); ++k)
							{
								const std::string& path = serv._locations[k]._path;
								if (is_location_match(current_req.getUri(), path))
								{
									if (!found_location || path.length() > best_match)
									{
										best_match = path.length();
										loc_idx = k;
										found_location = true;
									}
								}
							}
							HTTPResponse new_response;
							if (found_location)
								new_response.build(current_req, serv._locations[loc_idx]);
							else
								new_response.prepare_error(404, "No matching location block");
							new_response.construct_response(current_req, serv);
							std::string raw = new_response.get_raw_response();
                            send(client_fd, raw.c_str(), raw.size(), 0);
							close_connection(i);
							std::cout << "srv: response sent and connection closed." << std::endl;
						}
						
					}
				}
			}
		}
	}
}


void AppManager::signal_handler(int s)
{
	(void)s;
	int saved_errno = errno;
	while(waitpid(-1, NULL, WNOHANG) > 0);
	errno = saved_errno;
}

void AppManager::run(char **argv)
{
	server serv;
	serv.start_listening(argv);
	serv.srv_manage();
}

std::vector<std::string> tokenize(std::string& config_data)
{
	std::vector<std::string> tokens;
	std::string symbols = "{};";

	for (size_t i = 0; i < config_data.length(); ++i)
	{
		if (symbols.find(config_data[i]) != std::string::npos)
		{
			config_data.insert(i, " ");
			config_data.insert(i + 2, " ");
			i += 2;
		}
	}
	std::stringstream ss(config_data);
	std::string temp;
	while (ss >> temp)
		tokens.push_back(temp);
	return tokens;
}

ServerConfig::ServerConfig() : _port(8080), _client_max_body_size(1048576)
{}

LocationConfig::LocationConfig() : _autoindex(false), _return_code(0)
{}

size_t parse_size(std::string s)
{
	if (s.empty())
		return 0;
	char unit = toupper(s[s.length() - 1]);
	size_t multiplier = 1;

	if (unit == 'K')
		multiplier = 1024;
	else if (unit == 'M')
		multiplier = 1024 * 1024;
	else if (unit == 'G')
		multiplier = 1024 * 1024 * 1024;
	if (unit == 'K' || unit == 'M' || unit == 'G')
		s.erase(s.length() - 1);
	return (static_cast<size_t>(std::atoll(s.c_str()) * multiplier));
}

Config parse_config(const std::vector<std::string>& tokens)
{
	Config final_config;
	size_t i = 0;

	while (i < tokens.size())
	{
		if (tokens[i] != "server")
		{
			++i;
			continue;
		}

		++i;
		if (i >= tokens.size() || tokens[i] != "{")
			throw std::runtime_error("Configuration file Error");
		++i;

		ServerConfig serv;
		while (i < tokens.size() && tokens[i] != "}")
		{
			if (tokens[i] == "listen")
			{
				++i;
				if (i < tokens.size())
					serv._port = std::atoi(tokens[i].c_str());
				consume_until_semicolon(tokens, i);
			}
			else if (tokens[i] == "host")
			{
				++i;
				if (i < tokens.size())
					serv._host = tokens[i];
				consume_until_semicolon(tokens, i);
			}
			else if (tokens[i] == "server_name")
			{
				++i;
				while (i < tokens.size() && tokens[i] != ";")
				{
					serv._server_names.push_back(tokens[i]);
					++i;
				}
				if (i < tokens.size() && tokens[i] == ";")
					++i;
			}
			else if (tokens[i] == "client_max_body_size")
			{
				++i;
				if (i < tokens.size())
					serv._client_max_body_size = parse_size(tokens[i]);
				consume_until_semicolon(tokens, i);
			}
			else if (tokens[i] == "error_page")
			{
				std::vector<std::string> tmp;
				++i;
				while (i < tokens.size() && tokens[i] != ";")
				{
					tmp.push_back(tokens[i]);
					++i;
				}
				if (i < tokens.size() && tokens[i] == ";")
					++i;
				if (tmp.size() >= 2)
				{
					std::string path = tmp.back();
					for (size_t j = 0; j < tmp.size() - 1; ++j)
						serv._error_pages[std::atoi(tmp[j].c_str())] = path;
				}
			}
			else if (tokens[i] == "root")
			{
				++i;
				if (i < tokens.size())
					serv._root = tokens[i];
				consume_until_semicolon(tokens, i);
			}
			else if (tokens[i] == "index")
			{
				++i;
				while (i < tokens.size() && tokens[i] != ";")
				{
					serv._index.push_back(tokens[i]);
					++i;
				}
				if (i < tokens.size() && tokens[i] == ";")
					++i;
			}
			else if (tokens[i] == "location")
			{
				LocationConfig loc;
				++i;
				if (i >= tokens.size())
					throw std::runtime_error("Configuration file Error");
				loc._path = tokens[i];

				++i;
				if (i >= tokens.size() || tokens[i] != "{")
					throw std::runtime_error("Configuration file Error");
				++i;

				while (i < tokens.size() && tokens[i] != "}")
				{
					if (tokens[i] == "root")
					{
						++i;
						if (i < tokens.size())
							loc._root = tokens[i];
						consume_until_semicolon(tokens, i);
					}
					else if (tokens[i] == "allow_methods")
					{
						++i;
						while (i < tokens.size() && tokens[i] != ";")
						{
							loc._allowed_methods.push_back(tokens[i]);
							++i;
						}
						if (i < tokens.size() && tokens[i] == ";")
							++i;
					}
					else if (tokens[i] == "index")
					{
						++i;
						while (i < tokens.size() && tokens[i] != ";")
						{
							loc._index.push_back(tokens[i]);
							++i;
						}
						if (i < tokens.size() && tokens[i] == ";")
							++i;
					}
					else if (tokens[i] == "cgi_path")
					{
						++i;
						if (i < tokens.size())
							loc._cgi_path = tokens[i];
						consume_until_semicolon(tokens, i);
					}
					else if (tokens[i] == "cgi_extension")
					{
						++i;
						while (i < tokens.size() && tokens[i] != ";")
						{
							loc._cgi_ext.push_back(tokens[i]);
							++i;
						}
						if (i < tokens.size() && tokens[i] == ";")
							++i;
					}
					else if (tokens[i] == "autoindex")
					{
						++i;
						if (i < tokens.size())
							loc._autoindex = (tokens[i] == "on");
						consume_until_semicolon(tokens, i);
					}
					else if (tokens[i] == "return")
					{
						++i;
						if (i < tokens.size())
							loc._return_code = std::atoi(tokens[i].c_str());
						++i;
						if (i < tokens.size() && tokens[i] != ";")
							loc._return_url = tokens[i];
						consume_until_semicolon(tokens, i);
					}
					else
					{
						consume_until_semicolon(tokens, i);
					}
				}

				if (i >= tokens.size() || tokens[i] != "}")
					throw std::runtime_error("Configuration file Error");
				++i;

				serv._locations.push_back(loc);
			}
			else
			{
				consume_until_semicolon(tokens, i);
			}
		}

		if (i >= tokens.size() || tokens[i] != "}")
			throw std::runtime_error("Configuration file Error");
		++i;

		for (size_t j = 0; j < serv._locations.size(); ++j)
		{
			if (serv._locations[j]._root.empty())
				serv._locations[j]._root = serv._root;
			if (serv._locations[j]._index.empty())
				serv._locations[j]._index = serv._index;
		}
		if (serv._host.empty())
			serv._host = "127.0.0.1";
		final_config._servers.push_back(serv);
	}

	return final_config;
}

Config server::ConfigManager(char **argv)
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
		std::vector<std::string> tokens = tokenize(config_data);
		Config parsed = parse_config(tokens);
		normalize_config_paths(parsed, filepath);
		return parsed;
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
		AppManager start;
		start.run(argv);
	}
	catch(const std::exception& e)
	{
		std::cerr << e.what() << std::endl;
		return 1;
	}
    return 0;
}
