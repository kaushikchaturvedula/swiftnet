// Static File Server Example for SwiftNet
// Demonstrates serving static files with proper MIME types and error handling

#include "swiftnet.hpp"
#include <iostream>
#include <filesystem>
#include <fstream>

using namespace swiftnet;
namespace fs = std::filesystem;

int main() {
    SwiftNet app(8080);

    // Security middleware - prevent directory traversal
    app.use([](Request& req, Response& res, std::function<void()> next) {
        if (req.path().find("..") != std::string::npos) {
            res.status(403).text("Forbidden: Directory traversal not allowed");
            return;
        }
        next();
    });

    // Logging middleware
    app.use([](Request& req, Response& res, std::function<void()> next) {
        std::cout << "[" << req.method() << "] " << req.path() << std::endl;
        next();
    });

    // API endpoint
    app.get("/api/files", [](Request& req, Response& res) {
        Json response;
        response["message"] = "File API endpoint";
        response["timestamp"] = "2024-01-01T00:00:00Z";
        res.json(response);
    });

    // Serve index.html for root
    app.get("/", [](Request& req, Response& res) {
        res.html(R"(
<!DOCTYPE html>
<html>
<head>
    <title>SwiftNet File Server</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; }
        .container { max-width: 800px; margin: 0 auto; }
        .file-list { list-style: none; padding: 0; }
        .file-item { 
            padding: 10px; 
            border: 1px solid #ddd; 
            margin: 5px 0; 
            border-radius: 4px;
        }
        .file-item:hover { background-color: #f5f5f5; }
        .file-link { text-decoration: none; color: #333; }
        .file-size { color: #666; font-size: 0.9em; float: right; }
    </style>
</head>
<body>
    <div class="container">
        <h1>SwiftNet File Server</h1>
        <p>A high-performance C++ web server with Node.js-like API</p>
        
        <h2>Test Files</h2>
        <ul class="file-list">
            <li class="file-item">
                <a href="/files/sample.txt" class="file-link">sample.txt</a>
                <span class="file-size">Sample text file</span>
            </li>
            <li class="file-item">
                <a href="/files/data.json" class="file-link">data.json</a>
                <span class="file-size">JSON data file</span>
            </li>
            <li class="file-item">
                <a href="/files/style.css" class="file-link">style.css</a>
                <span class="file-size">CSS stylesheet</span>
            </li>
        </ul>

        <h2>API Endpoints</h2>
        <ul>
            <li><a href="/api/files">GET /api/files</a> - File API</li>
            <li><a href="/upload">POST /upload</a> - Upload file (form below)</li>
        </ul>

        <h2>Upload File</h2>
        <form action="/upload" method="post" enctype="multipart/form-data">
            <input type="file" name="file" required>
            <button type="submit">Upload</button>
        </form>
    </div>
</body>
</html>
        )");
    });

    // Static file serving for /files/* routes
    app.get("/files/:filename", [](Request& req, Response& res) {
        std::string filename = req.param("filename");
        std::string filepath = "./public/" + filename;

        // Security check
        if (filename.find("..") != std::string::npos || filename.find("/") != std::string::npos) {
            res.status(403).text("Forbidden: Invalid filename");
            return;
        }

        try {
            if (utils::file_exists(filepath)) {
                // Get file size for Content-Length header
                size_t file_size = utils::file_size(filepath);
                res.header("Content-Length", std::to_string(file_size));
                
                // Add caching headers
                res.header("Cache-Control", "public, max-age=3600");
                res.header("ETag", "\"" + std::to_string(file_size) + "\"");
                
                // Serve the file
                res.file(filepath);
            } else {
                res.not_found("File not found: " + filename);
            }
        } catch (const std::exception& e) {
            res.internal_error("Error reading file: " + std::string(e.what()));
        }
    });

    // File upload endpoint
    app.post("/upload", [](Request& req, Response& res) {
        // This is a simplified example - real multipart parsing would be more complex
        if (req.body().empty()) {
            res.bad_request("No file uploaded");
            return;
        }

        // In a real implementation, you would parse multipart/form-data
        // For now, just return a success message
        Json response;
        response["message"] = "File upload received";
        response["size"] = static_cast<int>(req.body().size());
        response["note"] = "This is a demo - multipart parsing not fully implemented";
        
        res.status(201).json(response);
    });

    // Download file with custom filename
    app.get("/download/:filename", [](Request& req, Response& res) {
        std::string filename = req.param("filename");
        std::string filepath = "./public/" + filename;

        if (utils::file_exists(filepath)) {
            res.header("Content-Disposition", "attachment; filename=\"" + filename + "\"")
               .file(filepath);
        } else {
            res.not_found("File not found");
        }
    });

    // File information endpoint
    app.get("/info/:filename", [](Request& req, Response& res) {
        std::string filename = req.param("filename");
        std::string filepath = "./public/" + filename;

        if (utils::file_exists(filepath)) {
            Json info;
            info["filename"] = filename;
            info["size"] = static_cast<int>(utils::file_size(filepath));
            info["mime_type"] = utils::mime_type(filepath);
            info["path"] = filepath;
            
            // Additional file info (would require filesystem APIs)
            info["exists"] = true;
            info["readable"] = true;
            
            res.json(info);
        } else {
            Json error;
            error["error"] = "File not found";
            error["filename"] = filename;
            res.status(404).json(error);
        }
    });

    // Create public directory if it doesn't exist
    try {
        if (!fs::exists("./public")) {
            fs::create_directory("./public");
            std::cout << "Created ./public directory" << std::endl;
        }

        // Create sample files
        std::ofstream sample_txt("./public/sample.txt");
        sample_txt << "Hello from SwiftNet!\nThis is a sample text file.\n";
        sample_txt.close();

        std::ofstream sample_json("./public/data.json");
        sample_json << R"({"message": "Hello from SwiftNet", "version": "1.0.0", "features": ["high-performance", "node.js-like API", "modern C++"]})";
        sample_json.close();

        std::ofstream sample_css("./public/style.css");
        sample_css << "body { font-family: Arial, sans-serif; margin: 20px; }\n";
        sample_css << "h1 { color: #333; }\n";
        sample_css << ".highlight { background-color: yellow; }\n";
        sample_css.close();

        std::cout << "Created sample files in ./public/" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "Warning: Could not create sample files: " << e.what() << std::endl;
    }

    // Generic file serving (catch-all)
    app.get("/static/*", [](Request& req, Response& res) {
        std::string path = req.path();
        std::string filepath = "." + path; // ./static/...

        if (utils::file_exists(filepath)) {
            res.file(filepath);
        } else {
            res.not_found("Static file not found");
        }
    });

    // 404 handler
    app.get(".*", [](Request& req, Response& res) {
        res.status(404).html(R"(
            <h1>404 - Page Not Found</h1>
            <p>The requested file or page was not found.</p>
            <p><a href="/">Go back to home</a></p>
        )");
    });

    // Start server
    std::cout << "SwiftNet File Server Example" << std::endl;
    std::cout << "============================" << std::endl;
    std::cout << "Starting file server on http://localhost:8080" << std::endl;
    std::cout << "Available endpoints:" << std::endl;
    std::cout << "  GET  /                    - File browser interface" << std::endl;
    std::cout << "  GET  /files/:filename     - Serve files from ./public/" << std::endl;
    std::cout << "  GET  /download/:filename  - Download files with attachment header" << std::endl;
    std::cout << "  GET  /info/:filename      - Get file information as JSON" << std::endl;
    std::cout << "  POST /upload              - Upload files (demo)" << std::endl;
    std::cout << "  GET  /static/*            - Serve static files from ./static/" << std::endl;
    std::cout << std::endl;

    app.listen([]() {
        std::cout << "File server is running! Press Ctrl+C to stop." << std::endl;
    });

    return 0;
} 