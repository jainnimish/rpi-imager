/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025 Raspberry Pi Ltd
 */

#include "bootimgcreator.h"
#include <QProcess>
#include <QDebug>

namespace {
    bool attachDiskImage(const QString& imagePath, QString& device) {
        QProcess mdconfigCreate;
        mdconfigCreate.start("mdconfig", QStringList() << "-f" << imagePath);
        if (!mdconfigCreate.waitForFinished(10000) || mdconfigCreate.exitCode() != 0) {
            qDebug() << "BootImgCreator (FreeBSD): mdconfig failed";
            return false;
        }
        device = QString(mdconfigCreate.readAllStandardOutput()).trimmed();
        return true;
    }

    bool detachDiskImage(const QString& device) {
        QProcess::execute("mdconfig", QStringList() << "-d" << "-u" << device);
        return true;
    }

    bool mountFilesystem(const QString& device, const QString& mountPoint) {
        QProcess mountProc;
        mountProc.start("mount", QStringList() << "-t" << "msdosfs" << device << mountPoint);
        if (!mountProc.waitForFinished(10000) || mountProc.exitCode() != 0) {
            qDebug() << "BootImgCreator (FreeBSD): mount failed:" << mountProc.readAllStandardError();
            return false;
        }
        return true;
    }
}
