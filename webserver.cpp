#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <functional>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #define SOCKET int
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
    #define closesocket close
#endif

class HTTPRequest {
public:
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    void parse(const std::string& request_str) {
        std::istringstream iss(request_str);
        std::string line;

        // 解析请求行
        if (std::getline(iss, line)) {
            std::istringstream line_stream(line);
            line_stream >> method >> path >> version;
        }

        // 解析头部
        while (std::getline(iss, line) && !line.empty() && line != "\r") {
            if (line.back() == '\r') {
                line.pop_back();
            }
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                // 去除值前面的空格
                while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) {
                    value.erase(0, 1);
                }
                headers[key] = value;
            }
        }

        // 读取请求体
        if (headers.find("Content-Length") != headers.end()) {
            int content_length = std::stoi(headers["Content-Length"]);
            std::vector<char> body_buffer(content_length);
            iss.read(body_buffer.data(), content_length);
            body = std::string(body_buffer.data(), content_length);
        }
    }
};

class HTTPResponse {
public:
    std::string version;
    int status_code;
    std::string status_message;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    HTTPResponse() : version("HTTP/1.1"), status_code(200), status_message("OK") {}

    void setContent(const std::string& content, const std::string& content_type = "text/html") {
        body = content;
        headers["Content-Type"] = content_type;
        headers["Content-Length"] = std::to_string(body.size());
    }

    std::string toString() const {
        std::ostringstream oss;
        oss << version << " " << status_code << " " << status_message << "\r\n";
        
        for (const auto& header : headers) {
            oss << header.first << ": " << header.second << "\r\n";
        }
        
        oss << "\r\n" << body;
        return oss.str();
    }
};

class WebServer {
private:
    SOCKET server_socket;
    int port;
    bool running;
    std::unordered_map<std::string, std::function<void(const HTTPRequest&, HTTPResponse&)>> routes;

    void initializeSocket() {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
#endif

        server_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (server_socket == INVALID_SOCKET) {
            throw std::runtime_error("Could not create socket");
        }

        // 允许地址重用
        int opt = 1;
        if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
            throw std::runtime_error("setsockopt failed");
        }

        sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port);

        if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
            throw std::runtime_error("Bind failed");
        }

        if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR) {
            throw std::runtime_error("Listen failed");
        }

        std::cout << "Server started on port " << port << std::endl;
    }

public:
    WebServer(int port) : port(port), running(false) {}

    ~WebServer() {
        stop();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void addRoute(const std::string& path, std::function<void(const HTTPRequest&, HTTPResponse&)> handler) {
        routes[path] = handler;
    }

    void addStaticFileRoute(const std::string& path, const std::string& file_path) {
        routes[path] = [file_path](const HTTPRequest& req, HTTPResponse& res) {
            std::ifstream file(file_path, std::ios::binary);
            if (file.is_open()) {
                std::stringstream buffer;
                buffer << file.rdbuf();
                file.close();
                
                // 简单的MIME类型检测
                std::string content_type = "text/plain";
                auto hasExtension = [](const std::string &str, const std::string &ext) -> bool {
                    if (str.length() < ext.length()) return false;
                    return str.compare(str.length() - ext.length(), ext.length(), ext) == 0;
                };
                
                if (hasExtension(file_path, ".html") || hasExtension(file_path, ".htm")) {
                    content_type = "text/html";
                } else if (hasExtension(file_path, ".css")) {
                    content_type = "text/css";
                } else if (hasExtension(file_path, ".js")) {
                    content_type = "application/javascript";
                } else if (hasExtension(file_path, ".json")) {
                    content_type = "application/json";
                } else if (hasExtension(file_path, ".png")) {
                    content_type = "image/png";
                } else if (hasExtension(file_path, ".jpg") || hasExtension(file_path, ".jpeg")) {
                    content_type = "image/jpeg";
                } else if (hasExtension(file_path, ".gif")) {
                    content_type = "image/gif";
                }
                
                res.setContent(buffer.str(), content_type);
            } else {
                res.status_code = 404;
                res.status_message = "Not Found";
                res.setContent("<html><body><h1>404 Not Found</h1></body></html>");
            }
        };
    }

    void start() {
        try {
            initializeSocket();
            running = true;

            while (running) {
                sockaddr_in client_addr;
                socklen_t client_addr_size = sizeof(client_addr);
                
                SOCKET client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_size);
                if (client_socket == INVALID_SOCKET) {
                    std::cerr << "Accept failed" << std::endl;
                    continue;
                }

                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
                std::cout << "Client connected: " << client_ip << std::endl;

                // 处理请求
                const int buffer_size = 8192;
                char buffer[buffer_size] = {0};
                int bytes_received = recv(client_socket, buffer, buffer_size - 1, 0);
                
                if (bytes_received > 0) {
                    std::string request_str(buffer, bytes_received);
                    HTTPRequest request;
                    request.parse(request_str);

                    HTTPResponse response;
                    
                    // 路由处理
                    if (routes.find(request.path) != routes.end()) {
                        routes[request.path](request, response);
                    } else {
                        // 404 Not Found
                        response.status_code = 404;
                        response.status_message = "Not Found";
                        response.setContent("<html><body><h1>404 Not Found</h1></body></html>");
                    }

                    // 发送响应
                    std::string response_str = response.toString();
                    send(client_socket, response_str.c_str(), response_str.size(), 0);
                }

                closesocket(client_socket);
            }
        } catch (const std::exception& e) {
            std::cerr << "Server error: " << e.what() << std::endl;
        }

        if (server_socket != INVALID_SOCKET) {
            closesocket(server_socket);
            server_socket = INVALID_SOCKET;
        }
    }

    void stop() {
        running = false;
        if (server_socket != INVALID_SOCKET) {
            closesocket(server_socket);
            server_socket = INVALID_SOCKET;
        }
    }
};

// 示例用法
int main() {
    try {
        WebServer server(8080);
        
        // 添加路由
        server.addRoute("/", [](const HTTPRequest& req, HTTPResponse& res) {
            res.setContent("<html><body><h1>Hello, World!</h1><p>Welcome to my C++ Web Server</p></body></html>");
        });
        
        server.addRoute("/api/data", [](const HTTPRequest& req, HTTPResponse& res) {
            res.setContent("{\"message\": \"This is JSON data\"}", "application/json");
        });
        
        // 添加静态文件路由
        server.addStaticFileRoute("/index.html", "public/index.html");
        
        std::cout << "Starting server on port 8080..." << std::endl;
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
