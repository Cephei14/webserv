#include "HTTPResponse.hpp"
#include "WebservUtils.hpp"

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
