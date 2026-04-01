// dtn_module.h: Header file for the main DTN Module that integrates controller, routing and storage
// functions Copyright (C) 2025 Michael Karpov
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

#ifndef DTN_MODULE_H
#define DTN_MODULE_H

struct DTN_Controller;
struct Routing_Function;
struct Storage_Function;

typedef struct DTN_Module {
    struct DTN_Controller* controller;
    struct Routing_Function* routing;
    struct Storage_Function* storage;
} DTN_Module;

DTN_Module* dtn_module_init(void);

void dtn_module_cleanup(DTN_Module* module);

#endif