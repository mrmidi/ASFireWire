// SPDX-License-Identifier: Apache-2.0
#pragma once
namespace Pro40Lab {
int developmentControllerPreparationCommand();
// No XPC/dispatch object types cross the Objective-C ARC / C++ ABI boundary.
int developmentConnectionCommand(bool helper);
}
