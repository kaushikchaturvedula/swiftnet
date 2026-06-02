package com.swiftnet.bench;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.RestController;
import reactor.core.publisher.Mono;

import java.util.LinkedHashMap;
import java.util.Map;

// Spring WebFlux (Reactor Netty) reference server -- the fastest mainstream JVM
// stack, used as the strongest "fair JVM" bar alongside Spring Boot MVC + Loom.
// Endpoints mirror the SwiftNet/Node references with equivalent work.
@SpringBootApplication
@RestController
public class WebfluxApplication {

    public static void main(String[] args) {
        SpringApplication.run(WebfluxApplication.class, args);
    }

    @GetMapping(value = "/", produces = "text/html")
    public Mono<String> root() {
        return Mono.just("<h1>Spring WebFlux (Netty) server</h1>");
    }

    @GetMapping("/user/{id}")
    public Mono<Map<String, Object>> user(@PathVariable String id) {
        Map<String, Object> m = new LinkedHashMap<>();
        m.put("user_id", id);
        m.put("name", "User " + id);
        m.put("processed_by", "spring_webflux_netty");
        return Mono.just(m);
    }
}
