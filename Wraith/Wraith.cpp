//
// Wraith.cpp
// Wraith
//
// A minimal Lilu kernel extension that forces kern.hv_vmm_present to
// return a fixed value regardless of the real hardware state.
// Configuration is a single boot-arg / NVRAM variable:
//
//   wrtvmm=on    →  always report "is a VM"     (returns 1)
//   wrtvmm=off   →  always report "not a VM"    (returns 0)
//   wrtvmm=none  →  pass through, no patching   (default when unset)
//
// Named after the Wraith — present but unseen.
//
// Based on sysctl OID walking technique from RestrictEvents by acidanthera.
// Original copyright © 2020 vit9696. BSD-3-Clause licence.
// Modifications copyright © 2026 David Parsons.
//

#include <stddef.h>

// Forward-declare the sysctl OID types we need.
// We do NOT include <sys/sysctl.h> or <sys/kern_sysctl.h> because both
// conflict with Lilu / MacKernelSDK headers in a kext build environment.
// These declarations match XNU bsd/sys/sysctl.h exactly.
struct sysctl_req;

struct sysctl_oid;

struct sysctl_oid_list {
    struct sysctl_oid *slh_first;
};

struct sysctl_oid {
    struct sysctl_oid_list *oid_parent;
    struct sysctl_oid      *oid_link;
    int                     oid_number;
    unsigned int            oid_kind;
    void                   *oid_arg1;
    int                     oid_arg2;
    const char             *oid_name;
    int                   (*oid_handler)(struct sysctl_oid *oidp,
                                         void *arg1, int arg2,
                                         struct sysctl_req *req);
    const char             *oid_fmt;
    const char             *oid_descr;
    int                     oid_version;
    int                     oid_refcnt;
};

#define SYSCTL_CHILDREN(oidp)     (reinterpret_cast<sysctl_oid_list *>((oidp)->oid_arg1))

#define SYSCTL_OUT(r, p, l) sysctl_handle_opaque(r, p, l)
extern "C" int sysctl_handle_opaque(struct sysctl_req *, void *, size_t);

#include <IOKit/IOService.h>
#include <Headers/kern_api.hpp>
#include <Headers/kern_nvram.hpp>
#include <Headers/kern_efi.hpp>
#include <Headers/plugin_start.hpp>

// ──────────────────────────────────────────────────────────────────────────────
// Boot argument identifiers
// ──────────────────────────────────────────────────────────────────────────────

static const char *bootargOff[]   { "-wrtoff"  };
static const char *bootargDebug[] { "-wrtdbg"  };
static const char *bootargBeta[]  { "-wrtbeta" };

// ──────────────────────────────────────────────────────────────────────────────
// State
// ──────────────────────────────────────────────────────────────────────────────

// The fixed value we will return from the sysctl.  Set during plugin init.
static int wrtForcedValue = -1;   // -1 = not configured, do nothing

// Saved pointer to the original handler so we can restore if needed.
static int (*orgHvVmmHandler)(struct sysctl_oid *, void *, int,
                               struct sysctl_req *) = nullptr;

// ──────────────────────────────────────────────────────────────────────────────
// Replacement sysctl handler
//
// Ignores the real kernel value entirely and returns wrtForcedValue.
// ──────────────────────────────────────────────────────────────────────────────

static int wrtHvVmmHandler(struct sysctl_oid *oidp,
                            void              *arg1,
                            int                arg2,
                            struct sysctl_req *req)
{
    DBGLOG("wrt", "kern.hv_vmm_present → returning forced value %d", wrtForcedValue);
    return SYSCTL_OUT(req, &wrtForcedValue, sizeof(wrtForcedValue));
}

// ──────────────────────────────────────────────────────────────────────────────
// NVRAM helper
// ──────────────────────────────────────────────────────────────────────────────

static bool readNvramVariable(const char      *fullName,
                               const char16_t *unicodeName,
                               const EFI_GUID *guid,
                               void           *dst,
                               size_t          max)
{
    NVStorage storage;
    if (storage.init()) {
        uint32_t size = 0;
        auto buf = storage.read(fullName, size, NVStorage::OptRaw);
        if (buf) {
            if (size <= max)
                memcpy(dst, buf, size);
            Buffer::deleter(buf);
        }
        storage.deinit();
        return buf && size <= max;
    }

    auto rt = EfiRuntimeServices::get(true);
    if (rt) {
        uint64_t size  = max;
        uint32_t attr  = 0;
        auto status = rt->getVariable(unicodeName, guid, &attr, &size, dst);
        rt->put();
        return status == EFI_SUCCESS && size <= max;
    }

    return false;
}

