#include <acpi.h>
#include <kernel.h>
#include <drivers/acpi.h>
#include <pci.h>

//#define ACPI_DEBUG
#ifdef ACPI_DEBUG
#define acpi_debug(x, ...) rprintf("ACPI: " x "\n", ##__VA_ARGS__)
#else
#define acpi_debug(x, ...)
#endif

#define acpi_heap   heap_locked(get_kernel_heaps())

boolean acpi_walk_madt(madt_handler mh)
{
    ACPI_TABLE_HEADER *madt;
    ACPI_STATUS rv = AcpiGetTable(ACPI_SIG_MADT, 1, &madt);
    if (ACPI_FAILURE(rv))
        return false;
    u8 *p = (u8 *)madt + sizeof(ACPI_TABLE_MADT);
    u8 *pe = (u8 *)madt + madt->Length;
    for (; p < pe; p += p[1])
        apply(mh, p[0], p);
    AcpiPutTable(madt);
    return true;
}

void init_acpi_tables(kernel_heaps kh)
{
    assert(ACPI_SUCCESS(AcpiInitializeSubsystem()));
    ACPI_STATUS rv = AcpiInitializeTables(NULL, 0, true);
    if (ACPI_FAILURE(rv)) {
        acpi_debug("AcpiInitializeTables returned %d", rv);
        return;
    }
    rv = AcpiLoadTables();
    if (ACPI_FAILURE(rv))
        acpi_debug("AcpiLoadTables returned %d", rv);
}

static UINT32 acpi_shutdown(void *context)
{
    acpi_debug("shutdown");
    kernel_shutdown(0);
    return ACPI_INTERRUPT_HANDLED;
}

closure_function(3, 1, void, acpi_powerdown,
                 ACPI_TABLE_FADT *, fadt, u16, pm1a_slp_typ, u16, pm1b_slp_typ,
                 int, status)
{
    acpi_debug("powerdown");
    ACPI_TABLE_FADT *fadt = bound(fadt);
    AcpiOsWritePort(fadt->Pm1aControlBlock,
                    ACPI_PM1_SLP_EN | ACPI_PM1_SLP_TYP(bound(pm1a_slp_typ)), 16);
    AcpiOsWritePort(fadt->Pm1bControlBlock,
                    ACPI_PM1_SLP_EN | ACPI_PM1_SLP_TYP(bound(pm1b_slp_typ)), 16);
}

static void acpi_powerdown_init(kernel_heaps kh)
{
    ACPI_TABLE_HEADER *fadt;
    ACPI_STATUS rv = AcpiGetTable(ACPI_SIG_FADT, 1, &fadt);
    if (ACPI_FAILURE(rv)) {
        acpi_debug("cannot find FADT: %d", rv);
        return;
    }

    /* Retrieve SLP_TYP register values to be used when powering down the system. */
    ACPI_BUFFER retb = {
        .Length = ACPI_ALLOCATE_BUFFER,
    };
    rv = AcpiEvaluateObjectTyped(NULL, "\\_S5", NULL, &retb, ACPI_TYPE_PACKAGE);
    if (ACPI_FAILURE(rv)) {
        acpi_debug("failed to get _S5 object (%d)", rv);
        return;
    }
    ACPI_OBJECT *obj = retb.Pointer;
    if ((obj->Package.Count >= 2) && (obj->Package.Elements[0].Type == ACPI_TYPE_INTEGER) &&
        (obj->Package.Elements[1].Type == ACPI_TYPE_INTEGER)) {
        vm_halt = closure(heap_general(kh), acpi_powerdown, (ACPI_TABLE_FADT *)fadt,
                          obj->Package.Elements[0].Integer.Value,
                          obj->Package.Elements[1].Integer.Value);
        assert(vm_halt != INVALID_ADDRESS);
    } else {
        acpi_debug("unexpected _S5 object value (%d elements)", obj->Package.Count);
        AcpiPutTable(fadt);
    }
    AcpiOsFree(obj);
}

void init_acpi(kernel_heaps kh)
{
    ACPI_STATUS rv = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
    if (ACPI_FAILURE(rv)) {
        acpi_debug("AcpiEnableSubsystem returned %d", rv);
        return;
    }
    rv = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
    if (ACPI_FAILURE(rv)) {
        acpi_debug("AcpiInitializeObjects returned %d", rv);
        return;
    }
    acpi_powerdown_init(kh);
    rv = AcpiInstallFixedEventHandler(ACPI_EVENT_POWER_BUTTON, acpi_shutdown, 0);
    if (ACPI_FAILURE(rv))
        acpi_debug("cannot install power button hander: %d", rv);
}

