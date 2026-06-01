#include "swiftnet.hpp"
#include <iostream>
#include <thread>
#include <chrono>

using namespace swiftnet;

vthread simple_task()
{
    std::cout << "Virtual thread started" << std::endl;
    
    // Simulate some work
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::cout << "Virtual thread completed" << std::endl;
    co_return;
}

int main()
{
    std::cout << "SwiftNet Simple Test" << std::endl;
    
    // Start the virtual thread scheduler
    vthread_scheduler::instance().start(2);
    
    // Schedule a simple virtual thread
    auto task = simple_task();
    vthread_scheduler::instance().schedule(std::move(task));
    
    // Let it run for a bit
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // Get stats
    auto stats = vthread_scheduler::instance().get_stats();
    std::cout << "Total scheduled: " << stats.total_scheduled << std::endl;
    std::cout << "Total I/O suspended: " << stats.total_io_suspended << std::endl;
    std::cout << "Total resumed: " << stats.total_resumed << std::endl;
    
    // Stop the scheduler
    vthread_scheduler::instance().stop();
    
    return 0;
} 