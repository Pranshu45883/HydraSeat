#pragma once

#include "hydra/gate_c_adapter.h"

#include <stdint.h>

#if defined(_WIN32)
#  if defined(HYDRA_GATE_C_SHIM_BUILD)
#    define HYDRA_GATE_C_SHIM_API __declspec(dllexport)
#  else
#    define HYDRA_GATE_C_SHIM_API __declspec(dllimport)
#  endif
#  define HYDRA_GATE_C_SHIM_CALL __cdecl
#else
#  define HYDRA_GATE_C_SHIM_API __attribute__((visibility("default")))
#  define HYDRA_GATE_C_SHIM_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define HYDRA_GATE_C_SHIM_API_VERSION 4u
#define HYDRA_GATE_C_SHIM_CONFIG_V1_BYTES 24u
#define HYDRA_GATE_C_SHIM_CONFIG_V2_BYTES 32u
#define HYDRA_GATE_C_SHIM_CONFIG_V3_BYTES 40u
#define HYDRA_GATE_C_SHIM_STATUS_V1_BYTES 60u
#define HYDRA_GATE_C_SHIM_GET_ASYNC_KEY_STATE_BIT 0x00000001u
#define HYDRA_GATE_C_SHIM_GET_KEY_STATE_BIT 0x00000002u
#define HYDRA_GATE_C_SHIM_GET_KEYBOARD_STATE_BIT 0x00000004u
#define HYDRA_GATE_C_SHIM_POLLING_API_MASK 0x00000007u
#define HYDRA_GATE_C_SHIM_GET_CURSOR_POS_BIT 0x00000008u
#define HYDRA_GATE_C_SHIM_SET_CURSOR_POS_BIT 0x00000010u
#define HYDRA_GATE_C_SHIM_CLIP_CURSOR_BIT 0x00000020u
#define HYDRA_GATE_C_SHIM_GET_CLIP_CURSOR_BIT 0x00000040u
#define HYDRA_GATE_C_SHIM_GET_FOREGROUND_WINDOW_BIT 0x00000080u
#define HYDRA_GATE_C_SHIM_GET_ACTIVE_WINDOW_BIT 0x00000100u
#define HYDRA_GATE_C_SHIM_GET_FOCUS_BIT 0x00000200u
#define HYDRA_GATE_C_SHIM_GET_CAPTURE_BIT 0x00000400u
#define HYDRA_GATE_C_SHIM_SET_CAPTURE_BIT 0x00000800u
#define HYDRA_GATE_C_SHIM_RELEASE_CAPTURE_BIT 0x00001000u
#define HYDRA_GATE_C_SHIM_CURSOR_FOCUS_API_MASK 0x00001ff8u
#define HYDRA_GATE_C_SHIM_REGISTER_RAW_INPUT_DEVICES_BIT 0x00002000u
#define HYDRA_GATE_C_SHIM_GET_REGISTERED_RAW_INPUT_DEVICES_BIT 0x00004000u
#define HYDRA_GATE_C_SHIM_GET_RAW_INPUT_DATA_BIT 0x00008000u
#define HYDRA_GATE_C_SHIM_GET_RAW_INPUT_BUFFER_BIT 0x00010000u
#define HYDRA_GATE_C_SHIM_RAW_INPUT_API_MASK 0x0001e000u
#define HYDRA_GATE_C_SHIM_ALL_API_MASK 0x0001ffffu
#define HYDRA_GATE_C_SHIM_ENABLE_CURSOR_FOCUS 0x00000001u
#define HYDRA_GATE_C_SHIM_ENABLE_RAW_INPUT 0x00000002u
#define HYDRA_GATE_C_SHIM_CONFIG_FLAGS_MASK 0x00000003u
#define HYDRA_GATE_C_SHIM_MODULE_CURRENT_EXECUTABLE 1u

