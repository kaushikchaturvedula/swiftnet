#include "ws/websocket.hpp"
#include <array>
#include <cstdint>
#include <cstring>

using namespace swiftnet;
using namespace swiftnet::ws;

namespace
{
    // --- SHA-1 (RFC 3174), enough for the WebSocket handshake ---
    std::string sha1(const std::string &msg)
    {
        std::uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
                      h3 = 0x10325476, h4 = 0xC3D2E1F0;

        std::string data = msg;
        std::uint64_t ml = static_cast<std::uint64_t>(data.size()) * 8;
        data.push_back((char)0x80);
        while (data.size() % 64 != 56)
            data.push_back((char)0x00);
        for (int i = 7; i >= 0; --i)
            data.push_back((char)((ml >> (i * 8)) & 0xFF));

        auto rol = [](std::uint32_t v, int b) { return (v << b) | (v >> (32 - b)); };

        for (std::size_t chunk = 0; chunk < data.size(); chunk += 64)
        {
            std::uint32_t w[80];
            for (int i = 0; i < 16; ++i)
                w[i] = (std::uint8_t(data[chunk + i * 4]) << 24) |
                       (std::uint8_t(data[chunk + i * 4 + 1]) << 16) |
                       (std::uint8_t(data[chunk + i * 4 + 2]) << 8) |
                       (std::uint8_t(data[chunk + i * 4 + 3]));
            for (int i = 16; i < 80; ++i)
                w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

            std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
            for (int i = 0; i < 80; ++i)
            {
                std::uint32_t f, k;
                if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
                else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
                else { f = b ^ c ^ d; k = 0xCA62C1D6; }
                std::uint32_t tmp = rol(a, 5) + f + e + k + w[i];
                e = d; d = c; c = rol(b, 30); b = a; a = tmp;
            }
            h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
        }

        std::string out(20, '\0');
        std::uint32_t hs[5] = {h0, h1, h2, h3, h4};
        for (int i = 0; i < 5; ++i)
        {
            out[i * 4] = (char)((hs[i] >> 24) & 0xFF);
            out[i * 4 + 1] = (char)((hs[i] >> 16) & 0xFF);
            out[i * 4 + 2] = (char)((hs[i] >> 8) & 0xFF);
            out[i * 4 + 3] = (char)(hs[i] & 0xFF);
        }
        return out;
    }

    std::string base64(const std::string &in)
    {
        static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int val = 0, bits = -6;
        for (unsigned char c : in)
        {
            val = (val << 8) + c;
            bits += 8;
            while (bits >= 0)
            {
                out.push_back(tbl[(val >> bits) & 0x3F]);
                bits -= 6;
            }
        }
        if (bits > -6)
            out.push_back(tbl[((val << 8) >> (bits + 8)) & 0x3F]);
        while (out.size() % 4)
            out.push_back('=');
        return out;
    }

    constexpr std::size_t kMaxFrame = 16 * 1024 * 1024; // 16 MiB per frame cap
} // namespace

std::string WebSocket::accept_key(const std::string &client_key)
{
    static const char *kMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    return base64(sha1(client_key + kMagic));
}

vthread_base<bool> WebSocket::ensure(std::size_t n)
{
    char tmp[4096];
    while (rbuf_.size() < n)
    {
        int r = co_await sock_.async_read(tmp, sizeof(tmp));
        if (r <= 0)
            co_return false; // EOF or error
        rbuf_.append(tmp, static_cast<std::size_t>(r));
    }
    co_return true;
}

vthread_base<int> WebSocket::send_frame(std::uint8_t opcode, const std::string &data)
{
    std::string frame;
    frame.push_back((char)(0x80 | opcode)); // FIN + opcode
    if (data.size() < 126)
    {
        frame.push_back((char)data.size());
    }
    else if (data.size() <= 0xFFFF)
    {
        frame.push_back((char)126);
        frame.push_back((char)((data.size() >> 8) & 0xFF));
        frame.push_back((char)(data.size() & 0xFF));
    }
    else
    {
        frame.push_back((char)127);
        for (int i = 7; i >= 0; --i)
            frame.push_back((char)((static_cast<std::uint64_t>(data.size()) >> (i * 8)) & 0xFF));
    }
    frame += data;
    int w = co_await sock_.async_write(frame.data(), frame.size());
    co_return w;
}

vthread_base<int> WebSocket::send_text(std::string data) { return send_frame(0x1, data); }
vthread_base<int> WebSocket::send_binary(std::string data) { return send_frame(0x2, data); }

vthread_base<int> WebSocket::close(std::uint16_t code)
{
    std::string payload;
    payload.push_back((char)((code >> 8) & 0xFF));
    payload.push_back((char)(code & 0xFF));
    int w = co_await send_frame(0x8, payload);
    co_return w;
}

vthread_base<WebSocket::message> WebSocket::recv()
{
    const message closed_msg{"", false, true};
    std::string payload;
    bool is_binary = false;

    while (true)
    {
        if (!(co_await ensure(2)))
            co_return closed_msg;

        std::uint8_t b0 = (std::uint8_t)rbuf_[0];
        std::uint8_t b1 = (std::uint8_t)rbuf_[1];
        bool fin = b0 & 0x80;
        std::uint8_t opcode = b0 & 0x0F;
        bool masked = b1 & 0x80;
        std::uint64_t len = b1 & 0x7F;
        std::size_t pos = 2;

        if (len == 126)
        {
            if (!(co_await ensure(4)))
                co_return closed_msg;
            len = ((std::uint8_t)rbuf_[2] << 8) | (std::uint8_t)rbuf_[3];
            pos = 4;
        }
        else if (len == 127)
        {
            if (!(co_await ensure(10)))
                co_return closed_msg;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | (std::uint8_t)rbuf_[2 + i];
            pos = 10;
        }

        std::uint8_t mask[4] = {0, 0, 0, 0};
        if (masked)
        {
            if (!(co_await ensure(pos + 4)))
                co_return closed_msg;
            for (int i = 0; i < 4; ++i)
                mask[i] = (std::uint8_t)rbuf_[pos + i];
            pos += 4;
        }

        if (len > kMaxFrame || payload.size() + len > kMaxFrame)
            co_return closed_msg; // oversized

        if (!(co_await ensure(pos + len)))
            co_return closed_msg;

        std::string frame_payload = rbuf_.substr(pos, len);
        if (masked)
            for (std::size_t i = 0; i < frame_payload.size(); ++i)
                frame_payload[i] ^= (char)mask[i % 4];
        rbuf_.erase(0, pos + len);

        switch (opcode)
        {
        case 0x8: // close
            co_await close();
            co_return closed_msg;
        case 0x9: // ping -> pong
            co_await send_frame(0xA, frame_payload);
            continue;
        case 0xA: // pong
            continue;
        case 0x1: // text
            is_binary = false;
            break;
        case 0x2: // binary
            is_binary = true;
            break;
        case 0x0: // continuation: keep is_binary
            break;
        default:
            co_return closed_msg; // unknown opcode
        }

        payload += frame_payload;
        if (fin)
            co_return message{std::move(payload), is_binary, false};
    }
}
