/**
 * Implements shimming SATA device to look like a SATA DOM (Disk-on-Module) device supported by the syno kernel
 * If you didn't read the docs for shim/boot_device_shim.c go there and read it first!
 *
 * HOW THE KERNEL ASSIGNS SYNOBOOT TYPE?
 * The determination of what is or isn't the correct synoboot device for SATA is made using vendor and model *names*, as
 * standard SCSI/SATA don't have any VID/PID designation like USB or PCI.
 * Syno kernel uses different vendor/model names depending on the platform. They are taken from the kernel config option
 * pairs CONFIG_SYNO_SATA_DOM_VENDOR/CONFIG_SYNO_SATA_DOM_MODEL and CONFIG_SYNO_SATA_DOM_VENDOR_SECOND_SRC/
 * CONFIG_SYNO_SATA_DOM_MODEL_SECOND_SRC. This gives the following supported matrix at the time of writing:
 *   - vendor-name="SATADOM"  and model-name="TYPE D 3SE" (purley only)
 *   - vendor-name="SATADOM-" and model-name="TYPE D 3SE" (all except purley)
 *   - vendor-name="SATADOM"  and model-name="3SE"        (purley only)
 *   - vendor-name="SATADOM"  and model-name="D150SH"     (all other)
 *
 * HOW THIS SHIM MATCHES DEVICE TO SHIM?
 * The decision is made based on "struct boot_media" (derived from boot config) passed to the register method. The
 * only criterion used is the physical size of the disk. The *first* device which is smaller or equal to
 * boot_media->dom_size_mib will be shimmed. If a consecutive device matching this rule appears a warning will be
 * triggered.
 * This sounds quite unusual. We considered multiple options before going that route:
 *   - Unlike USB we cannot easily match SATA devices using any stable identifier so any VID/PID was out of the window
 *   - S/N sounds like a good candidate unless you realize hypervisors use the same one for all disks
 *   - Vendor/Model names cannot be edited by the user and hypervisors ust the same one for all disks
 *   - Host/Port location can change (and good luck updating it in the boot params every time)
 *   - The only stable factor seems to be size
 *
 * HOW IT WORKS FOR HOT PLUGGED DEVICES?
 * While the USB boot shim depends on a race condition (albeit a pretty stable one) there's no way to use the same
 * method for SATA, despite both of them using SCSI under the hood. This is because true SCSI/SATA devices are directly
 * supported by the drivers/scsi/sd.c which generates no events before the type is determined. Because of this we
 * decided to exploit the dynamic nature of Linux drivers subsystem. All drivers register their buses with the kernel
 * and are automatically informed by the kernel if something appears on these buses (either during boot or via hot plug)
 *
 * This module simply asks the kernel drivers subsystem for the driver registered for "sd" (SCSI) devices. Then it
 * replaces its trigger function pointer. Normally it points to drivers/scsi/sd.c:sd_probe() which "probes" and configs
 * the device. Our sd_probe_shim() first reads the capacity and if criteria are met (see section above) it replaces
 * the vendor & model names and passes the control to the real sd_probe(). If nothing matches it transparently calls
 * the real sd_probe().
 *
 * If you're debugging you can test it without restarting the whole SD by removing and re-adding device. For example for
 * "sd 6:0:0:0: [sdg] 630784 512-byte logical blocks: (322 MB/308 MiB)" you should do:
 *    echo 1 > /sys/block/sdg/device/delete             # change SDG to the correct device
 *    echo "0 0 0" > /sys/class/scsi_host/host6/scan    # host 6 is the same as "sd 6:..." notation in dmesg
 * Warning: rescans and delete hard-yanks the device from controller so DO NOT do this on a disk with important data!
 *
 * HOW IT WORKS FOR EXISTING DEVICES?
 * Unfortunately, our sd_probe() replacement is still a bit of a race condition. However, this time we're racing with
 * SCSI driver loading which usually isn't a module. Because of this we need to expect some (probably all) devices to be
 * already probed. We need to do essentially what's described above (with /sys) but from kernel space.
 * To avoid any crashes and possible data loss we are never touching disks which aren't SATA and matching the size
 * match criterion. In other words this shim will NOT yank a data drive from the system.
 *
 * KNOWN LIMITATIONS
 * If you hot-unplug that SATA drive which is used for synoboot it will NOT be shimmed the next time you plug it without
 * rebooting. This is because we were lazy and didn't implement the removal shimming (as this behavior isn't defined
 * anyway with synoboot devices as they're not user-removable).
 *
 * This shim is only supported on kernels compiled with CONFIG_SYNO_BOOT_SATA_DOM enabled. Kernels built without that
 * option will never check for the vendor/model names and will never be considered SYNOBOOT.
 *
 * SOURCES
 *  - Synology's kernel GPL source -> drivers/scsi/sd.c, search for "gSynoBootSATADOM"
 *  - https://www.seagate.com/files/staticfiles/support/docs/manual/Interface%20manuals/100293068j.pdf
 */
