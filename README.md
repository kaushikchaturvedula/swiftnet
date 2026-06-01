# SwiftNet

> **High-Performance C++ Networking Library**
> 
> *Sophisticated Virtual Thread System with Automatic Mounting/Unmounting*

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows-brightgreen.svg)](#cross-platform-support)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

## 🚀 **Revolutionary Virtual Thread Architecture**

SwiftNet is a **cutting-edge C++ networking library** built with an **extremely sophisticated virtual thread system** featuring **automatic mounting and unmounting** on CPU cores. When a virtual thread encounters I/O operations, it's **automatically suspended and unmounted**, allowing another virtual thread to **immediately mount** on the same CPU core, maximizing CPU utilization and system throughput.

```cpp
// Virtual threads automatically mount/unmount during I/O
app.get("/user/:id", [](Request& req, Response& res) {
    // This runs in a virtual thread mounted on a CPU core
    std::string user_id = req.param("id");
    
    // When this I/O happens, virtual thread is UNMOUNTED
    // Another virtual thread MOUNTS on the same CPU core
    auto user_data = co_await async_database_lookup(user_id);
    // I/O completes, original virtual thread RE-MOUNTS
    
    res.json({"user": user_data, "id": user_id});
});
```

## 🏗️ **Architecture Overview**

```mermaid
graph TB
    subgraph "SwiftNet High-Performance Architecture"
        subgraph "Application Layer"
            APP[Express.js-like API]
            ROUTES[Routes & Middleware]
            HANDLERS[Request Handlers]
        end
        
        subgraph "Virtual Thread Scheduler"
            SCHEDULER[Advanced Scheduler]
            MOUNT[Mount/Unmount Engine]
            WORKSTEAL[Work-Stealing Algorithm]
            AFFINITY[CPU Affinity Manager]
        end
        
        subgraph "CPU Cores"
            CORE0["Core 0<br/>VThread A<br/>Queue: B,C"]
            CORE1["Core 1<br/>VThread D<br/>Queue: E,F"]
            CORE2["Core 2<br/>VThread G<br/>Queue: H,I"]
            CORE3["Core 3<br/>VThread J<br/>Queue: K,L"]
        end
        
        subgraph "I/O System"
            IOQUEUE[I/O Wait Queue]
            KQUEUE[kqueue/macOS]
            IOURING[io_uring/Linux]
            IOCP[IOCP/Windows]
        end
        
        subgraph "Memory Management"
            MPSC[Lock-free MPSC Queues]
            POOLS[Per-Core Memory Pools]
            ARENAS[Memory Arenas]
        end
    end
    
    APP --> ROUTES
    ROUTES --> HANDLERS
    HANDLERS --> SCHEDULER
    
    SCHEDULER --> MOUNT
    SCHEDULER --> WORKSTEAL
    SCHEDULER --> AFFINITY
    
    MOUNT --> CORE0
    MOUNT --> CORE1
    MOUNT --> CORE2
    MOUNT --> CORE3
    
    WORKSTEAL -.-> CORE0
    WORKSTEAL -.-> CORE1
    WORKSTEAL -.-> CORE2
    WORKSTEAL -.-> CORE3
    
    CORE0 --> IOQUEUE
    CORE1 --> IOQUEUE
    CORE2 --> IOQUEUE
    CORE3 --> IOQUEUE
    
    IOQUEUE --> KQUEUE
    IOQUEUE --> IOURING
    IOQUEUE --> IOCP
    
    SCHEDULER --> MPSC
    SCHEDULER --> POOLS
    SCHEDULER --> ARENAS
```

## 🏗️ **Theoretical Advantages Over Node.js**

SwiftNet's architecture is designed to address several fundamental limitations of Node.js:

### **Multi-Core Utilization**
- **SwiftNet**: True parallelism with virtual threads distributed across CPU cores
- **Node.js**: Single-threaded event loop (main thread) with limited worker thread usage
- **Advantage**: Full CPU utilization vs single-core bottleneck

### **Memory Management**
- **SwiftNet**: Lock-free data structures, per-core memory pools, no garbage collection
- **Node.js**: V8 garbage collection with stop-the-world pauses
- **Advantage**: Predictable memory allocation without GC pauses

### **I/O Handling**
- **SwiftNet**: Direct platform I/O (io_uring/kqueue/IOCP) with virtual thread suspension
- **Node.js**: libuv abstraction layer with callback-based async
- **Advantage**: Lower latency and higher throughput for I/O operations

### **Context Switching**
- **SwiftNet**: Lightweight virtual thread mounting/unmounting in microseconds
- **Node.js**: Event loop scheduling and callback queues
- **Advantage**: More efficient task switching and lower overhead

### **Native Performance**
- **SwiftNet**: Compiled C++ with zero-cost abstractions
- **Node.js**: Interpreted JavaScript with JIT compilation
- **Advantage**: Direct CPU instruction execution vs interpretation overhead

> **Note**: These are theoretical advantages based on architectural differences. 
> See the [Benchmarks](#benchmarks) section for actual performance comparisons.

## 🧵 **Advanced Virtual Thread Features**

### **Sophisticated Mounting/Unmounting System**

```cpp
// Virtual Thread Lifecycle:
// 1. Created for incoming request
// 2. MOUNTED on available CPU core
// 3. Executes until I/O or completion
// 4. UNMOUNTED during I/O operation
// 5. Another virtual thread MOUNTS on same core
// 6. I/O completes → original thread RE-MOUNTS
// 7. Completion and cleanup
```

### **Key Features**

- **🔥 Automatic Mounting/Unmounting**: Virtual threads seamlessly transition on/off CPU cores
- **⚡ Work-Stealing Scheduler**: Advanced algorithm prevents core starvation
- **🎯 CPU Affinity Optimization**: Intelligent core binding for cache locality
- **🔄 I/O Suspension & Resumption**: Microsecond-level context switching
- **⏱️ Preemptive Scheduling**: Time-sliced execution prevents blocking
- **📊 Real-time Monitoring**: Comprehensive performance statistics

## 🌐 **Cross-Platform High-Performance I/O**

| Platform | I/O Backend | Performance |
|----------|-------------|-------------|
| **Linux** | `io_uring` + `epoll` fallback | Highest performance |
| **macOS** | `kqueue` | Native BSD performance |
| **Windows** | `IOCP` | Native Windows performance |

```cpp
// Platform detection and optimal backend selection
#if defined(SWIFTNET_BACKEND_IOURING)
    // Linux: io_uring for maximum performance
#elif defined(SWIFTNET_BACKEND_KQUEUE)  
    // macOS: kqueue for BSD optimization
#elif defined(SWIFTNET_BACKEND_IOCP)
    // Windows: IOCP for Windows optimization
#endif
```

## 🚀 **Features**

### **Modern C++20 Coroutines**
- **`co_await` async I/O** with automatic virtual thread suspension
- **Zero-copy operations** where possible
- **Exception-safe** coroutine handling

### **Express.js-like API**
- **HTTP methods**: GET, POST, PUT, DELETE, PATCH, OPTIONS, HEAD
- **Route parameters**: `/user/:id` with automatic extraction
- **Middleware chains** with `next()` function
- **JSON handling**: Automatic parsing and serialization
- **Static file serving** with MIME type detection
- **CORS support** built-in

### **Enterprise-Grade Performance**
- **Lock-free data structures** (MPSC queues)
- **Per-core memory arenas** for optimal allocation
- **Memory pools** to reduce allocation overhead
- **Intelligent load balancing** across CPU cores

## 📋 **Requirements**

- **C++20** compatible compiler (GCC 10+, Clang 11+, MSVC 2019+)
- **CMake 3.20+**
- **Platform-specific dependencies**:
  - **Linux**: `liburing` (optional, falls back to epoll)
  - **macOS**: Native kqueue support (built-in)
  - **Windows**: Native IOCP support (built-in)

## 🛠️ **Installation**

### **Quick Start**

```bash
git clone https://github.com/NeuralRevenant/swiftnet.git
cd swiftnet
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### **CMake Options**

```bash
# Enable jemalloc for better memory allocation
cmake -DSWIFTNET_WITH_JEMALLOC=ON ..

# Build examples and tests
cmake -DBUILD_EXAMPLES=ON -DBUILD_TESTS=ON ..

# Debug build with full instrumentation
cmake -DCMAKE_BUILD_TYPE=Debug ..
```

## 💻 **Usage Examples**

### **Basic HTTP Server with Virtual Threads**

```cpp
#include "swiftnet.hpp"

using namespace swiftnet;

int main() {
    SwiftNet app(8080);

    // Each request runs in its own virtual thread
    app.get("/", [](Request& req, Response& res) {
        res.html("<h1>SwiftNet: High-Performance C++ Networking!</h1>");
    });

    // Virtual thread with I/O suspension/resumption
    app.get("/user/:id", [](Request& req, Response& res) {
        std::string user_id = req.param("id");
        
        // This I/O will suspend the virtual thread
        // Another vthread mounts on this CPU core
        auto user_data = co_await async_database_lookup(user_id);
        
        res.json({
            {"user_id", user_id},
            {"data", user_data},
            {"processed_by", "virtual_thread_with_mounting"}
        });
    });

    // Middleware with virtual thread support
    app.use([](Request& req, Response& res, std::function<void()> next) {
        std::cout << "Request: " << req.method() << " " << req.path() 
                  << " (Virtual Thread: " << std::this_thread::get_id() << ")" 
                  << std::endl;
        next();
    });

    app.listen([]() {
        std::cout << "SwiftNet server with advanced virtual threads running!" << std::endl;
    });

    return 0;
}
```

### **Advanced Virtual Thread Control**

```cpp
// Get real-time scheduler statistics
auto stats = vthread_scheduler::instance().get_stats();
std::cout << "Virtual threads scheduled: " << stats.total_scheduled << std::endl;
std::cout << "I/O suspensions: " << stats.total_io_suspended << std::endl;
std::cout << "Work stolen: " << stats.work_stolen << std::endl;

// Schedule with specific CPU affinity
vthread_scheduler::instance().schedule_with_affinity(my_vthread, 2);

// Monitor per-core execution
for (size_t i = 0; i < stats.per_core_executed.size(); ++i) {
    std::cout << "Core " << i << ": " << stats.per_core_executed[i] 
              << " virtual threads executed" << std::endl;
}
```

### **JSON API with Async Processing**

```cpp
app.post("/api/users", [](Request& req, Response& res) {
    if (!req.is_json()) {
        res.bad_request("Content-Type must be application/json");
        return;
    }
    
    try {
        Json user_data = req.json();
        
        // Validation
        if (!user_data.contains("name") || !user_data.contains("email")) {
            res.bad_request("Missing required fields: name, email");
            return;
        }
        
        // Async database write (virtual thread suspends here)
        auto user_id = co_await async_create_user(user_data);
        
        res.created({
            {"id", user_id},
            {"name", user_data["name"]},
            {"email", user_data["email"]},
            {"created_at", current_timestamp()},
            {"virtual_thread_info", "suspended and resumed during I/O"}
        });
    } catch (const std::exception& e) {
        res.bad_request("Invalid JSON: " + std::string(e.what()));
    }
});
```

## 📚 **API Reference**

### **SwiftNet Class**

#### **HTTP Methods**
```cpp
SwiftNet& get(const std::string& path, handler_t handler);
SwiftNet& post(const std::string& path, handler_t handler);
SwiftNet& put(const std::string& path, handler_t handler);
SwiftNet& del(const std::string& path, handler_t handler);
SwiftNet& patch(const std::string& path, handler_t handler);
SwiftNet& options(const std::string& path, handler_t handler);
SwiftNet& head(const std::string& path, handler_t handler);
```

#### **Middleware**
```cpp
SwiftNet& use(middleware_t middleware);
SwiftNet& use(const std::string& path, middleware_t middleware);
SwiftNet& cors(const std::string& origin = "*");
SwiftNet& json(size_t limit = 1024 * 1024);
SwiftNet& logger();
```

#### **Server Control**
```cpp
void listen(std::function<void()> callback = nullptr);
void listen(uint16_t port, std::function<void()> callback = nullptr);
void close();
```

### **Request Class**

```cpp
const std::string& method() const;
const std::string& path() const;
const std::string& body() const;
std::string header(const std::string& name) const;
std::string query(const std::string& name) const;
std::string param(const std::string& name) const;
bool is_json() const;
Json json() const;
```

### **Response Class**

```cpp
Response& status(int code);
Response& header(const std::string& name, const std::string& value);
Response& text(const std::string& content);
Response& html(const std::string& content);
Response& json(const Json& data);
Response& file(const std::string& filepath);
Response& redirect(const std::string& url, int code = 302);
```

## 🧪 **Testing & Examples**

### **Run Performance Tests**

```bash
# Build and run comprehensive performance test
cd build
make performance_test
./examples/performance_test

# Expected output:
# ✅ Virtual thread mounting on CPU cores
# ✅ Automatic unmounting during I/O operations  
# ✅ Work-stealing scheduler across N cores
# ✅ Load balancing and CPU affinity optimization
# ✅ Sophisticated I/O suspension and resumption
# ✅ Zero CPU idle time - cores always busy
```

### **Run HTTP Server Examples**

```bash
# Basic HTTP server with virtual threads
./examples/basic_server

# Test with curl:
curl http://localhost:8080/
curl http://localhost:8080/user/123
curl http://localhost:8080/stats
```

### **Load Testing**

```bash
# Test with wrk for high concurrency
wrk -t12 -c1000 -d30s http://localhost:8080/user/123

# Expected: High throughput with automatic virtual thread management
```

## 🔧 **Configuration**

### **Virtual Thread Scheduler Tuning**

```cpp
// Set number of worker threads (default: hardware_concurrency)
app.set_threads(8);

// Configure TCP backlog
app.set_backlog(2048);

// Get scheduler instance for advanced control
auto& scheduler = vthread_scheduler::instance();
scheduler.start(8); // 8 cores
```

### **Memory Pool Configuration**

```cpp
// Per-core memory arenas (automatically configured)
// 1MB per core by default, can be customized via scheduler
```

## 📊 **Monitoring & Observability**

### **Real-time Statistics**

```cpp
auto stats = vthread_scheduler::instance().get_stats();

// Core metrics
stats.total_scheduled;      // Total virtual threads scheduled
stats.total_io_suspended;   // Total I/O suspensions
stats.total_resumed;        // Total resumptions from I/O
stats.work_stolen;          // Work-stealing events
stats.context_switches;     // Virtual thread context switches

// Per-core breakdown
for (size_t i = 0; i < stats.per_core_executed.size(); ++i) {
    std::cout << "Core " << i << ": " << stats.per_core_executed[i] << std::endl;
}
```

### **Built-in Endpoints**

```cpp
// Statistics endpoint (automatically available)
GET /stats
{
  "scheduler_statistics": {
    "total_scheduled": 100,
    "total_io_suspended": 85,
    "work_stolen": 12,
    "context_switches": 156
  },
  "per_core_execution": [
    {"core": 0, "executed": 25},
    {"core": 1, "executed": 31},
    {"core": 2, "executed": 22},
    {"core": 3, "executed": 22}
  ]
}
```

## 🏆 **Benchmarks**

### **Performance Testing**

To validate the theoretical advantages of SwiftNet's virtual thread architecture, we conduct comprehensive benchmarks against Node.js using identical server implementations.

- **Hardware**: Apple M1 Pro (10 cores), macOS 26, native arm64 builds.
- **Tool**: [wrk](https://github.com/wg/wrk), HTTP/1.1 keep-alive, each server measured **in isolation** with a warm-up run.
- **Compared**: SwiftNet vs **Node.js 22 / Express** vs **Spring Boot 3.4 (Java 23 virtual threads)**.

#### **Benchmark Results**

`GET /` at `wrk -t12 -c1000` (Requests/sec, p99 latency):

| Server | Req/sec | p99 | vs Node |
|---|--:|--:|--:|
| Spring Boot (virtual threads) | 122,831 | 21.6 ms | 7.2× |
| **SwiftNet** | **67,355** | **64.4 ms** | **3.9×** |
| Node.js / Express | 17,086 | 643 ms¹ | 1.0× |

¹ Node sheds connections at `-c1000` (single event loop saturated).

**Honest takeaway:** SwiftNet beats Node.js/Express by **~3.4–3.9×** (and far better tail latency), but the mature **Spring Boot + JVM** stack is currently **~1.8× ahead** of SwiftNet. Full methodology, both endpoints, two concurrency levels, complete percentiles, analysis, and the headroom items that explain the Spring Boot gap are in **[BENCHMARKS.md](BENCHMARKS.md)**.

#### **Running Benchmarks**

```bash
# Build SwiftNet (native arm64 release)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 && cmake --build build -j

# Reference servers
(cd benchmark && npm install)                    # Node.js / Express
(cd benchmark/springboot && gradle bootJar)      # Spring Boot (Java 21+)

# SwiftNet vs Node.js                            ./benchmark/run_comparison.sh
# SwiftNet vs Node.js vs Spring Boot (deep run)  T=12 C=1000 D=20s ./benchmark/bench_all.sh
```

## 🧠 **Advanced Concepts**

### **Virtual Thread Lifecycle**

```
┌─────────────┐    ┌──────────────┐    ┌─────────────┐
│   CREATED   │───▶│   MOUNTED    │───▶│  EXECUTING  │
└─────────────┘    └──────────────┘    └─────────────┘
                                              │
┌─────────────┐    ┌──────────────┐          │ I/O
│ COMPLETED   │◀───│   UNMOUNTED  │◀─────────┘
└─────────────┘    └──────────────┘
       │                   │
       ▼                   ▼ I/O Complete
   [CLEANUP]         [RE-MOUNTED]
```

### **Work-Stealing Algorithm**

```cpp
// Pseudo-code for work-stealing
if (local_queue.empty()) {
    for (int attempts = 0; attempts < 4; ++attempts) {
        victim_core = random() % num_cores;
        if (victim_core != current_core) {
            if (auto stolen_task = victim_queue.try_pop()) {
                mount_vthread(stolen_task, current_core);
                execute_vthread(stolen_task);
                return true;
            }
        }
    }
}
```

### **Development Setup**

```bash
git clone https://github.com/NeuralRevenant/swiftnet.git
cd swiftnet
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON ..
make -j$(nproc)
./tests/swiftnet_tests
```

## 📄 **License**

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🙏 **Acknowledgments**

- **C++20 Coroutines** for enabling elegant async/await syntax
- **io_uring**, **kqueue**, and **IOCP** for high-performance I/O
- **Work-stealing** algorithms from academic research
- **Lock-free data structures** for scalable concurrency

---

<div align="center">

**🚀 SwiftNet: High-Performance C++ Networking Library 🚀**

*Built with ❤️ and sophisticated virtual thread technology*

• [Examples](examples/)

</div> 