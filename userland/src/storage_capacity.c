/* HFS has real free-space counters but no APFS purgeable-space accounting.
 * iOS 16.5 CFURL reports zero for its two policy-capacity keys on this volume.
 * Preserve the normal answer; for that HFS-only zero case, provide the actual
 * allocatable space as a conservative capacity (without claiming purgeable bytes).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <sys/mount.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void install_capacity_methods(void);
void hakios_storage_capacity_init(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, install_capacity_methods);
}

static CFNumberRef
hfs_capacity(CFURLRef url, CFStringRef key, CFTypeRef original)
{
    if (!original || CFGetTypeID(original) != CFNumberGetTypeID()) return NULL;
    int64_t reported = 0;
    if (!CFNumberGetValue(original, kCFNumberSInt64Type, &reported) || reported != 0) return NULL;
    unsigned bit;
    if (CFEqual(key, kCFURLVolumeAvailableCapacityForImportantUsageKey)) bit = 1;
    else if (CFEqual(key, kCFURLVolumeAvailableCapacityForOpportunisticUsageKey)) bit = 2;
    else return NULL;
    char path[PATH_MAX];
    struct statfs sf;
    if (!CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, sizeof(path)) ||
        statfs(path, &sf) != 0 || strcmp(sf.f_fstypename, "hfs") != 0 ||
        (sf.f_flags & MNT_RDONLY) || sf.f_bsize == 0 || sf.f_bavail == 0 ||
        sf.f_bavail > INT64_MAX / sf.f_bsize) return NULL;
    int64_t available = (int64_t)(sf.f_bavail * sf.f_bsize);
    CFNumberRef result = CFNumberCreate(NULL, kCFNumberSInt64Type, &available);
    if (result) {
        static unsigned logged;
        if (!(__sync_fetch_and_or(&logged, bit) & bit)) {
            int fd = open("/dev/console", O_WRONLY);
            if (fd >= 0) {
                dprintf(fd, "hakios: capacity: HFS %s: %s capacity from statfs = %lld bytes\n",
                    path, bit == 1 ? "important" : "opportunistic", (long long)available);
                close(fd);
            }
        }
    }
    return result;
}

static Boolean
capacity_property(CFURLRef url, CFStringRef key, void *value, CFErrorRef *error)
{
    Boolean ok = CFURLCopyResourcePropertyForKey(url, key, value, error);
    int saved_errno = errno;
    if (ok && value) {
        CFTypeRef *out = value;
        CFNumberRef corrected = hfs_capacity(url, key, *out);
        if (corrected) { CFRelease(*out); *out = corrected; }
    }
    errno = saved_errno;
    return ok;
}

static CFMutableDictionaryRef
copy_capacity_overrides(CFURLRef url, CFDictionaryRef original)
{
    CFMutableDictionaryRef corrected = NULL;
    CFStringRef wanted[] = { kCFURLVolumeAvailableCapacityForImportantUsageKey,
        kCFURLVolumeAvailableCapacityForOpportunisticUsageKey };
    if (original) {
        for (unsigned i = 0; i < sizeof(wanted)/sizeof(wanted[0]); i++) {
            CFTypeRef value = CFDictionaryGetValue(original, wanted[i]);
            CFNumberRef space = hfs_capacity(url, wanted[i], value);
            if (space) {
                if (!corrected) corrected = CFDictionaryCreateMutableCopy(NULL, 0, original);
                if (corrected) CFDictionarySetValue(corrected, wanted[i], space);
                CFRelease(space);
            }
        }
    }
    return corrected;
}

static CFDictionaryRef
capacity_properties(CFURLRef url, CFArrayRef keys, CFErrorRef *error)
{
    CFDictionaryRef original = CFURLCopyResourcePropertiesForKeys(url, keys, error);
    int saved_errno = errno;
    CFMutableDictionaryRef corrected = copy_capacity_overrides(url, original);
    if (corrected) CFRelease(original);
    errno = saved_errno;
    return corrected ? corrected : original;
}

/* NSURL calls CFURL directly within CoreFoundation, so its method boundary
 * must be covered too. getResourceValue returns an autoreleased object; unlike
 * the CF Copy entry points above, it does not transfer ownership to the caller. */
static BOOL (*original_get)(id, SEL, id *, id, id *);
static id (*original_values)(id, SEL, id, id *);

static id autorelease_cf(CFTypeRef object)
{
    return ((id (*)(id, SEL))objc_msgSend)((id)object, sel_registerName("autorelease"));
}

static BOOL capacity_get(id self, SEL cmd, id *value, id key, id *error)
{
    BOOL ok = original_get(self, cmd, value, key, error);
    int saved_errno = errno;
    if (ok && value) {
        CFNumberRef corrected = hfs_capacity((CFURLRef)self, (CFStringRef)key, (CFTypeRef)*value);
        if (corrected) *value = autorelease_cf(corrected);
    }
    errno = saved_errno;
    return ok;
}

static id capacity_values(id self, SEL cmd, id keys, id *error)
{
    id original = original_values(self, cmd, keys, error);
    int saved_errno = errno;
    CFMutableDictionaryRef corrected = copy_capacity_overrides((CFURLRef)self, (CFDictionaryRef)original);
    errno = saved_errno;
    return corrected ? autorelease_cf(corrected) : original;
}

static void install_capacity_methods(void)
{
    Class cls = objc_getClass("NSURL");
    Method get = cls ? class_getInstanceMethod(cls, sel_registerName("getResourceValue:forKey:error:")) : NULL;
    Method values = cls ? class_getInstanceMethod(cls, sel_registerName("resourceValuesForKeys:error:")) : NULL;
    if (get && method_getNumberOfArguments(get) == 5) {
        original_get = (void *)method_getImplementation(get);
        method_setImplementation(get, (IMP)capacity_get);
    }
    if (values && method_getNumberOfArguments(values) == 4) {
        original_values = (void *)method_getImplementation(values);
        method_setImplementation(values, (IMP)capacity_values);
    }
}

__attribute__((used)) static const struct { const void *replacement, *original; }
capacity_interpose[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)capacity_property, (const void *)CFURLCopyResourcePropertyForKey },
    { (const void *)capacity_properties, (const void *)CFURLCopyResourcePropertiesForKeys }
};
