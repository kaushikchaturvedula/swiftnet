// Fair Node.js baseline: Express across ALL cores via the cluster module.
// (The single-process nodejs-server.js is kept only as a documented worst case.)
// Endpoints mirror SwiftNet basic_server and the Spring Boot reference with
// equivalent work (small sync JSON; no artificial delay).
const cluster = require('cluster');
const os = require('os');
const express = require('express');

const PORT = 3000;

if (cluster.isPrimary) {
    const n = os.cpus().length;
    console.log(`node cluster: forking ${n} workers on :${PORT}`);
    for (let i = 0; i < n; i++) cluster.fork();
    cluster.on('exit', () => cluster.fork()); // keep the pool full
} else {
    const app = express();
    app.use(express.json());
    app.get('/', (req, res) => {
        res.type('text/html').send('<h1>Node.js cluster (Express) server</h1>');
    });
    app.get('/user/:id', (req, res) => {
        res.json({ user_id: req.params.id, name: 'User ' + req.params.id, processed_by: 'node_cluster' });
    });
    app.listen(PORT);
}
