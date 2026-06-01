const express = require('express');
const app = express();
const port = 3000;

// Middleware
app.use(express.json());

// Simulated async database lookup (like SwiftNet's example)
const asyncDatabaseLookup = (userId) => {
    return new Promise((resolve) => {
        // Simulate I/O delay (same as SwiftNet's 100 microseconds)
        setTimeout(() => {
            resolve({
                id: userId,
                name: `User ${userId}`,
                email: `user${userId}@example.com`,
                status: 'active'
            });
        }, 0.1); // 0.1ms = 100 microseconds
    });
};

// Routes equivalent to SwiftNet examples
app.get('/', (req, res) => {
    res.send('<h1>Node.js: High-Performance JavaScript Server!</h1>');
});

app.get('/user/:id', async (req, res) => {
    const userId = req.params.id;
    
    // Async database lookup (equivalent to SwiftNet's co_await)
    const userData = await asyncDatabaseLookup(userId);
    
    res.json({
        user_id: userId,
        data: userData,
        processed_by: 'nodejs_async_await'
    });
});

app.get('/search', (req, res) => {
    const query = req.query.q;
    if (!query) {
        return res.status(400).json({ error: 'Missing query parameter q' });
    }
    
    // Simulate async search (same delay as SwiftNet)
    setTimeout(() => {
        res.json({
            query: query,
            results: [
                'Node.js Event Loop Result 1',
                'V8 JavaScript Engine Result 2',
                'Single-threaded Async Result 3'
            ],
            total: 3,
            processing_info: 'Processed by Node.js event loop'
        });
    }, 0.05); // 50 microseconds
});

app.post('/api/users', (req, res) => {
    const userData = req.body;
    
    // Validation (same as SwiftNet)
    if (!userData.name || !userData.email) {
        return res.status(400).json({
            error: 'Missing required fields',
            required: ['name', 'email']
        });
    }
    
    // Simulate async database write (same delay as SwiftNet)
    setTimeout(() => {
        res.status(201).json({
            id: 123,
            name: userData.name,
            email: userData.email,
            created_at: new Date().toISOString(),
            processing_details: {
                nodejs: 'processed by event loop',
                engine: 'V8 JavaScript engine',
                async: 'promise-based async/await'
            }
        });
    }, 0.2); // 200 microseconds
});

app.get('/stats', (req, res) => {
    const memUsage = process.memoryUsage();
    const cpuUsage = process.cpuUsage();
    
    res.json({
        nodejs_statistics: {
            memory_usage: {
                rss: memUsage.rss,
                heap_used: memUsage.heapUsed,
                heap_total: memUsage.heapTotal,
                external: memUsage.external
            },
            cpu_usage: {
                user: cpuUsage.user,
                system: cpuUsage.system
            },
            event_loop: {
                active_handles: process._getActiveHandles().length,
                active_requests: process._getActiveRequests().length
            }
        },
        features: [
            'Single-threaded event loop',
            'V8 JavaScript engine with JIT',
            'Promise-based async/await',
            'libuv for I/O operations',
            'Garbage collection memory management'
        ]
    });
});

app.get('/stress', (req, res) => {
    // Simulate stress test with many async operations
    const promises = [];
    for (let i = 0; i < 100; i++) {
        promises.push(new Promise(resolve => {
            setTimeout(() => resolve(i), 1);
        }));
    }
    
    Promise.all(promises).then(() => {
        res.json({
            message: 'Stress test: Many async operations completed',
            async_operations_created: 100,
            event_loop_features: [
                'Single-threaded execution',
                'Non-blocking I/O via libuv',
                'Promise-based concurrency',
                'Garbage collected memory',
                'Callback queue processing'
            ]
        });
    });
});

// Error handling
app.get('/error', (req, res) => {
    res.status(500).json({
        error: 'This is an intentional error for testing Node.js error handling'
    });
});

app.listen(port, () => {
    console.log(`Node.js server listening on port ${port}`);
    console.log('Features:');
    console.log('  ✓ Single-threaded event loop');
    console.log('  ✓ V8 JavaScript engine with JIT compilation');
    console.log('  ✓ Promise-based async/await');
    console.log('  ✓ libuv for cross-platform I/O');
    console.log('  ✓ Automatic garbage collection');
    console.log('');
    console.log('Endpoints:');
    console.log('  GET  /           - Welcome page');
    console.log('  GET  /user/:id   - User profile (async)');
    console.log('  GET  /search?q=  - Search (async)');
    console.log('  POST /api/users  - Create user (JSON + async)');
    console.log('  GET  /stress     - Stress test (many async ops)');
    console.log('  GET  /stats      - Node.js statistics');
    console.log('  GET  /error      - Error example');
});
