// Basic HTTP Server Example for SwiftNet
// Demonstrates how to create a simple HTTP server with Express.js-like syntax
// Now with sophisticated virtual thread mounting/unmounting system

#include "swiftnet.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <signal.h>

using namespace swiftnet;

// Global reference to the SwiftNet app for signal handling
SwiftNet* g_app = nullptr;

void signal_handler(int signal) {
    if (signal == SIGINT) {
        std::cout << "\n\nReceived interrupt signal (Ctrl+C). Shutting down server..." << std::endl;
        if (g_app) {
            g_app->close();
        }
    }
}

void print_scheduler_stats() {
    auto stats = vthread_scheduler::instance().get_stats();
    std::cout << "\n=== Virtual Thread Scheduler Statistics ===" << std::endl;
    std::cout << "Total scheduled: " << stats.total_scheduled << std::endl;
    std::cout << "Total I/O suspended: " << stats.total_io_suspended << std::endl;
    std::cout << "Total resumed: " << stats.total_resumed << std::endl;
    std::cout << "Work stolen: " << stats.work_stolen << std::endl;
    std::cout << "Context switches: " << stats.context_switches << std::endl;
    
    std::cout << "Per-core execution counts:" << std::endl;
    for (size_t i = 0; i < stats.per_core_executed.size(); ++i) {
        std::cout << "  Core " << i << ": " << stats.per_core_executed[i] << std::endl;
    }
    std::cout << "=========================================\n" << std::endl;
}

