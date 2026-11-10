/* Inspect the actual filesystem and Foundation/MobileGestalt capacity answers. */
#import <Foundation/Foundation.h>
#include <sys/mount.h>
#include <sys/attr.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

extern void hakios_storage_capacity_init(void);

static void inspect(NSString *path)
{
    const char *p = path.fileSystemRepresentation;
    struct statfs sf;
    if (statfs(p, &sf) == 0) {
        printf("storage_info: %s fs=%s device=%s mount=%s block=%u total=%llu free=%llu available=%llu flags=%#x\n",
            p, sf.f_fstypename, sf.f_mntfromname, sf.f_mntonname, sf.f_bsize,
            (unsigned long long)sf.f_blocks * sf.f_bsize,
            (unsigned long long)sf.f_bfree * sf.f_bsize,
            (unsigned long long)sf.f_bavail * sf.f_bsize, sf.f_flags);
    } else printf("storage_info: statfs %s: %s\n", p, strerror(errno));
    struct attrlist attrs = { .bitmapcount = ATTR_BIT_MAP_COUNT,
        .volattr = ATTR_VOL_INFO | ATTR_VOL_SIZE | ATTR_VOL_SPACEFREE | ATTR_VOL_SPACEAVAIL };
    struct __attribute__((packed)) { uint32_t length; off_t total, free, available; } result;
    if (getattrlist(p, &attrs, &result, sizeof(result), 0) == 0)
        printf("storage_info: getattrlist %s total=%lld free=%lld available=%lld\n", p, result.total, result.free, result.available);
    else printf("storage_info: getattrlist %s: %s\n", p, strerror(errno));
    NSError *error = nil;
    NSDictionary *file = [[NSFileManager defaultManager] attributesOfFileSystemForPath:path error:&error];
    printf("storage_info: NSFileManager %s: %s error=%s\n", p, file.description.UTF8String, error.description.UTF8String);
    error = nil;
    NSDictionary *volume = [[NSURL fileURLWithPath:path] resourceValuesForKeys:@[
        NSURLVolumeTotalCapacityKey, NSURLVolumeAvailableCapacityKey,
        NSURLVolumeAvailableCapacityForImportantUsageKey,
        NSURLVolumeAvailableCapacityForOpportunisticUsageKey] error:&error];
    printf("storage_info: NSURL %s: %s error=%s\n", p, volume.description.UTF8String, error.description.UTF8String);
}

int main(void)
{
    hakios_storage_capacity_init();
    @autoreleasepool {
        for (NSString *path in @[@"/", @"/private/var", @"/var/mobile", @"/System/Volumes/Data"])
            inspect(path);
        void *h = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW);
        CFTypeRef (*copy)(CFStringRef) = h ? dlsym(h, "MGCopyAnswer") : NULL;
        if (copy) {
            CFTypeRef answer = copy(CFSTR("DiskUsage"));
            printf("storage_info: MG DiskUsage: %s\n", [(__bridge id)answer description].UTF8String);
            if (answer) CFRelease(answer);
        } else printf("storage_info: MobileGestalt: %s\n", dlerror());
    }
    return 0;
}
