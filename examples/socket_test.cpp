#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>
#include <cstring>

int main() {
    // Create socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket() failed: " << strerror(errno) << std::endl;
        return 1;
    }
    
    std::cout << "Created socket fd=" << listen_fd << std::endl;
    
    // Set non-blocking
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
    
    // Set reuse address
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Bind to port 8081
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8081);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind() failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }
    
    std::cout << "Bound to port 8081" << std::endl;
    
    // Listen
    if (listen(listen_fd, 1024) < 0) {
        std::cerr << "listen() failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }
    
    std::cout << "Listening on port 8081, fd=" << listen_fd << std::endl;
    std::cout << "Connect with: curl http://localhost:8081/" << std::endl;
    
    // Test select() in a loop
    for (int i = 0; i < 100; ++i) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_fd, &read_fds);
        
        struct timeval timeout = {0, 100000}; // 100ms timeout
        int max_fd = listen_fd + 1;
        
        int ret = select(max_fd, &read_fds, nullptr, nullptr, &timeout);
        
        if (ret > 0) {
            std::cout << "SUCCESS: select() detected connection ready! ret=" << ret << std::endl;
            
            // Try to accept
            sockaddr_in client_addr{};
            socklen_t len = sizeof(client_addr);
            int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &len);
            
            if (client_fd >= 0) {
                std::cout << "SUCCESS: Accepted connection, client_fd=" << client_fd << std::endl;
                
                // Send a simple response
                const char* response = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello World!\n";
                write(client_fd, response, strlen(response));
                close(client_fd);
                
                std::cout << "Connection handled successfully!" << std::endl;
                break;
            } else {
                std::cout << "accept() failed: " << strerror(errno) << std::endl;
            }
        } else if (ret == 0) {
            std::cout << "Loop " << i << ": select() timeout (no connections)" << std::endl;
        } else {
            std::cout << "select() error: " << strerror(errno) << std::endl;
            break;
        }
    }
    
    close(listen_fd);
    std::cout << "Test complete." << std::endl;
    return 0;
} 