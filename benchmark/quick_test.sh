#!/bin/bash

echo "Quick SwiftNet vs Node.js Performance Test"
echo "=========================================="

# Start Node.js server
cd benchmark
node nodejs-server.js &
NODEJS_PID=$!
cd ..

# Start SwiftNet server
cd build
./examples/basic_server &
SWIFTNET_PID=$!
cd ..

sleep 3

echo "Testing Node.js (port 3000)..."
wrk -t4 -c100 -d10s http://localhost:3000/ | grep -E "(Requests/sec|Latency)"

echo ""
echo "Testing SwiftNet (port 8080)..."
wrk -t4 -c100 -d10s http://localhost:8080/ | grep -E "(Requests/sec|Latency)"

# Clean up
kill $NODEJS_PID $SWIFTNET_PID 2>/dev/null

echo ""
echo "Quick test completed!"
