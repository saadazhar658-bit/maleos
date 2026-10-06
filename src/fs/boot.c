#include "fs/fs_boot.h"

#include "fs/tar.h"
#include "fs/vfs.h"
#include "kernel/printk.h"
#include "kernel/string.h"
#include "mm/mm.h"

void fs_boot_init(const struct boot_info *bi)
{
    for (size_t i = 0; i < bi->module_count; i++) {
        const struct boot_module *m = &bi->modules[i];
        if (strcmp(m->name, "initrd") != 0)
            continue;
        int n = tar_unpack((const void *)(HHDM_BASE + m->start), (size_t)(m->end - m->start), "/");
        if (n < 0)
            printk("VFS: initrd is corrupt (error %d)\n", n);
        else
            printk("VFS: initrd unpacked, %d entries (%lu bytes)\n", n,
                   (unsigned long)(m->end - m->start));
        break;
    }

    vfs_mkdir_p("/tmp");
    vfs_mkdir_p("/mnt");
    vfs_mkdir_p("/mnt2");

    static const struct {
        const char *dev;
        const char *target;
    } disks[] = {{"hda", "/mnt"}, {"sda", "/mnt2"}};
    for (size_t i = 0; i < sizeof(disks) / sizeof(disks[0]); i++) {
        if (!blk_find(disks[i].dev))
            continue;
        int rc = vfs_mount(disks[i].dev, disks[i].target, "ext2");
        if (rc == 0)
            printk("VFS: mounted %s on %s (ext2, read-only)\n", disks[i].dev, disks[i].target);
        else
            printk("VFS: %s is not mountable as ext2 (error %d)\n", disks[i].dev, rc);
    }
    vfs_dump_mounts();
}
