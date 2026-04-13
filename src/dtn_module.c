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
#include "dtn_routing.h"
#include "dtn_storage.h"

DTN_Module* dtn_module_init(void) {
    printf("Initializing DTN Module...\n");
    DTN_Module* module = (DTN_Module*)malloc(sizeof(DTN_Module));
    if (!module) {
        perror("Failed to allocate memory for DTN_Module");
        return NULL;
    }

    module->controller = NULL;
    module->routing = NULL;
    module->storage = NULL;

    module->controller = dtn_controller_create(module);
    module->routing = dtn_routing_create(module);
    module->storage = dtn_storage_create(module);

    if (!module->controller || !module->routing || !module->storage) {
        fprintf(stderr, "Failed to create one or more DTN components.\n");
        dtn_module_cleanup(module);
        return NULL;
    }

    printf("DTN Module initialized successfully.\n");
    return module;
}

void dtn_module_cleanup(DTN_Module* module) {
    if (!module)
        return;
    printf("Cleaning up DTN Module...\n");

    dtn_controller_destroy(module->controller);
    dtn_routing_destroy(module->routing);
    dtn_storage_destroy(module->storage);

    free(module);
}