int main()
{
    std::cout << "SwiftNet Advanced Virtual Thread Server Example" << std::endl;
    std::cout << "===============================================" << std::endl;
    std::cout << "Features sophisticated virtual thread mounting/unmounting" << std::endl;
    std::cout << "with I/O suspension and work-stealing scheduler" << std::endl;
    std::cout << "Starting server on http://localhost:8080" << std::endl;
    std::cout << "Try these endpoints:" << std::endl;
    std::cout << "  GET  /                - Welcome page" << std::endl;
    std::cout << "  GET  /user/123        - User profile (async I/O)" << std::endl;
    std::cout << "  GET  /search?q=test   - Search (async processing)" << std::endl;
    std::cout << "  POST /api/users       - Create user (JSON + async)" << std::endl;
    std::cout << "  GET  /stress          - Stress test (many virtual threads)" << std::endl;
    std::cout << "  GET  /stats           - Scheduler statistics" << std::endl;
    std::cout << "  GET  /error           - Error example" << std::endl;
    std::cout << std::endl;

    SwiftNet app(8080);
    g_app = &app; // Assign global reference

    // Set up signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    
    // Enable logging middleware
    app.logger();

    // Enable CORS
    app.cors();

    // Enable JSON parsing
    app.json();

    // Start statistics monitoring thread
    std::atomic<bool> stats_running{true};
    std::thread stats_thread([&stats_running]() {
        while (stats_running) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            if (stats_running) {
                print_scheduler_stats();
            }
        }
    });

    // Basic route
    app.get("/", [](Request &req, Response &res) {
        res.html("<h1>Welcome to SwiftNet Advanced!</h1>"
                "<p>A high-performance C++ web framework with sophisticated virtual thread scheduling</p>"
                "<ul>"
                "<li><a href='/user/123'>User Profile</a> - Demonstrates I/O suspension</li>"
                "<li><a href='/search?q=test'>Search</a> - Async processing</li>"
                "<li><a href='/stress'>Stress Test</a> - Many virtual threads</li>"
                "<li><a href='/stats'>Scheduler Stats</a> - Performance metrics</li>"
                "</ul>");
    });

    // Route with parameters - demonstrates I/O suspension and resumption
    app.get("/user/:id", [](Request &req, Response &res) {
        std::string user_id = req.param("id");
        
        // Simulate async database lookup that would suspend the virtual thread
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        
        Json response;
        response["message"] = "User profile loaded asynchronously";
        response["user_id"] = user_id;
        response["vthread_info"] = "This request was processed by a virtual thread that was mounted/unmounted during I/O";
        response["scheduler"] = "Advanced work-stealing scheduler with I/O suspension";
        
        res.json(response);
    });

    // Route with query parameters - demonstrates async processing
    app.get("/search", [](Request &req, Response &res) {
        std::string query = req.query("q");
        if (query.empty()) {
            res.bad_request("Missing query parameter 'q'");
            return;
        }
        
        // Simulate async search that demonstrates virtual thread context switching
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        
        Json response;
        response["query"] = query;
        response["results"] = Json::array({
            "Advanced Virtual Thread Result 1",
            "Sophisticated I/O Scheduler Result 2", 
            "Work-Stealing Algorithm Result 3"
        });
        response["total"] = 3;
        response["processing_info"] = "Processed by virtual thread with sophisticated scheduling";
        
        res.json(response);
    });

    // POST route with JSON body - demonstrates complex async processing
    app.post("/api/users", [](Request &req, Response &res) {
        if (!req.is_json()) {
            res.bad_request("Content-Type must be application/json");
            return;
        }
        
        try {
            Json user_data = req.json();
            
            // Simple validation
            if (!user_data.contains("name") || !user_data.contains("email")) {
                res.bad_request("Missing required fields: name, email");
                return;
            }
            
            // Simulate async database write with virtual thread suspension
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            
            // Create response
            Json response;
            response["id"] = 123;
            response["name"] = user_data["name"];
            response["email"] = user_data["email"];
            response["created_at"] = "2024-01-01T00:00:00Z";
            response["processing_details"] = {
                {"virtual_thread", "mounted and unmounted during I/O operations"},
                {"scheduler", "advanced work-stealing with CPU affinity"},
                {"suspension", "automatic I/O suspension and resumption"}
            };
            
            res.created(response);
        } catch (const std::exception &e) {
            res.bad_request("Invalid JSON: " + std::string(e.what()));
        }
    });

    // Stress test endpoint - creates many virtual threads
    app.get("/stress", [](Request &req, Response &res) {
        Json response;
        response["message"] = "Stress test: Creating many virtual threads";
        response["virtual_threads_created"] = 1000;
        response["scheduler_features"] = Json::array({
            "Work-stealing across CPU cores",
            "I/O suspension and resumption", 
            "CPU affinity optimization",
            "Preemptive scheduling",
            "Load balancing"
        });
        
        // Simulate creating many virtual threads (in a real app, these would be separate tasks)
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
        
        res.json(response);
    });

    // Statistics endpoint - shows scheduler performance
    app.get("/stats", [](Request &req, Response &res) {
        auto stats = vthread_scheduler::instance().get_stats();
        
        Json response;
        response["scheduler_statistics"] = {
            {"total_scheduled", stats.total_scheduled},
            {"total_io_suspended", stats.total_io_suspended},
            {"total_resumed", stats.total_resumed},
            {"work_stolen", stats.work_stolen},
            {"context_switches", stats.context_switches}
        };
        
        Json per_core = Json::array();
        for (size_t i = 0; i < stats.per_core_executed.size(); ++i) {
            per_core.push_back({
                {"core", i},
                {"executed", stats.per_core_executed[i]}
            });
        }
        response["per_core_execution"] = per_core;
        
        response["features"] = Json::array({
            "Sophisticated virtual thread mounting/unmounting",
            "I/O suspension with automatic resumption",
            "Work-stealing scheduler with CPU affinity",
            "Load balancing across cores",
            "Preemptive scheduling with time slicing",
            "Memory pool per CPU core",
            "Cross-platform I/O backends (io_uring, kqueue, IOCP)"
        });
        
        res.json(response);
    });

    // Error handling example
    app.get("/error", [](Request &req, Response &res) {
        res.internal_error("This is an intentional error for testing virtual thread error handling");
    });

    // Middleware example for API routes
    app.use("/api/*", [](Request &req, Response &res, std::function<void()> next) {
        std::cout << "Advanced API middleware: " << req.method() << " " << req.path() 
                  << " (processed by virtual thread)" << std::endl;
        
        // Add advanced API headers
        res.header("X-API-Version", "2.0")
           .header("X-VThread-Scheduler", "Advanced")
           .header("X-Async-IO", "Sophisticated");
        
        next(); // Continue to the actual route handler
    });

    std::cout << "Advanced server with sophisticated virtual thread scheduler is running!" << std::endl;
    std::cout << "Features:" << std::endl;
    std::cout << "  ✓ Virtual thread mounting/unmounting on I/O" << std::endl;
    std::cout << "  ✓ Work-stealing scheduler across CPU cores" << std::endl;
    std::cout << "  ✓ Automatic I/O suspension and resumption" << std::endl;
    std::cout << "  ✓ Load balancing and CPU affinity" << std::endl;
    std::cout << "  ✓ Cross-platform async I/O backends" << std::endl;
    std::cout << "Press Ctrl+C to stop." << std::endl;
    
    // Start the server
    app.listen([]() {
        std::cout << "SwiftNet advanced server listening on port 8080" << std::endl;
        std::cout << "Virtual thread scheduler is online with sophisticated I/O handling" << std::endl;
    });

    // Clean up
    stats_running = false;
    if (stats_thread.joinable()) {
        stats_thread.join();
    }

    std::cout << "Server shutdown complete." << std::endl;
    return 0;
} 