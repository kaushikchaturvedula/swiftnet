// Fastify across ALL cores via the cluster module — the strongest Node baseline.
const cluster = require('cluster');
const os = require('os');

const PORT = 3000;

if (cluster.isPrimary) {
    const n = os.cpus().length;
    console.log(`fastify cluster: forking ${n} workers on :${PORT}`);
    for (let i = 0; i < n; i++) cluster.fork();
    cluster.on('exit', () => cluster.fork());
} else {
    const fastify = require('fastify')({ logger: false });
    fastify.get('/', (req, reply) => {
        reply.type('text/html').send('<h1>Fastify cluster server</h1>');
    });
    fastify.get('/user/:id', (req, reply) => {
        reply.send({ user_id: req.params.id, name: 'User ' + req.params.id, processed_by: 'fastify_cluster' });
    });
    fastify.listen({ port: PORT, host: '0.0.0.0' }).catch((e) => { console.error(e); process.exit(1); });
}
