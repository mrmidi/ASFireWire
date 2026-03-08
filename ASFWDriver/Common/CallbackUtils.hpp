#pragma once

#include <memory>
#include <type_traits>
#include <utility>

namespace ASFW::Common {

template <typename Callback>
using SharedCallback = std::shared_ptr<std::decay_t<Callback>>;

template <typename Callback>
[[nodiscard]] auto ShareCallback(Callback&& callback)
    -> SharedCallback<Callback>
{
    return std::make_shared<std::decay_t<Callback>>(std::forward<Callback>(callback));
}

template <typename Callback, typename... Args>
void InvokeSharedCallback(const std::shared_ptr<Callback>& callback, Args&&... args)
{
    (*callback)(std::forward<Args>(args)...);
}

} // namespace ASFW::Common