/* OS services layer */

ACPI_STATUS AcpiOsInitialize(void)
{
    return AE_OK;
}

void *AcpiOsAllocate(ACPI_SIZE size)
{
    void *p = allocate(acpi_heap, size);
    return (p != INVALID_ADDRESS) ? p : 0;
}

void AcpiOsFree(void *memory)
{
    deallocate(acpi_heap, memory, -1ull);
}

void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS where, ACPI_SIZE length)
{
    acpi_debug("%s(0x%lx, %ld)", __func__, where, length);
    u64 page_offset = where & PAGEMASK;
    length += page_offset;
    heap vh = (heap)heap_virtual_page(get_kernel_heaps());
    void *v = allocate(vh, length);
    if (v == INVALID_ADDRESS)
        return 0;
    acpi_debug("  mapping 0x%lx(%ld) to 0x%lx", v, length, where & ~PAGEMASK);
    map(u64_from_pointer(v), where & ~PAGEMASK, length, pageflags_writable(pageflags_memory()));
    return v + page_offset;
}

void AcpiOsUnmapMemory(void *where, ACPI_SIZE length)
{
    acpi_debug("%s(0x%lx, %ld)", __func__, where, length);
    u64 page_offset = u64_from_pointer(where) & PAGEMASK;
    where -= page_offset;
    length = pad(length + page_offset, PAGESIZE);
    acpi_debug("  unmapping 0x%lx(%ld)", where, length);
    unmap(u64_from_pointer(where), length);
    heap vh = (heap)heap_virtual_page(get_kernel_heaps());
    deallocate(vh, where, length);
}

ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS address, UINT64 *value, UINT32 width)
{
    void *v = AcpiOsMapMemory(address, sizeof(UINT64));
    if (!v)
        return AE_NO_MEMORY;
    switch (width) {
    case 8:
        *value = *(u8 *)v;
        break;
    case 16:
        *value = *(u16 *)v;
        break;
    case 32:
        *value = *(u32 *)v;
        break;
    case 64:
        *value = *(u64 *)v;
        break;
    default:
        return AE_BAD_PARAMETER;
    }
    AcpiOsUnmapMemory(v, sizeof(UINT64));
    return AE_OK;
}

ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS address, UINT64 value, UINT32 width)
{
    void *v = AcpiOsMapMemory(address, sizeof(UINT64));
    if (!v)
        return AE_NO_MEMORY;
    switch (width) {
    case 8:
        *(u8 *)v = value;
        break;
    case 16:
        *(u16 *)v = value;
        break;
    case 32:
        *(u32 *)v = value;
        break;
    case 64:
        *(u64 *)v = value;
        break;
    default:
        return AE_BAD_PARAMETER;
    }
    AcpiOsUnmapMemory(v, sizeof(UINT64));
    return AE_OK;
}

ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *pci_id, UINT32 reg, UINT64 *value, UINT32 width)
{
    struct pci_dev dev = {
            .bus = pci_id->Bus,
            .slot = pci_id->Device,
            .function = pci_id->Function,
    };
    *value = pci_cfgread(&dev, reg, width / 8);
    return AE_OK;
}

ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *pci_id, UINT32 reg, UINT64 value, UINT32 width)
{
    struct pci_dev dev = {
            .bus = pci_id->Bus,
            .slot = pci_id->Device,
            .function = pci_id->Function,
    };
    pci_cfgwrite(&dev, reg, width / 8, value);
    return AE_OK;
}

UINT64 AcpiOsGetTimer(void)
{
    if (!platform_monotonic_now)    /* platform clock not available yet */
        return 0;
    timestamp t = now(CLOCK_ID_MONOTONIC);
    return nsec_from_timestamp(t) / 100;    /* return time in 100-nanosecond units */
}

void AcpiOsStall(UINT32 usecs)
{
    kernel_delay(microseconds(usecs));
}

void AcpiOsSleep(UINT64 msecs)
{
    halt("%s not supported\n", __func__);
}

ACPI_STATUS AcpiOsSignal(UINT32 function, void *info)
{
    return AE_NOT_IMPLEMENTED;
}

