/* BEGIN_COMMON_COPYRIGHT_HEADER
 * (c)LGPL2+
 *
 * Flacon - audio File Encoder
 * https://github.com/flacon/flacon
 *
 * Copyright: 2022
 *   Alexander Sokoloff <sokoloff.a@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * END_COMMON_COPYRIGHT_HEADER */

#include "extprogram.h"
#include <QDebug>
#include "../types.h"
#include <QLoggingCategory>

namespace {
Q_LOGGING_CATEGORY(LOG, "ExtProgram")
}

ExtProgram::ExtProgram(QObject *parent) :
    QProcess(parent)
{
    connect(this, &QProcess::errorOccurred, this, &ExtProgram::handleError);
}

void ExtProgram::handleError(QProcess::ProcessError error)
{
    qCWarning(LOG) << "ERROR";
    qCWarning(LOG) << QString("%1: The '%2' program crashes").arg(objectName()).arg(program());
    qCWarning(LOG) << "Program with args:" << debugProgramArgs(program(), arguments());
    qCWarning(LOG) << "Error:" << error;
    qCWarning(LOG) << "Error string:" << errorString();
    if (isOpen()) {
        qCWarning(LOG) << "Stderr: ............";
        qCWarning(LOG) << readAllStandardError();
        qCWarning(LOG) << "....................";
    }

    QString msg = QString("The '%1' program crashes with an error: %2").arg(program()).arg(errorString());
    throw FlaconError(msg);
}
