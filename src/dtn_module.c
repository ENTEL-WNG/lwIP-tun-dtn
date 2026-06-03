// dtn_module.c: Implementation of the DTN Module initialization and cleanup functions
// Copyright (C) 2025 Michael Karpov
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "dtn_module.h"

#include <stdio.h>
#include <stdlib.h>

#include "dtn_controller.h"
#include "dtn_logger.h"
#include "dtn_routing.h"
#include "dtn_storage.h"

DTN_Module* dtn_module_init(void) {
    DTN_INFO("Initializing DTN Module...");
    DTN_Module* module = (DTN_Module*)malloc(sizeof(DTN_Module));
    if (!module) {
        DTN_ERROR("Failed to allocate memory for DTN_Module");
        return NULL;
    }

    module->routing = NULL;
    module->storage = NULL;

    module->routing = dtn_routing_create(module);
    module->storage = dtn_storage_create(module);

    if (!module->routing || !module->storage) {
        DTN_ERROR("Failed to create one or more DTN components.");
        dtn_module_cleanup(module);
        return NULL;
    }

    // dtn_controller_stats_timer_start();
    DTN_INFO("DTN Module initialized successfully.");
    return module;
}

void dtn_module_cleanup(DTN_Module* module) {
    if (!module)
        return;
    DTN_INFO("Cleaning up DTN Module...");

    dtn_routing_destroy(module->routing);
    dtn_storage_destroy(module->storage);

    free(module);
}