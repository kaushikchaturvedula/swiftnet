package com.swiftnet.bench;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.RestController;

import java.util.LinkedHashMap;
import java.util.Map;

// Spring Boot MVC reference server for the SwiftNet benchmark. Endpoints mirror
// the SwiftNet basic_server and the Node.js/Express reference so the three are
// compared on equivalent work. Virtual threads are enabled in
// application.properties (spring.threads.virtual.enabled=true).
@SpringBootApplication
@RestController
public class BenchApplication {

    public static void main(String[] args) {
        SpringApplication.run(BenchApplication.class, args);
    }

    @GetMapping(value = "/", produces = "text/html")
    public String root() {
        return "<h1>Spring Boot benchmark server</h1>";
    }

    @GetMapping("/user/{id}")
    public Map<String, Object> user(@PathVariable String id) {
        Map<String, Object> m = new LinkedHashMap<>();
        m.put("user_id", id);
        m.put("name", "User " + id);
        m.put("processed_by", "spring_boot_virtual_threads");
        return m;
    }
}