#include "sata_boot_shim.h"
#include "../../common.h"
#include "../../config/runtime_config.h" //struct boot_device & consts, NATIVE_SATA_DOM_SUPPORTED

#ifdef NATIVE_SATA_DOM_SUPPORTED
#include "../../internal/call_protected.h" //scsi_scan_host_selected()
#include <linux/dma-direction.h> //DMA_FROM_DEVICE
#include <linux/unaligned/be_byteshift.h> //get_unaligned_be32()
#include <linux/delay.h> //msleep
#include <scsi/scsi.h> //cmd consts (e.g. SERVICE_ACTION_IN) and SCAN_WILD_CARD
#include <scsi/scsi_eh.h> //struct scsi_sense_hdr
#include <scsi/scsi_host.h> //struct Scsi_Host
#include <scsi/scsi_transport.h> //struct scsi_transport_template
#include <scsi/scsi_device.h> //scsi_execute_req()

#define SCSI_RC16_LEN 32 //originally defined in drivers/scsi/sd.c as RC16_LEN
#define SCSI_CMD_TIMEOUT (30 * HZ) //originally defined in drivers/scsi/sd.h as SD_TIMEOUT
#define SCSI_CMD_MAX_RETRIES 5 //normal drives shouldn't fail the command even once
#define SCSI_CAP_MAX_RETRIES 3
#define SCSI_BUF_SIZE 512 //originally defined in drivers/scsi/sd.h as SD_BUF_SIZE

//Old kernels used ambiguous constant: https://github.com/torvalds/linux/commit/eb846d9f147455e4e5e1863bfb5e31974bb69b7c
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,19,0)
#define SERVICE_ACTION_IN_16 SERVICE_ACTION_IN
#endif

/**
 * Issues SCSI "READ CAPACITY (16)" command
 * Make sure you read what this function returns!
 *
 * @param sdp
 * @param buffer Pointer to a buffer of size SCSI_BUF_SIZE
 * @param sshdr Sense header
 * @return 0 on command success, >0 if command failed; if the command failed it MAY be repeated
 */
static int scsi_read_cap16(struct scsi_device *sdp, unsigned char *buffer, struct scsi_sense_hdr *sshdr)
{
    unsigned char cmd[16];
    memset(cmd, 0, 16);
    cmd[0] = SERVICE_ACTION_IN_16;
    cmd[1] = SAI_READ_CAPACITY_16;
    cmd[13] = SCSI_RC16_LEN;
    memset(buffer, 0, SCSI_RC16_LEN);

    return scsi_execute_req(sdp, cmd, DMA_FROM_DEVICE, buffer, SCSI_RC16_LEN, sshdr, SCSI_CMD_TIMEOUT,
                            SCSI_CMD_MAX_RETRIES, NULL);
}

/**
 * Issues SCSI "READ CAPACITY (10)" command
 * Make sure you read what this function returns!
 *
 * @param sdp
 * @param buffer Pointer to a buffer of size SCSI_BUF_SIZE
 * @param sshdr Sense header
 * @return 0 on command success, >0 if command failed; if the command failed it MAY be repeated
 */
static int scsi_read_cap10(struct scsi_device *sdp, unsigned char *buffer, struct scsi_sense_hdr *sshdr)
{
    unsigned char cmd[16];
    cmd[0] = READ_CAPACITY;
    memset(&cmd[1], 0, 9);
    memset(buffer, 0, 8);

    return scsi_execute_req(sdp, cmd, DMA_FROM_DEVICE, buffer, 8, sshdr, SCSI_CMD_TIMEOUT, SCSI_CMD_MAX_RETRIES, NULL);
}