typedef enum HydraGateCShimResult {
    HYDRA_GATE_C_SHIM_OK = 0,
    HYDRA_GATE_C_SHIM_ALREADY_INSTALLED = 1,
    HYDRA_GATE_C_SHIM_INVALID_ARGUMENT = 2,
    HYDRA_GATE_C_SHIM_STRUCT_VERSION_MISMATCH = 3,
    HYDRA_GATE_C_SHIM_ADAPTER_VERSION_MISMATCH = 4,
    HYDRA_GATE_C_SHIM_IMPORT_NOT_FOUND = 5,
    HYDRA_GATE_C_SHIM_DUPLICATE_IMPORT = 6,
    HYDRA_GATE_C_SHIM_ALREADY_PATCHED = 7,
    HYDRA_GATE_C_SHIM_INVALID_IMAGE = 8,
    HYDRA_GATE_C_SHIM_PROTECTION_FAILURE = 9,
    HYDRA_GATE_C_SHIM_PATCH_FAILURE = 10,
    HYDRA_GATE_C_SHIM_ROLLBACK_FAILURE = 11,
    HYDRA_GATE_C_SHIM_ADAPTER_UNAVAILABLE = 12,
    HYDRA_GATE_C_SHIM_INTERNAL_ERROR = 13,
    HYDRA_GATE_C_SHIM_UNSUPPORTED_PLATFORM = 14
} HydraGateCShimResult;

typedef enum HydraGateCShimLifecycle {
    HYDRA_GATE_C_SHIM_INACTIVE = 0,
    HYDRA_GATE_C_SHIM_INSTALLING = 1,
    HYDRA_GATE_C_SHIM_ACTIVE = 2,
    HYDRA_GATE_C_SHIM_FAIL_CLOSED = 3,
    HYDRA_GATE_C_SHIM_UNINSTALLING = 4
} HydraGateCShimLifecycle;

#pragma pack(push, 1)

typedef struct HydraGateCShimConfigV1 {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t seat_id;
    uint32_t process_id;
    uint32_t flags;
    uint32_t reserved0;
} HydraGateCShimConfigV1;

typedef struct HydraGateCShimConfigV2 {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t seat_id;
    uint32_t process_id;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t target_window;
} HydraGateCShimConfigV2;

/*
 * P3-E-01 adds an explicit allowlisted API mask for real non-protected
 * application profiles. V1/V2 keep the original all-or-nothing controlled
 * probe contract; only the V3 entry point may request a strict subset.
 */
typedef struct HydraGateCShimConfigV3 {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t seat_id;
    uint32_t process_id;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t target_window;
    uint32_t required_api_mask;
    uint32_t reserved1;
} HydraGateCShimConfigV3;

typedef struct HydraGateCShimStatusV1 {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t lifecycle;
    uint32_t last_result;
    uint64_t generation;
    uint32_t system_error;
    uint32_t module_kind;
    uint32_t expected_api_mask;
    uint32_t discovered_api_mask;
    uint32_t patched_api_mask;
    uint32_t restored_api_mask;
    uint32_t hook_failure_count;
    uint32_t last_adapter_result;
    uint8_t adapter_available;
    uint8_t rollback_complete;
    uint16_t reserved0;
} HydraGateCShimStatusV1;

#pragma pack(pop)

HYDRA_GATE_C_SHIM_API uint32_t HYDRA_GATE_C_SHIM_CALL
hydra_gate_c_shim_api_version(void);

HYDRA_GATE_C_SHIM_API HydraGateCShimResult HYDRA_GATE_C_SHIM_CALL
hydra_gate_c_shim_install(
    HydraGateCAdapterHandle adapter,
    const HydraGateCShimConfigV2* config);

HYDRA_GATE_C_SHIM_API HydraGateCShimResult HYDRA_GATE_C_SHIM_CALL
hydra_gate_c_shim_install_v3(
    HydraGateCAdapterHandle adapter,
    const HydraGateCShimConfigV3* config);

HYDRA_GATE_C_SHIM_API HydraGateCShimResult HYDRA_GATE_C_SHIM_CALL
hydra_gate_c_shim_mark_adapter_unavailable(void);

HYDRA_GATE_C_SHIM_API HydraGateCShimResult HYDRA_GATE_C_SHIM_CALL
hydra_gate_c_shim_uninstall(void);

HYDRA_GATE_C_SHIM_API HydraGateCShimResult HYDRA_GATE_C_SHIM_CALL
hydra_gate_c_shim_get_status(HydraGateCShimStatusV1* status);

/*
 * Called only by the HydraSeat-owned probe after it applies one authenticated
 * Seat-local InputEvent to the adapter. It posts at most one synthetic
 * WM_INPUT and never performs pipe or file I/O.
 */
HYDRA_GATE_C_SHIM_API HydraGateCShimResult HYDRA_GATE_C_SHIM_CALL
hydra_gate_c_shim_dispatch_raw_input(void);

#ifdef __cplusplus
}
#endif