void AcpiOsPrintf(const char *fmt, ...)
{
    /* We can't use the printf-style functions of the kernel because they treat the %X format
     * identifier differently from the standard printf-style functions. Just print the format
     * string, it may still be helpful if any error messages are generated. */
    rputs(fmt);
}

ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *existing_table, ACPI_TABLE_HEADER **new_table)
{
    *new_table = 0;
    return AE_OK;
}

ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *existing_table,
                                        ACPI_PHYSICAL_ADDRESS *new_address,
                                        UINT32 *new_table_length)
{
    *new_address = 0;
    return AE_OK;
}

ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *init_val, ACPI_STRING *new_val)
{
    *new_val = 0;
    return AE_OK;
}

ACPI_STATUS AcpiOsCreateCache(char *cache_name, UINT16 object_size, UINT16 max_depth,
                              ACPI_CACHE_T **return_cache)
{
    heap h = allocate_objcache(acpi_heap, (heap)heap_linear_backed(get_kernel_heaps()), object_size,
                               PAGESIZE);
    if (h == INVALID_ADDRESS)
        return AE_NO_MEMORY;
    *return_cache = (ACPI_CACHE_T *)h;
    return AE_OK;
}

void *AcpiOsAcquireObject(ACPI_CACHE_T *cache)
{
    heap h = (heap)cache;
    void *obj = allocate_zero(h, h->pagesize);
    return (obj != INVALID_ADDRESS) ? obj : 0;
}

ACPI_STATUS AcpiOsReleaseObject(ACPI_CACHE_T *cache, void *object)
{
    heap h = (heap)cache;
    deallocate(h, object, h->pagesize);
    return AE_OK;
}

ACPI_STATUS AcpiOsPurgeCache(ACPI_CACHE_T *cache)
{
    /* not implemented */
    return AE_OK;
}

ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *out_handle)
{
    spinlock l = allocate(acpi_heap, sizeof(*l));
    if (l == INVALID_ADDRESS)
        return AE_NO_MEMORY;
    spin_lock_init(l);
    *out_handle = l;
    return AE_OK;
}

ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK handle)
{
    return spin_lock_irq(handle);
}

void AcpiOsReleaseLock(ACPI_SPINLOCK handle, ACPI_CPU_FLAGS flags)
{
    spin_unlock_irq(handle, flags);
}

void AcpiOsDeleteLock(ACPI_SPINLOCK handle)
{
    deallocate(acpi_heap, handle, sizeof(struct spinlock));
}

ACPI_STATUS AcpiOsCreateSemaphore(UINT32 max_units, UINT32 initial_units, ACPI_HANDLE *out_handle)
{
    ACPI_STATUS rv;
    if (max_units != 1)
        return AE_NOT_IMPLEMENTED;
    rv = AcpiOsCreateLock(out_handle);
    if (rv != AE_OK)
        return rv;
    if (initial_units == 0)
        spin_lock(*out_handle);
    return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore(ACPI_HANDLE handle, UINT32 units, UINT16 msec_timeout)
{
    if (units == 1) {
        spin_lock(handle);
        return AE_OK;
    }
    return AE_NOT_IMPLEMENTED;
}

ACPI_STATUS AcpiOsSignalSemaphore(ACPI_HANDLE handle, UINT32 units)
{
    if (units == 1) {
        spin_unlock(handle);
        return AE_OK;
    }
    return AE_NOT_IMPLEMENTED;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_HANDLE handle)
{
    AcpiOsDeleteLock(handle);
    return AE_OK;
}

closure_function(2, 0, void, acpi_async_func,
                 ACPI_OSD_EXEC_CALLBACK, function, void *, context)
{
    acpi_debug("async %p", bound(function));
    bound(function)(bound(context));
    closure_finish();
}

ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE type, ACPI_OSD_EXEC_CALLBACK function, void *context)
{
    acpi_debug("execute %p", function);
    thunk t = closure(acpi_heap, acpi_async_func, function, context);
    if (t == INVALID_ADDRESS)
        return AE_NO_MEMORY;
    if (!enqueue_irqsafe(runqueue, t)) {
        deallocate_closure(t);
        return AE_NO_MEMORY;
    }
    return AE_OK;
}

ACPI_THREAD_ID AcpiOsGetThreadId(void)
{
    return 1;   /* dummy value */
}