/**
 * Attempts to read capacity of a device assuming reasonably modern pathway
 *
 * This function (along with scsi_read_cap{10|16}) is loosely based on drivers/scsi/sd.c:sd_read_capacity(). However,
 * this method cuts some corners to be faster as we're expecting rather modern hardware. Additionally, functions from
 * sd.c cannot be used as they're static. Even that some of them can be called using kallsyms they aren't stateless and
 * will cause a KP later on (as they modify the device passed to them).
 * Thus this function should be seen as a way to quickly estimate (as it reports full mebibytes rounded down) the
 * capacity without causing side effects.
 *
 * @param sdp
 * @return capacity in full mebibytes, or -E on error
 */
static long long opportunistic_read_capacity(struct scsi_device *sdp)
{
    //some drives work only with the 16 version but older ones can only accept the older variant
    //to prevent false-positive "command failed" we need to try both
    bool use_cap16 = true;

    unsigned char *buffer = kmalloc(SCSI_BUF_SIZE, GFP_KERNEL);
    if (!buffer) {
        pr_loc_crt("kmalloc failure");
        return -EFAULT;
    }

    int out;
    int sense_valid = 0;
    struct scsi_sense_hdr sshdr;
    int read_retry = SCSI_CAP_MAX_RETRIES;
    do {
        //It can return 0 or a positive integer; 0 means immediate success where 1 means an error. Depending on the error
        //the command may be repeated.
        out = (use_cap16) ? scsi_read_cap16(sdp, buffer, &sshdr) : scsi_read_cap10(sdp, buffer, &sshdr);
        if (out == 0) {
            break; //command just succeeded
        }

        if (unlikely(out > 0)) { //it's technically an error but we may be able to recover
            if (!use_cap16) { //if we previously used CAP(16) and it failed we can try older CAP(10) [even on hard-fail]
                use_cap16 = false;
                continue;
            }

            //Some failures are hard-failure (e.g. drive doesn't support the cmd), some are soft-failures
            //In soft failures some are known to take more time (e.g. spinning rust is spinning up) and some should be
            //fast-repeat. We really only distinguish hard from soft and just wait some time for others
            //In a normal scenario this path will be cold as the drive will respond to CAP(16) or CAP(10) right away.

            sense_valid = scsi_sense_valid(&sshdr);
            if (!sense_valid) {
                pr_loc_dbg("Invalid sense - trying again");
                continue; //Sense invalid, this can be repeated right away
            }

            //Drive deliberately rejected the request and indicated that this situtation will not change
            if (sshdr.sense_key == ILLEGAL_REQUEST && (sshdr.asc == 0x20 || sshdr.asc == 0x24) && sshdr.ascq == 0x00) {
                pr_loc_err("Drive refused to provide capacity");
                return -EINVAL;
            }

            //Drive is busy - wait for some time
            if (sshdr.sense_key == UNIT_ATTENTION && sshdr.asc == 0x29 && sshdr.ascq == 0x00) {
                pr_loc_dbg("Drive busy during capacity pre-read (%d attempts left), trying again", read_retry-1);
                msleep(500); //if it's a spinning rust over USB we may need to wait
                continue;
            }
        }
    } while (--read_retry);

    if (out != 0) {
        pr_loc_err("Failed to pre-read capacity of the drive after %d attempts due to SCSI errors",
                   (SCSI_CAP_MAX_RETRIES - read_retry));
        kfree(buffer);
        return -EIO;
    }

    unsigned sector_size = get_unaligned_be32(&buffer[8]);
    unsigned long long lba = get_unaligned_be64(&buffer[0]);

    //Good up to 8192000000 pebibytes - good luck overflowing that :D
    long long size_mb = ((lba+1) * sector_size) / 1024 / 1024; //sectors * sector size = size in bytes

    kfree(buffer);
    return size_mb;
}

/**
 * Checks if a given generic device is a disk connected to a SATA port/host controller
 */