// ──────────────────────────────────────────────────────────────────────────────
// Parse wrtvmm value from boot-args or NVRAM.
//
// wrtvmm=on    →  force VMM present   (return 1)
// wrtvmm=off   →  force VMM absent    (return 0)
// wrtvmm=none  →  no patching         (return -1)
//
// Returns -1 if the variable is absent, set to "none", or unrecognised.
// ──────────────────────────────────────────────────────────────────────────────

static int parseWrtVmm()
{
    char value[8] {};   // longest valid string is "none" (4 chars) + NUL

    // Boot-arg takes priority.
    if (PE_parse_boot_argn("wrtvmm", value, sizeof(value))) {
        DBGLOG("wrt", "read wrtvmm=%s from boot-args", value);
    }
    // Fall back to NVRAM.
    else if (readNvramVariable(NVRAM_PREFIX(LILU_VENDOR_GUID, "wrtvmm"),
                               u"wrtvmm",
                               &EfiRuntimeServices::LiluVendorGuid,
                               value, sizeof(value))) {
        value[sizeof(value) - 1] = '\0';
        DBGLOG("wrt", "read wrtvmm=%s from NVRAM", value);
    }
    else {
        DBGLOG("wrt", "wrtvmm not set — Wraith will not patch");
        return -1;
    }

    if (strcmp(value, "on") == 0)   return 1;
    if (strcmp(value, "off") == 0)  return 0;
    if (strcmp(value, "none") == 0) return -1;

    SYSLOG("wrt", "wrtvmm value \"%s\" is invalid (use on / off / none), ignoring", value);
    return -1;
}

// ──────────────────────────────────────────────────────────────────────────────
// Walk the sysctl OID tree and reroute kern.hv_vmm_present
// ──────────────────────────────────────────────────────────────────────────────

static void rerouteHvVmm(KernelPatcher &patcher)
{
    // Resolve the root sysctl children list.
    mach_vm_address_t addr =
        patcher.solveSymbol(KernelPatcher::KernelID, "__sysctl__children");

    if (!addr) {
        SYSLOG("wrt", "failed to resolve __sysctl__children");
        patcher.clearError();
        return;
    }

    auto *root = reinterpret_cast<sysctl_oid_list *>(addr);

    // Find the "kern" node in the top-level list.
    sysctl_oid *kernOid = nullptr;
    for (auto *o = root->slh_first; o; o = o->oid_link) {
        if (o->oid_name && strcmp(o->oid_name, "kern") == 0) {
            kernOid = o;
            break;
        }
    }

    if (!kernOid) {
        SYSLOG("wrt", "failed to find 'kern' sysctl node");
        return;
    }

    // Find "hv_vmm_present" inside kern.
    sysctl_oid *vmmOid = nullptr;
    auto *children = SYSCTL_CHILDREN(kernOid);
    if (children) {
        for (auto *o = children->slh_first; o; o = o->oid_link) {
            if (o->oid_name && strcmp(o->oid_name, "hv_vmm_present") == 0) {
                vmmOid = o;
                break;
            }
        }
    }

    if (!vmmOid) {
        SYSLOG("wrt", "kern.hv_vmm_present not found — kernel may not expose VMM status");
        return;
    }

    // Save the original and swap in our handler.
    orgHvVmmHandler    = vmmOid->oid_handler;
    vmmOid->oid_handler = wrtHvVmmHandler;

    if (vmmOid->oid_handler == wrtHvVmmHandler)
        DBGLOG("wrt", "kern.hv_vmm_present rerouted — will return %d", wrtForcedValue);
    else
        SYSLOG("wrt", "handler swap may have failed");
}

// ──────────────────────────────────────────────────────────────────────────────
// Plugin entry point
// ──────────────────────────────────────────────────────────────────────────────

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    bootargOff,   arrsize(bootargOff),
    bootargDebug, arrsize(bootargDebug),
    bootargBeta,  arrsize(bootargBeta),
    KernelVersion::BigSur,
    KernelVersion::Tahoe,
    []() {
        DBGLOG("wrt", "Wraith loaded");

        wrtForcedValue = parseWrtVmm();

        if (wrtForcedValue == -1) {
            DBGLOG("wrt", "no wrtvmm value set — nothing to do");
            return;
        }

        // kern.hv_vmm_present exists from Big Sur 11.3 (minor ≥ 4) onward.
        bool supported = (getKernelVersion() >= KernelVersion::Monterey) ||
                         (getKernelVersion() == KernelVersion::BigSur &&
                          getKernelMinorVersion() >= 4);

        if (!supported) {
            SYSLOG("wrt", "kernel too old for kern.hv_vmm_present, skipping");
            return;
        }

        lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
            rerouteHvVmm(patcher);
        });
    }
};
