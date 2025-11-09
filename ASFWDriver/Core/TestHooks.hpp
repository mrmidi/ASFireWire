#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace ASFW::Driver {

// Host-side testing shims that allow the core to run without DriverKit. These
// abstractions will be wired through dependency injection once unit tests are
// introduced.
class TestClock {
public:
    virtual ~TestClock() = default;
    virtual uint64_t Now() const = 0;
};

class SteadyTestClock final : public TestClock {
public:
    uint64_t Now() const override;
};

class InterruptTestHook {
public:
    using Handler = std::function<void()>;

    void Install(Handler handler);
    void Trigger();

private:
    Handler handler_;
};

} // namespace ASFW::Driver