static bool inline is_sata_disk(struct device *dev)
{
    //from the kernel's pov SCSI devices include SCSI hosts, "leaf" devices, and others - this filters real SCSI devices
    if (!scsi_is_sdev_device(dev))
        return false;

    struct scsi_device *sdp = to_scsi_device(dev);

    //end/leaf devices can be disks or other things - filter only real disks
    //more than that use syno's private property (hey! not all of their kernel mods are bad ;)) to determine port which
    //a given device uses (vanilla kernel doesn't care about silly ports - SCSI is SCSI)
    if (sdp->type != TYPE_DISK || sdp->host->hostt->syno_port_type != SYNO_PORT_TYPE_SATA)
        return false;

    return true;
}

static unsigned long max_dom_size_mib = 0; //this will be set during register
bool device_mapped = false;
static bool inline is_shim_target(struct scsi_device *sdp)
{
    pr_loc_dbg("Probing SATA disk id=%u channel=%u vendor=\"%s\" model=\"%s\"", sdp->id, sdp->channel, sdp->vendor,
               sdp->model);

    long long capacity_mib = opportunistic_read_capacity(sdp);
    if (capacity_mib < 0) {
        pr_loc_dbg("Failed to estimate drive capacity - it WILL NOT be shimmed");
        return false;
    }

    if (capacity_mib > max_dom_size_mib) {
        pr_loc_dbg("Device has capacity of ~%llu MiB - it WILL NOT be shimmed (>%lu)", capacity_mib, max_dom_size_mib);
        return false;
    }

    if (unlikely(device_mapped)) {
        pr_loc_wrn("Boot device was already shimmed but a new matching device (~%llu MiB <= %lu) appeared again - "
                   "this may produce unpredictable outcomes! Ignoring - check your hardware", capacity_mib,
                   max_dom_size_mib);
        return false;
    }

    pr_loc_dbg("Device has capacity of ~%llu MiB - it is a shimmable target (<=%lu)", capacity_mib, max_dom_size_mib);

    return true;
}

static int (*org_sd_probe) (struct device *dev) = NULL; //set during register
static int sd_probe_shim(struct device *dev)
{
    if (!is_sata_disk(dev)) {
        pr_loc_dbg("%s: new SCSI device connected - it's not a SATA disk, ignoring", __FUNCTION__);
        goto proxy;
    }

    struct scsi_device *sdp = to_scsi_device(dev);
    if (is_shim_target(sdp)) {
        pr_loc_dbg("Shimming device with capacity of to vendor=\"%s\" model=\"%s\"", CONFIG_SYNO_SATA_DOM_VENDOR,
                   CONFIG_SYNO_SATA_DOM_MODEL);
        sdp->vendor = CONFIG_SYNO_SATA_DOM_VENDOR;
        sdp->model = CONFIG_SYNO_SATA_DOM_MODEL;
        device_mapped = true;
    }

    proxy:
    return org_sd_probe(dev);
}

//int bus_for_each_dev(struct bus_type *bus, struct device *start, void *data,
//		     int (*fn)(struct device *dev, void *data));
//bus_for_each_dev

struct scsi_disk_stub {
    struct scsi_driver *driver;
    struct scsi_device *device;
    struct device	dev;
    struct gendisk	*disk;
    atomic_t	openers;
    sector_t	capacity;
    u32		max_ws_blocks;
};

/**
 * Processes existing device and if it's a SATA drive which matches shim criteria it will be unplugged & replugged to be
 * shimmed
 *
 * @return 0 means "continue calling me" while any other value means "I found what I was looking for, stop calling me"
 */
