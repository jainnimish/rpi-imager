/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2020-2025 Raspberry Pi Ltd
 *
 * FreeBSD drive enumeration.
 *
 * Design notes:
 * - Uses sysctl, libCAM, and libGEOM to enumerate disks and query properties
 * - Individual functions are provided for determing specific drive properties
 * - Handles some edge cases: Loop devices, NVMe, SD/MMC, ram disks
 */

#include "drivelist.h"
#include "embedded_config.h"

#include <fcntl.h>
#include <sys/mount.h>
#include <camlib.h>
#include <libgeom.h>
#include <sys/sysctl.h>
#include <algorithm>
#include <array>
#include <unordered_set>
#include <vector>
#include <optional>
#include <cassert>
#include <QString>
#include <QStringList>

namespace Drivelist {

namespace {

/**
 * @brief Return the label associated with a GEOM device, if it exists.
 */
std::optional<QString> getLabel(const struct ggeom *dev) {
    QString label;
    struct gprovider *provider;
    struct ggeom *providerObj;

    LIST_FOREACH(provider, &dev->lg_provider, lg_provider) {
        providerObj = provider->lg_geom;
        if (QString(providerObj->lg_class->lg_name) == "LABEL") {
            return providerObj->lg_name;
        }
    }

    return std::nullopt;
}

/**
 * @brief Return the GEOM object representing a disk's partition table, if it exists.
 */
std::optional<const struct ggeom*> getDiskPartitionTable(const struct ggeom *disk) {
    struct gprovider *provider;

    // Search providers of a disk for a partition table
    LIST_FOREACH(provider, &disk->lg_provider, lg_provider) {
        if (QString(provider->lg_geom->lg_class->lg_name) == "PART") {
            return provider->lg_geom;
        }
    }

    return std::nullopt;
}

/**
 * @brief Return a list of GEOM objects representing the partitions in a table.
 */
std::vector<const struct ggeom*> getPartitionObjs(const struct ggeom *table) {
    std::vector<const struct ggeom*> result;

    struct gprovider *partition;

    // Enumerate geom objects representing partitions in the table
    LIST_FOREACH(partition, &table->lg_provider, lg_provider) {
        result.push_back(partition->lg_geom);
    }

    return result;
}

/**
 * @brief Return a list of partitions mounted on a disk and its partitions.
 */
QStringList getMountpointList(const struct ggeom *disk) {
    QStringList mountpoints;
    const auto partitionTable = getDiskPartitionTable(disk);
    if (!partitionTable) {
        return mountpoints;
    }

    std::vector<const struct ggeom*> partitionObjs = getPartitionObjs(*partitionTable);
    std::unordered_set<QString> partitions;

    for (const auto& obj : partitionObjs) {
        partitions.insert(QString(obj->lg_name));
    }

    struct statfs *mntlist;
    int nmnt = getmntinfo(&mntlist, MNT_WAIT);

    for (int i = 0; i < nmnt; i++) {
        QString from = mntlist[i].f_mntfromname;
        if (partitions.contains(from)) {
            QString on = mntlist[i].f_mntonname;
            mountpoints.append(on);
        }
    }

    return mountpoints;
}

/**
 * @brief Return a list of GEOM labels associated with a disk and its partitions.
 */
QStringList getMountpointLabelList(const struct ggeom *disk) {

    QStringList labels;
    const auto partitionTable = getDiskPartitionTable(disk);
    if (!partitionTable) {
        return labels;
    }

    std::vector<const struct ggeom*> partitionObjs = getPartitionObjs(*partitionTable);

    for (const auto& obj : partitionObjs) {
        auto label = getLabel(obj);
        if (label) {
            labels.append(*label);
        }
    }

    return labels;
}

/**
 * @brief Return the type (DISK, PART, etc.) of a GEOM device.
 */
QString getDeviceType(const struct ggeom *device) {
    const struct gconsumer *consumer;
    const struct ggeom *consumerObj;

    assert(QString(device->lg_class->lg_name) == "DEV");

    consumer = LIST_FIRST(&device->lg_consumer);
    consumerObj = consumer->lg_geom;

    return QString(consumerObj->lg_class->lg_name);
}

/**
 * @brief Return the value associated with some key in the config of a GEOM.
 */
std::optional<QString> getGeomConfig(const struct ggeom *gp, const QString& key) {
    struct gconfig *confItem;

    // Search geom object config for a field matching item
    LIST_FOREACH(confItem, &gp->lg_config, lg_config) {
        if (QString(confItem->lg_name) == key) {
            return QString(confItem->lg_val);
        }
    }

    return std::nullopt;
}

/**
 * @brief Query CAM to determine if a GEOM disk is removable.
 */
bool isRemovable(const struct ggeom *gp) {

    assert(QString(gp->lg_class->lg_name) == "DISK");

    const char *name = gp->lg_name;
    bool removable;

    struct cam_device *dev = cam_open_device(name, O_RDWR);
    if (dev == nullptr) {
        return false;
    }
    removable = SID_IS_REMOVABLE(&dev->inq_data);
    cam_close_device(dev);

    return removable;
}

/**
 * @brief Query sysctl to determine if a GEOM disk is write-protected.
 */
bool isReadOnly(const struct ggeom *gp) {
    const char *name = gp->lg_name;
    const QString oid = QString::asprintf("kern.geom.disk.%s.flags", name);

    char oldp[1024];
    size_t len = 1024;
    if (sysctlbyname(oid.toStdString().c_str(), oldp, &len, nullptr, 0) == -1) {
        return false;
    }

    QString rawFlagList = QString(oldp);
    // The string is of form be<flag1,flag2,...>. We wish to remove "^be<' and
    // ">$". Then, we may iterate over its tokens.
    rawFlagList.slice(3, rawFlagList.length() - 4);

    QStringList flags = rawFlagList.split(u',');

    return std::any_of(flags.cbegin(), flags.cend(),
                       [](const QString& i) { return i == "WRITEPROTECT"; });
}

/**
 * @brief Determine if a GEOM disk represents a ramdisk rather than a physical drive.
 */
bool isVirtual(const struct ggeom *gp) {
    const struct gconsumer *consumer = LIST_FIRST(&gp->lg_consumer);
    const struct ggeom *consumerObj = consumer->lg_geom;
    return QString(consumerObj->lg_class->lg_name) == "MD";
}

/**
 * @brief Determine if a system folder is mounted on a given GEOM disk.
 */
bool isBackingSystemPath(const struct ggeom *gp) {
    std::unordered_set<QString> systemPaths = {"/", "/usr", "/var", "/home", "/boot"};
    for (const auto& mountpoint : getMountpointList(gp)) {
        if (systemPaths.contains(mountpoint)) {
            return true;
        }
    }

    return false;
}

/**
 * @brief Lookup through CAM the interface a disk uses.
 */
cam_xport getTransportType(const struct ggeom *disk) {
    cam_xport transport = XPORT_UNKNOWN;
    union ccb *ccb = nullptr;
    struct cam_device *device = cam_open_device(disk->lg_name, O_RDWR);
    if (device == nullptr) {
        goto fail;
    }
    ccb = cam_getccb(device);
    if (ccb == nullptr) {
        goto fail;
    }

    ccb->ccb_h.func_code = XPT_GET_TRAN_SETTINGS;
    ccb->cts.type = CTS_TYPE_CURRENT_SETTINGS;
    if (cam_send_ccb(device, ccb) < 0
        || ((ccb->ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP)) {
        goto fail;
    }

    transport = ccb->cts.transport;

fail:
    if (device) {
        cam_close_device(device);
    }

    if (ccb) {
        cam_freeccb(ccb);
    }

    return transport;
}

/**
 * @brief Determines if a disk uses MMC, SD, or SDIO as its interface.
 */
bool isMMCSD(const struct ggeom *disk) {
    return getTransportType(disk) == XPORT_MMCSD;
}

/**
 * @brief Determines if a disk is a USB device.
 */
bool isUSB(const struct ggeom *disk) {
    return getTransportType(disk) == XPORT_USB;
}

/**
 * @brief Determines if a disk operates on the SCSI bus.
 */
bool isSCSI(const struct ggeom *disk) {
    std::array<cam_xport, 10> scsiTransports{
        XPORT_SPI,      /* SCSI Parallel Interface */
        XPORT_FC,       /* Fiber Channel */
        XPORT_SSA,      /* Serial Storage Architecture */
        XPORT_USB,      /* Universal Serial Bus */
        XPORT_ATA,      /* AT Attachment */
        XPORT_SAS,      /* Serial Attached SCSI */
        XPORT_SATA,     /* Serial AT Attachment */
        XPORT_ISCSI,    /* iSCSI */
        XPORT_SRP,      /* SCSI RDMA Protocol */
        XPORT_UFSHCI    /* Universal Flash Storage Host Interface */
    };

    cam_xport transport = getTransportType(disk);

    return std::any_of(scsiTransports.cbegin(), scsiTransports.cend(),
                  [&transport](const cam_xport& i) { return i == transport; });
}

/**
 * @brief Return the interface/bus type of a disk as a string.
 */
std::string getBusType(const struct ggeom *disk) {
    cam_xport transport = getTransportType(disk);

    switch (transport) {
    case XPORT_UNSPECIFIED:
        return "unspecified";
    case XPORT_SPI:      /* SCSI Parallel Interface */
    case XPORT_FC:       /* Fiber Channel */
    case XPORT_SSA:      /* Serial Storage Architecture */
    case XPORT_SAS:      /* Serial Attached SCSI */
    case XPORT_ISCSI:    /* iSCSI */
    case XPORT_SRP:      /* SCSI RDMA Protocol */
    case XPORT_UFSHCI:   /* Universal Flash Storage Host Interface */
        return "scsi";
    case XPORT_ATA:      /* AT Attachment */
    case XPORT_SATA:     /* Serial AT Attachment */
        return "atapi";
    case XPORT_USB:      /* Universal Serial Bus */
        return "usb";
    case XPORT_PPB:      /* Parallel Port Bus */
        return "ppb";
    case XPORT_NVME:     /* NVMe over PCIe */
    case XPORT_NVMF:     /* NVMe over Fabrics */
        return "nvme";
    case XPORT_MMCSD:    /* MMC, SD, SDIO card */
        return "mmcsd";
    case XPORT_UNKNOWN:
    default:
        return "unknown";
    }
}

/**
 * @brief Provide a description including Vendor, Model, labels, and whether it is MMCSD or loopback.
 */
std::string getDescription(const struct ggeom *gp) {
    QStringList descParts;
    descParts.reserve(4);

    // Auxiliary function for cleaning and appending to QStringList
    const auto addIfNotEmpty = [](QStringList& list, const QString& s) {
        QString trimmed = s.trimmed();
        if (!trimmed.isEmpty()) {
            list.append(trimmed);
        }
    };

    const auto description = getGeomConfig(gp, "descr");
    if (description) {
        addIfNotEmpty(descParts, *description);
    }

    const auto label = getLabel(gp);
    if (label) {
        addIfNotEmpty(descParts, *label);
    }

    // Iterate over partitions and add labels to the description in the format
    // "(label1, label2, ...)"
    const auto partitionTable = getDiskPartitionTable(gp);
    if (partitionTable) {
        std::vector<const struct ggeom*> partitionObjs = getPartitionObjs(*partitionTable);
        QStringList labels;
        for (const auto& obj : partitionObjs) {
            auto label = getLabel(obj);
            if (label) {
                addIfNotEmpty(labels, *label);
            }
        }
        if (!labels.isEmpty()) {
            descParts.append("(" + labels.join(", ") + ")");
        }
    }

    if (isMMCSD(gp)) {
        descParts.append("Internal SD card reader");
    }

    std::string name = gp->lg_name;
    if (name.starts_with("lo")) {
        descParts.append("Loopback Device");
    }

    return sanitizeForDisplay(descParts.join(" ").toStdString());
}

/**
 * @brief Return the interface/bus type of a disk as a string.
 */
bool isUndesiredDeviceType(const struct ggeom *disk) {
    struct cam_device *device = cam_open_device(disk->lg_name, O_RDWR);
    if (device == nullptr) {
        return true;
    }

    std::array<uint8_t, 2> opticalTypes{T_OPTICAL, T_CDROM};
    uint8_t deviceType = SID_TYPE(&device->inq_data);

    bool undesirable = std::any_of(opticalTypes.cbegin(), opticalTypes.cend(),
                                   [&deviceType](const uint8_t i) { return i == deviceType; });

    cam_close_device(device);
    return undesirable;
}

/**
 * @brief Generate a DeviceDescriptor from a reference to the GEOM object of a disk.
 */
std::optional<DeviceDescriptor> parseDiskDevice(const struct ggeom *disk)
{
    DeviceDescriptor device;

    if (isUndesiredDeviceType(disk)) {
        return std::nullopt;
    }

    const QString name = disk->lg_name;
    device.device = name.toStdString();
    device.raw = name.toStdString();

    device.busType = getBusType(disk);

    device.isCard = isMMCSD(disk);
    device.isUSB = isUSB(disk);
    device.isSCSI = !device.isUSB && isSCSI(disk);
    device.isVirtual = isVirtual(disk);
    device.isReadOnly = isReadOnly(disk);
    device.isRemovable = isRemovable(disk);
    device.isSystem = device.isVirtual ? isBackingSystemPath(disk) : !device.isRemovable;

    for (const QString& mountpoint : getMountpointList(disk)) {
        device.mountpoints.push_back(mountpoint.toStdString());
    }

    for (const QString& mountpointLabel : getMountpointLabelList(disk)) {
        device.mountpointLabels.push_back(mountpointLabel.toStdString());
    }

    device.size = static_cast<uint64_t>(LIST_FIRST(&disk->lg_provider)->lg_mediasize);
    device.blockSize = static_cast<uint32_t>(LIST_FIRST(&disk->lg_provider)->lg_sectorsize);
    device.logicalBlockSize = device.blockSize; // TODO: LOG-SEC

    device.description = getDescription(disk);

    return device;
}

} // Anonymous namespace

// ============================================================================
// Public API
// ============================================================================

std::vector<DeviceDescriptor> ListStorageDevices()
{
    std::vector<DeviceDescriptor> deviceList;

    struct gmesh devtree;
    struct gclass *geom_class;
    struct ggeom *obj;

    geom_gettree(&devtree);

    LIST_FOREACH(geom_class, &devtree.lg_class, lg_class) {
        if (QString(geom_class->lg_name) != "DISK") {
            continue;
        }

        LIST_FOREACH(obj, &geom_class->lg_geom, lg_geom) {
            auto device = parseDiskDevice(obj);
            if (device) {
                deviceList.push_back(std::move(*device));
            }
        }
    }

    geom_deletetree(&devtree);

    return deviceList;
}

} // namespace Drivelist
