#include <iostream>
#include "swiftnet.hpp"

int main() {
    std::cout << "[TEST] Starting acceptor test..." << std::endl;
    
    try {
        std::cout << "[TEST] Creating scheduler..." << std::endl;
        auto& scheduler = swiftnet::vthread_scheduler::instance();
        
        std::cout << "[TEST] Creating acceptor..." << std::endl;
        swiftnet::acceptor acc;
        
        std::cout << "[TEST] Binding to port 8080..." << std::endl;
        acc.bind("127.0.0.1", 8080);
        
        std::cout << "[TEST] Starting to listen..." << std::endl;
        acc.listen();
        
        std::cout << "[TEST] Creating async_accept coroutine..." << std::endl;
        auto accept_task = acc.async_accept([](swiftnet::tcp_socket sock) {
            std::cout << "[TEST] Connection accepted!" << std::endl;
        });
        
        std::cout << "[TEST] Scheduling acceptor task..." << std::endl;
        scheduler.schedule(std::move(accept_task));
        
        std::cout << "[TEST] Starting scheduler run..." << std::endl;
        scheduler.run();
        
    } catch (const std::exception& e) {
        std::cout << "[TEST] Exception: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "[TEST] Test completed" << std::endl;
    return 0;
} 