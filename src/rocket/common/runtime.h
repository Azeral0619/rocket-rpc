#pragma once
#include <cstdint>

namespace google::protobuf {
class MethodDescriptor;
}

namespace rocket {

class RpcInterface;

class RunTime {
  public:
    [[nodiscard]] RpcInterface* getRpcInterface() const noexcept;

    // Kept inline so the hot logging/RPC paths compile down to a direct TLS
    // address lookup instead of an out-of-line function call.
    static RunTime* GetRunTime() noexcept {
        static thread_local RunTime runtime;
        return &runtime;
    }

    std::uint64_t m_msgid{0};
    // Generated protobuf descriptors live for the lifetime of the process.
    // Keeping this pointer avoids copying a method name on every request, log
    // record and coroutine suspend/resume.
    const google::protobuf::MethodDescriptor* m_method{nullptr};
    RpcInterface* m_rpc_interface{nullptr};
};

// Installs request identity only for the dynamic extent of a handler or
// completion callback. This prevents reused IO/worker threads from leaking the
// previous RPC's log context into unrelated work.
class RunTimeScope {
  public:
    RunTimeScope(std::uint64_t msgid,
                 const google::protobuf::MethodDescriptor* method) noexcept
        : m_runtime(RunTime::GetRunTime()),
          m_previous_msgid(m_runtime->m_msgid),
          m_previous_method(m_runtime->m_method) {
        m_runtime->m_msgid = msgid;
        m_runtime->m_method = method;
    }

    ~RunTimeScope() {
        m_runtime->m_msgid = m_previous_msgid;
        m_runtime->m_method = m_previous_method;
    }

    RunTimeScope(const RunTimeScope&) = delete;
    RunTimeScope& operator=(const RunTimeScope&) = delete;

  private:
    RunTime* m_runtime;
    std::uint64_t m_previous_msgid;
    const google::protobuf::MethodDescriptor* m_previous_method;
};

class RunTimeInterfaceScope {
  public:
    explicit RunTimeInterfaceScope(RpcInterface* rpc_interface) noexcept
        : m_runtime(RunTime::GetRunTime()),
          m_previous(m_runtime->m_rpc_interface) {
        m_runtime->m_rpc_interface = rpc_interface;
    }

    ~RunTimeInterfaceScope() { m_runtime->m_rpc_interface = m_previous; }

    RunTimeInterfaceScope(const RunTimeInterfaceScope&) = delete;
    RunTimeInterfaceScope& operator=(const RunTimeInterfaceScope&) = delete;

  private:
    RunTime* m_runtime;
    RpcInterface* m_previous;
};

} // namespace rocket
