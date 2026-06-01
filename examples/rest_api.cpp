// REST API Example for SwiftNet
// Demonstrates building a complete REST API with CRUD operations

#include "swiftnet.hpp"
#include <iostream>
#include <unordered_map>
#include <mutex>

using namespace swiftnet;

// Simple in-memory database
class UserDatabase
{
private:
    std::unordered_map<int, Json> users_;
    std::mutex mutex_;
    int next_id_ = 1;

public:
    Json create_user(const Json &user_data)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        Json user = user_data;
        user["id"] = next_id_++;
        user["created_at"] = "2024-01-01T00:00:00Z"; // Simplified timestamp
        
        users_[user["id"]] = user;
        return user;
    }

    Json get_user(int id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = users_.find(id);
        return it != users_.end() ? it->second : Json{};
    }

    Json get_all_users()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Json users = Json::array();
        for (const auto &[id, user] : users_) {
            users.push_back(user);
        }
        return users;
    }

    bool update_user(int id, const Json &user_data)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = users_.find(id);
        if (it == users_.end()) {
            return false;
        }
        
        // Update existing fields
        for (auto &[key, value] : user_data.items()) {
            if (key != "id") { // Don't allow ID changes
                it->second[key] = value;
            }
        }
        it->second["updated_at"] = "2024-01-01T00:00:00Z";
        
        return true;
    }

    bool delete_user(int id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return users_.erase(id) > 0;
    }
};

int main()
{
    std::cout << "SwiftNet REST API Example" << std::endl;
    std::cout << "=========================" << std::endl;
    std::cout << "Starting RESTful API server on http://localhost:3000" << std::endl;
    std::cout << "Endpoints:" << std::endl;
    std::cout << "  GET    /api/users      - Get all users" << std::endl;
    std::cout << "  GET    /api/users/:id  - Get user by ID" << std::endl;
    std::cout << "  POST   /api/users      - Create new user" << std::endl;
    std::cout << "  PUT    /api/users/:id  - Update user by ID" << std::endl;
    std::cout << "  DELETE /api/users/:id  - Delete user by ID" << std::endl;
    std::cout << std::endl;

    SwiftNet app(3000);
    UserDatabase db;

    // Enable middleware
    app.logger();
    app.cors();
    app.json(1024 * 1024); // 1MB JSON limit

    // API versioning middleware
    app.use("/api/*", [](Request &req, Response &res, std::function<void()> next) {
        res.header("X-API-Version", "1.0")
           .header("Content-Type", "application/json");
        next();
    });

    // GET /api/users - Get all users
    app.get("/api/users", [&db](Request &req, Response &res) {
        Json response;
        response["users"] = db.get_all_users();
        response["total"] = response["users"].size();
        
        res.json(response);
    });

    // GET /api/users/:id - Get user by ID
    app.get("/api/users/:id", [&db](Request &req, Response &res) {
        try {
            int user_id = std::stoi(req.param("id"));
            Json user = db.get_user(user_id);
            
            if (user.empty()) {
                res.not_found("User not found with ID: " + std::to_string(user_id));
                return;
            }
            
            res.json(user);
        } catch (const std::exception &e) {
            res.bad_request("Invalid user ID format");
        }
    });

    // POST /api/users - Create new user
    app.post("/api/users", [&db](Request &req, Response &res) {
        if (!req.is_json()) {
            res.bad_request("Content-Type must be application/json");
            return;
        }

        try {
            Json user_data = req.json();
            
            // Validate required fields
            if (!user_data.contains("name") || !user_data.contains("email")) {
                Json error;
                error["error"] = "Missing required fields";
                error["required"] = Json::array({"name", "email"});
                res.status(400).json(error);
                return;
            }

            // Validate email format (basic)
            std::string email = user_data["email"];
            if (email.find('@') == std::string::npos) {
                Json error;
                error["error"] = "Invalid email format";
                res.status(400).json(error);
                return;
            }

            Json new_user = db.create_user(user_data);
            res.created(new_user);
            
        } catch (const std::exception &e) {
            Json error;
            error["error"] = "Invalid JSON data";
            error["message"] = e.what();
            res.status(400).json(error);
        }
    });

    // PUT /api/users/:id - Update user by ID
    app.put("/api/users/:id", [&db](Request &req, Response &res) {
        if (!req.is_json()) {
            res.bad_request("Content-Type must be application/json");
            return;
        }

        try {
            int user_id = std::stoi(req.param("id"));
            Json update_data = req.json();
            
            if (db.update_user(user_id, update_data)) {
                Json updated_user = db.get_user(user_id);
                res.json(updated_user);
            } else {
                res.not_found("User not found with ID: " + std::to_string(user_id));
            }
            
        } catch (const std::exception &e) {
            res.bad_request("Invalid request: " + std::string(e.what()));
        }
    });

    // DELETE /api/users/:id - Delete user by ID
    app.del("/api/users/:id", [&db](Request &req, Response &res) {
        try {
            int user_id = std::stoi(req.param("id"));
            
            if (db.delete_user(user_id)) {
                Json response;
                response["message"] = "User deleted successfully";
                response["id"] = user_id;
                res.json(response);
            } else {
                res.not_found("User not found with ID: " + std::to_string(user_id));
            }
            
        } catch (const std::exception &e) {
            res.bad_request("Invalid user ID format");
        }
    });

    // Health check endpoint
    app.get("/health", [](Request &req, Response &res) {
        Json health;
        health["status"] = "healthy";
        health["timestamp"] = "2024-01-01T00:00:00Z";
        health["version"] = "1.0.0";
        
        res.json(health);
    });

    // 404 handler for API routes
    app.get("/api/*", [](Request &req, Response &res) {
        Json error;
        error["error"] = "Endpoint not found";
        error["path"] = req.path();
        error["method"] = req.method();
        
        res.status(404).json(error);
    });

    std::cout << "REST API server is running! Press Ctrl+C to stop." << std::endl;
    std::cout << "Try: curl -X GET http://localhost:3000/api/users" << std::endl;
    std::cout << "Try: curl -X POST http://localhost:3000/api/users -H \"Content-Type: application/json\" -d '{\"name\":\"John Doe\",\"email\":\"john@example.com\"}'" << std::endl;
    
    // Start the server
    app.listen([]() {
        std::cout << "SwiftNet REST API listening on port 3000" << std::endl;
    });

    return 0;
} 