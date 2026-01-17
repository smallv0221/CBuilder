// Configuration can be provided via config.json or environment variables
// See source code for configuration options

#include <dlfcn.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Function pointer type for the plugin's ExtractLanguage function
typedef const char* (*ExtractLanguageFunc)(const char*);

struct Config {
    std::string host = "localhost";
    int port = 5000;
    std::string pluginPath = "./plugins/libextractor.so";
    bool valid = false;
};

// Simple JSON value extraction (no external library needed)
std::string extractJsonString(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return "";
    
    size_t colonPos = json.find(':', keyPos);
    if (colonPos == std::string::npos) return "";
    
    size_t valueStart = json.find_first_not_of(" \t\n\r", colonPos + 1);
    if (valueStart == std::string::npos) return "";
    
    if (json[valueStart] == '"') {
        size_t valueEnd = json.find('"', valueStart + 1);
        if (valueEnd != std::string::npos) {
            return json.substr(valueStart + 1, valueEnd - valueStart - 1);
        }
    } else {
        // Number or other value
        size_t valueEnd = json.find_first_of(",}\n", valueStart);
        if (valueEnd != std::string::npos) {
            return json.substr(valueStart, valueEnd - valueStart);
        }
    }
    return "";
}

Config loadConfiguration() {
    Config config;
    
    // Try environment variables first
    const char* envHost = std::getenv("WLS_HOST");
    const char* envPort = std::getenv("WLS_PORT");
    const char* envPlugin = std::getenv("WLS_PLUGIN_PATH");
    
    if (envHost && envPort) {
        config.host = envHost;
        config.port = std::stoi(envPort);
        if (envPlugin) {
            config.pluginPath = envPlugin;
        }
        config.valid = true;
        return config;
    }
    
    // Try config.json in current directory
    std::ifstream configFile("config.json");
    if (!configFile.is_open()) {
        // Try in executable directory
        configFile.open("./config.json");
    }
    
    if (configFile.is_open()) {
        std::stringstream buffer;
        buffer << configFile.rdbuf();
        std::string json = buffer.str();
        configFile.close();
        
        std::string host = extractJsonString(json, "host");
        std::string port = extractJsonString(json, "port");
        std::string pluginPath = extractJsonString(json, "pluginPath");
        
        if (!host.empty()) config.host = host;
        if (!port.empty()) config.port = std::stoi(port);
        if (!pluginPath.empty()) config.pluginPath = pluginPath;
        config.valid = true;
        return config;
    }
    
    return config; // valid = false
}

// Parse HTTP request and return the body
std::string parseHttpRequest(const std::string& request, std::string& method, std::string& path) {
    std::istringstream stream(request);
    stream >> method >> path;
    
    // Find the body (after \r\n\r\n)
    size_t bodyStart = request.find("\r\n\r\n");
    if (bodyStart != std::string::npos) {
        return request.substr(bodyStart + 4);
    }
    return "";
}

// Build HTTP response
std::string buildHttpResponse(int statusCode, const std::string& body) {
    std::ostringstream response;
    std::string statusText = (statusCode == 200) ? "OK" : "Bad Request";
    response << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
    response << "Content-Type: application/json\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << body;
    return response.str();
}

int main() {
    Config config = loadConfiguration();
    
    if (!config.valid) {
        std::cerr << "ERROR: Configuration not found." << std::endl;
        return 1;
    }
    
    // Load the plugin
    void* pluginHandle = dlopen(config.pluginPath.c_str(), RTLD_LAZY);
    if (!pluginHandle) {
        std::cerr << "ERROR: Plugin not found at " << config.pluginPath << std::endl;
        std::cerr << "       " << dlerror() << std::endl;
        return 1;
    }
    
    // Get the ExtractLanguage function
    dlerror(); // Clear any existing error
    ExtractLanguageFunc extractLanguage = (ExtractLanguageFunc)dlsym(pluginHandle, "ExtractLanguage");
    const char* dlsymError = dlerror();
    if (dlsymError) {
        std::cerr << "ERROR: Could not find ExtractLanguage function in plugin." << std::endl;
        std::cerr << "       " << dlsymError << std::endl;
        dlclose(pluginHandle);
        return 1;
    }
    
    // Create socket
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        std::cerr << "ERROR: Failed to create socket" << std::endl;
        dlclose(pluginHandle);
        return 1;
    }
    
    // Allow address reuse
    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Bind to address
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_port = htons(config.port);
    
    if (config.host == "localhost" || config.host == "127.0.0.1") {
        address.sin_addr.s_addr = inet_addr("127.0.0.1");
    } else {
        address.sin_addr.s_addr = INADDR_ANY;
    }
    
    if (bind(serverFd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "ERROR: Failed to bind to " << config.host << ":" << config.port << std::endl;
        close(serverFd);
        dlclose(pluginHandle);
        return 1;
    }
    
    if (listen(serverFd, 10) < 0) {
        std::cerr << "ERROR: Failed to listen" << std::endl;
        close(serverFd);
        dlclose(pluginHandle);
        return 1;
    }
    
    std::cout << "Server running on " << config.host << ":" << config.port << std::endl;
    
    while (true) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(serverFd, (struct sockaddr*)&clientAddr, &clientLen);
        
        if (clientFd < 0) {
            continue;
        }
        
        // Read request
        std::string request;
        char buffer[8192];
        ssize_t bytesRead;
        
        // Read headers first
        while ((bytesRead = read(clientFd, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytesRead] = '\0';
            request.append(buffer, bytesRead);
            
            // Check if we have the complete headers
            size_t headerEnd = request.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                // Parse Content-Length
                size_t clPos = request.find("Content-Length:");
                if (clPos == std::string::npos) {
                    clPos = request.find("content-length:");
                }
                
                if (clPos != std::string::npos) {
                    size_t clEnd = request.find("\r\n", clPos);
                    std::string clValue = request.substr(clPos + 15, clEnd - clPos - 15);
                    size_t contentLength = std::stoul(clValue);
                    size_t bodyStart = headerEnd + 4;
                    size_t currentBodySize = request.size() - bodyStart;
                    
                    // Read remaining body if needed
                    while (currentBodySize < contentLength) {
                        bytesRead = read(clientFd, buffer, sizeof(buffer) - 1);
                        if (bytesRead <= 0) break;
                        buffer[bytesRead] = '\0';
                        request.append(buffer, bytesRead);
                        currentBodySize += bytesRead;
                    }
                }
                break;
            }
        }
        
        if (!request.empty()) {
            std::string method, path;
            std::string body = parseHttpRequest(request, method, path);
            
            std::string response;
            
            if (method == "GET" && path == "/health") {
                response = buildHttpResponse(200, "{\"status\":\"healthy\"}");
            } else if (method == "POST" && path == "/extract") {
                if (body.empty()) {
                    response = buildHttpResponse(400, "{\"error\":\"No HTML content provided\"}");
                } else {
                    const char* language = extractLanguage(body.c_str());
                    std::string jsonBody = "{\"language\":\"";
                    jsonBody += (language ? language : "unknown");
                    jsonBody += "\"}";
                    response = buildHttpResponse(200, jsonBody);
                }
            } else {
                response = buildHttpResponse(404, "{\"error\":\"Not found\"}");
            }
            
            write(clientFd, response.c_str(), response.size());
        }
        
        close(clientFd);
    }
    
    close(serverFd);
    dlclose(pluginHandle);
    return 0;
}
