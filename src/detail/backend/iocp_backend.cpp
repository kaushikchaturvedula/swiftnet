// IOCP reactor backend (Windows) -- UNVERIFIED.
// S1: moved verbatim from the original src/event_loop.cpp (skeleton). S8 replaces
// this with a real OVERLAPPED completion port (WSARecv/WSASend).
#include "detail/backend/iface.hpp"

#if defined(SWIFTNET_BACKEND_IOCP)

#include <winsock2.h>
#include <windows.h>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace swiftnet::detail
{
    class iocp_backend final : public reactor_backend
    {
        void *iocp_;

    public:
        iocp_backend()
        {
            iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
            if (!iocp_)
                throw std::runtime_error("CreateIoCompletionPort failed");
        }

        ~iocp_backend() override { CloseHandle(static_cast<HANDLE>(iocp_)); }

        void arm(int fd, std::uint32_t mask, std::uint64_t token) override
        {
            // IOCP is completion-based; associate the socket so completions issued by the
            // socket layer surface here, keyed by token. (Readiness arming is a no-op.)
            CreateIoCompletionPort(reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd)),
                                   static_cast<HANDLE>(iocp_), static_cast<ULONG_PTR>(token), 0);
            (void)mask;
        }

        void arm_timer(std::uint64_t token, int ms) override
        {
            // Stub backend: post a completion after ms via a throwaway thread.
            void *iocp = iocp_;
            std::thread([iocp, token, ms]
                        {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                PostQueuedCompletionStatus(static_cast<HANDLE>(iocp), 0, static_cast<ULONG_PTR>(token), nullptr); })
                .detach();
        }

        void cancel(int fd, std::uint32_t mask) override
        {
            (void)fd;
            (void)mask;
        }

        int wait(io_event *evs, int max, int timeout_ms) override
        {
            (void)max;
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            LPOVERLAPPED ov = nullptr;
            DWORD to = (timeout_ms < 0) ? INFINITE : static_cast<DWORD>(timeout_ms);
            BOOL ok = GetQueuedCompletionStatus(static_cast<HANDLE>(iocp_), &bytes, &key, &ov, to);
            if (!ok && ov == nullptr)
                return 0; // timeout / no completion
            if (key == 0)
                return 0; // wake() nudge
            evs[0].token = static_cast<std::uint64_t>(key);
            evs[0].mask = READABLE | WRITABLE; // completion-based: readiness unknown
            evs[0].res = static_cast<int>(bytes);
            return 1;
        }

        void wake() override
        {
            PostQueuedCompletionStatus(static_cast<HANDLE>(iocp_), 0, 0, nullptr);
        }
    };

    std::unique_ptr<reactor_backend> make_iocp_backend()
    {
        return std::make_unique<iocp_backend>();
    }

} // namespace swiftnet::detail

#endif // SWIFTNET_BACKEND_IOCP
