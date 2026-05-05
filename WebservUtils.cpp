#include "WebservUtils.hpp"

volatile sig_atomic_t signaled = 0;

long now_ms()
{
	struct timeval tv;
	if (gettimeofday(&tv, NULL) == -1)
		return 0;
	return static_cast<long>(tv.tv_sec) * 1000L + static_cast<long>(tv.tv_usec / 1000L);
}

bool reap_child_nonblocking(pid_t pid)
{
	return (waitpid(pid, NULL, WNOHANG) == pid);
}

std::string to_lower_copy(const std::string& s)
{
	std::string out = s;
	for (size_t i = 0; i < out.size(); ++i)
		out[i] = static_cast<char>(tolower(out[i]));
	return out;
}

void log_post_upload_success(int client_fd, const HTTPRequest& req, const std::string& raw_response)
{
	if (req.getMethod() == "POST" &&
		(raw_response.find("HTTP/1.1 200 ") == 0 || raw_response.find("HTTP/1.1 201 ") == 0))
		std::cout << "client fd " << client_fd << " uploaded successfully" << std::endl;
}

std::string trim_copy(const std::string& s)
{
	size_t start = s.find_first_not_of(" \t");
	if (start == std::string::npos)
		return "";
	size_t end = s.find_last_not_of(" \t");
	return s.substr(start, end - start + 1);
}

std::string normalize_host_header_value(const std::string& host_value)
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

std::string uri_path_without_query(const std::string& uri)
{
	size_t q = uri.find('?');
	if (q == std::string::npos) {
		return uri;
	}
	return uri.substr(0, q);
}

size_t location_match_prefix_length(const std::string& path, const std::string& location_path)
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

LocationConfig* find_best_location_for_path(ServerConfig& srv, const std::string& uri)
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

bool load_custom_error_page_body(const ServerConfig& srv, int status_code, std::string& body_out)
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

std::string reason_phrase_from_status(int status)
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

void parse_cgi_output_block(const std::string& cgi_output, int& status, std::string& content_type, std::string& body)
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

void build_raw_http_response(const HTTPRequest& req, int status, const std::string& content_type, const std::string& body, bool keep_alive, std::string& out)
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
void *get_addr_type(struct sockaddr *the_addr)
{
	if (the_addr->sa_family == AF_INET)
	{
		return &((reinterpret_cast<struct sockaddr_in*>(the_addr))->sin_addr);
	}
	return &((reinterpret_cast<struct sockaddr_in6*>(the_addr))->sin6_addr);
}
bool decode_chunked_body_for_cgi(const std::string& raw, std::string& out)
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

bool get_request_body_size_for_limit(const HTTPRequest& req, size_t& size_out)
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

std::string to_cgi_http_header_env_key(const std::string& header_key)
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
