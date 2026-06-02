// Fastify (single process) — the realistic high-performance Node baseline.
// Same endpoints/work as the other reference servers.
const fastify = require('fastify')({ logger: false });

fastify.get('/', (req, reply) => {
    reply.type('text/html').send('<h1>Fastify server</h1>');
});
fastify.get('/user/:id', (req, reply) => {
    reply.send({ user_id: req.params.id, name: 'User ' + req.params.id, processed_by: 'fastify' });
});

fastify.listen({ port: 3000, host: '0.0.0.0' }).catch((e) => { console.error(e); process.exit(1); });
