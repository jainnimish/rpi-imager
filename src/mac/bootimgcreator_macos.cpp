/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025 Raspberry Pi Ltd
 */

#include "bootimgcreator.h"
#include <QProcess>
#include <QDebug>

bool BootImgCreator::attachDiskImage(const QString& imagePath, QString& device) {
    QProcess hdiutilAttach;
    hdiutilAttach.start("hdiutil", QStringList() << "attach" << "-nomount" << imagePath);
    if (!hdiutilAttach.waitForFinished(10000) || hdiutilAttach.exitCode() != 0) {
        qDebug() << "BootImgCreator (macOS): hdiutil attach failed";
        return false;
    }
    device = QString(hdiutilAttach.readAllStandardOutput()).trimmed();
    return true;
}

bool BootImgCreator::detachDiskImage(const QString& device) {
    QProcess::execute("hdiutil", QStringList() << "detach" << device);
    return true;
}

bool BootImgCreator::mountFilesystem(const QString& device, const QString& mountPoint) {
    QProcess mountProc;
    mountProc.start("mount", QStringList() << "-t" << "msdos" << device << mountPoint);
    if (!mountProc.waitForFinished(10000) || mountProc.exitCode() != 0) {
        qDebug() << "BootImgCreator (macOS): mount failed:" << mountProc.readAllStandardError();
        return false;
    }
    return true;
}
