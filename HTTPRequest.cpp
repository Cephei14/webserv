#include "HTTPRequest.hpp"
#include "WebservUtils.hpp"

HTTPRequest::HTTPRequest() : _isParsed(false), _headersParsed(false), _parsed_request_end(0)
{}

void HTTPRequest::reset_parse_state() 
{
	_method.clear();
	_uri.clear();
	_version.clear();
	std::string().swap(_body);
	_headers.clear();
	_headersParsed = false;
	_isParsed = false;
	 _parsed_request_end = 0;
}

void HTTPRequest::clearBody()
{
	std::string().swap(_body);
}

void HTTPRequest::takeBody(std::string& out)
{
	out.clear();
	out.swap(_body);
}

void HTTPRequest::consume_parsed_request()
{
	std::string remaining;
	remaining.swap(_raw_buf);
	reset_parse_state();
	_raw_buf.swap(remaining);
	if(!_raw_buf.empty())
		Validate(_raw_buf);
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
		{
			_body.assign(_raw_buf, body_start, _parsed_request_end - body_start);
			_raw_buf.erase(0, _parsed_request_end);
			std::string compact(_raw_buf);
			_raw_buf.swap(compact);
			_parsed_request_end = 0;
		}
		else
		{
			_body.clear();
			_raw_buf.erase(0, _parsed_request_end);
			std::string compact(_raw_buf);
			_raw_buf.swap(compact);
			_parsed_request_end = 0;
		}
	}
}

void HTTPRequest::AddRawP(const char* line, int nbytes)
{
	_raw_buf.append(line, nbytes);
	if (!_isParsed)
		Validate(_raw_buf);
}

bool HTTPRequest::IsParsed() const
{
	return(_isParsed);
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
