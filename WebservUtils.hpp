#ifndef WEBSERVUTILS_HPP
# define WEBSERVUTILS_HPP

# include "webserv.hpp"

extern volatile sig_atomic_t signaled;

const size_t MAX_REQUEST_LINE_BYTES = 8192;
const size_t MAX_URI_BYTES = 8192;
const size_t MAX_HEADER_BLOCK_BYTES = 65536;
const size_t MAX_CONCURRENT_UPLOADS = 4;

long now_ms();
bool reap_child_nonblocking(pid_t pid);
std::string to_lower_copy(const std::string& s);
void log_post_upload_success(int client_fd, const HTTPRequest& req, const std::string& raw_response);
std::string trim_copy(const std::string& s);
std::string normalize_host_header_value(const std::string& host_value);
std::string uri_path_without_query(const std::string& uri);
size_t location_match_prefix_length(const std::string& path, const std::string& location_path);
LocationConfig* find_best_location_for_path(ServerConfig& srv, const std::string& uri);
bool load_custom_error_page_body(const ServerConfig& srv, int status_code, std::string& body_out);
std::string reason_phrase_from_status(int status);
void parse_cgi_output_block(const std::string& cgi_output, int& status, std::string& content_type, std::string& body);
void build_raw_http_response(const HTTPRequest& req, int status, const std::string& content_type, const std::string& body, bool keep_alive, std::string& out);
bool decode_chunked_body_for_cgi(const std::string& raw, std::string& out);
bool get_request_body_size_for_limit(const HTTPRequest& req, size_t& size_out);
std::string to_cgi_http_header_env_key(const std::string& header_key);
void *get_addr_type(struct sockaddr *the_addr);

#endif