static int on_existing_device(struct device *dev, void *data)
{
    if (!is_sata_disk(dev)) {
        pr_loc_dbg("Checking existing SCSI device \"%s\" - it's not a SATA disk, ignoring", dev_name(dev));
        return 0;
    }

    struct scsi_device *sdp = to_scsi_device(dev);

    //This will ask the device for its capacity again. This isn't done to save code space and consolidate new and old
    // devices into a common is_shim_target() path. The reason for this is even thou "struct scsi_disk" has the capacity
    // cached we cannot access it. When this module is built with full kernel sources the "struct scsi_disk" is a
    // clusterfuck of MY_ABC_HERE and since it resides in non-public header it is completely absent from toolkit builds.
    if (!is_shim_target(sdp)) {
        pr_loc_dbg("Device \"%s\" is not a shim target - ignoring", dev_name(dev));
        return 0;
    }

    pr_loc_inf("Device is \"%s\" vendor=\"%s\" model=\"%s\" is already connected - forcefully reconnecting to shim",
               dev_name(dev), sdp->vendor, sdp->model);

    struct Scsi_Host *host = sdp->host;
    pr_loc_dbg("Removing device from host%d", host->host_no);
    scsi_remove_device(sdp); //this will do locking for remove

    //See drivers/scsi/scsi_sysfs.c:scsi_scan() for details
    if (host->transportt->user_scan) {
        pr_loc_dbg("Triggering template-based rescan of host%d", host->host_no);
        host->transportt->user_scan(host, SCAN_WILD_CARD, SCAN_WILD_CARD, SCAN_WILD_CARD);
    } else {
        pr_loc_dbg("Triggering generic rescan of host%d", host->host_no);
        //this is unfortunately defined in scsi_scan.c, it can be emulated because it's just bunch of loops, but why?
        //This will also most likely never be used anyway
        _scsi_scan_host_selected(host, SCAN_WILD_CARD, SCAN_WILD_CARD, SCAN_WILD_CARD, 1);
    }

    //We're deliberately not returning non-zero (which will stop scanning) to detect situation where two shimmable devs
    //are matched and only the first one is used
    return 0;
}

/**
 * Scan existing device on the SD driver and try to determine if they're shimmable
 */
static void inline probe_existing_devices(struct device_driver *drv)
{
    int code = bus_for_each_dev(drv->bus, NULL, NULL, on_existing_device);
    pr_loc_dbg("bus_for_each_dev returned %d", code);
}

extern struct bus_type scsi_bus_type;
int register_sata_boot_shim(const struct boot_media *boot_dev_config)
{
    if (unlikely(boot_dev_config->type != BOOT_MEDIA_SATA)) {
        pr_loc_bug("%s doesn't support device type %d", __FUNCTION__, boot_dev_config->type);
        return -EINVAL;
    }

    if (unlikely(org_sd_probe)) {
        pr_loc_bug("SATA boot shim is already registered");
        return -EEXIST;
    }

    struct device_driver *drv = driver_find("sd", &scsi_bus_type);
    if (IS_ERR(drv)) {
        pr_loc_crt("Failed to get sd driver from kernel");
        return PTR_ERR(drv);
    }

    //Order of these sets is important as we don't acquire a lock
    max_dom_size_mib = boot_dev_config->dom_size_mib;
    org_sd_probe = drv->probe;
    drv->probe = sd_probe_shim;

    //Some devices may already be scanned (most likely all if SD was non-modular as it usually is) - we need to scan
    // them as well and if they match shimming criteria kick them out of the controller and re-probe so that they go
    // through sd_probe_shim(). Their capacity will be read twice but there's no really way around that (see the
    // comment in on_existing_device()).
    probe_existing_devices(drv);

    pr_loc_dbg("SATA boot shim registered");
    return 0;
}

int unregister_sata_boot_shim(void)
{
    if (unlikely(!org_sd_probe)) {
        pr_loc_bug("SATA boot shim is not registered");
        return -ENOENT;
    }

    struct device_driver *drv = driver_find("sd", &scsi_bus_type);
    if (IS_ERR(drv)) {
        pr_loc_crt("Failed to get sd driver from kernel");
        return PTR_ERR(drv);
    }

    //Order of these sets is important as we don't acquire a lock
    drv->probe = org_sd_probe;
    org_sd_probe = NULL;
    max_dom_size_mib = 0;
    //we are consciously NOT clearing device_mapped. It may be registered and we're not doing anything to unregister it

    pr_loc_dbg("SATA boot shim unregistered");
    return 0;
}
#else //ifdef NATIVE_SATA_DOM_SUPPORTED
int register_sata_boot_shim(const struct boot_media *boot_dev_config)
{
    pr_loc_err("SATA boot shim cannot be registered in a kernel built without SATA DoM support");
    return -ENODEV;
}

int unregister_sata_boot_shim(void)
{
    pr_loc_err("SATA boot shim cannot be unregistered in a kernel built without SATA DoM support");
    return -ENODEV;
}
#endif //ifdef else NATIVE_SATA_DOM_SUPPORTED