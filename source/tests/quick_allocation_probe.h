// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include "physical_interactions/quick_controller.h"
namespace QuickAllocationProbe {
extern thread_local bool tracking;
extern thread_local std::size_t allocations;
extern std::size_t updates, cancellations;
struct Scope {
    Scope(const char* operation);
    ~Scope();
    const char* operation;
    std::size_t before;
    bool previous;
};
class Controller : public PhysicalInteractions::QuickController {
public:
    void Update(const VR::Quick::Config& config, const VR::Quick::Input& input,
                PhysicalInteractions::QuickOutput& output) {
        ++updates;
        Scope scope{"Update"};
        PhysicalInteractions::QuickController::Update(config, input, output);
    }
    void Cancel(PhysicalInteractions::QuickOutput& output, const char* why) {
        ++cancellations;
        Scope scope{"Cancel"};
        PhysicalInteractions::QuickController::Cancel(output, why);
    }
};
} // namespace QuickAllocationProbe
