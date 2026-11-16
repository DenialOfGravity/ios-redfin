// (tests) Darwin's mach types, as much as the transport names
#pragma once
#include <cstdint>
typedef int kern_return_t;
typedef uint32_t mach_port_t;
typedef mach_port_t task_port_t;
typedef uint64_t mach_vm_address_t;
typedef uint64_t mach_vm_size_t;
constexpr kern_return_t KERN_SUCCESS = 0;
inline task_port_t mach_task_self() { return 1; }
