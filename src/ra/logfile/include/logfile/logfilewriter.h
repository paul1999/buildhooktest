/***************************************************************************
 *   Copyright 2015 Michael Eischer                                        *
 *   Robotics Erlangen e.V.                                                *
 *   http://www.robotics-erlangen.de/                                      *
 *   info@robotics-erlangen.de                                             *
 *                                                                         *
 *   This program is free software: you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation, either version 3 of the License, or     *
 *   any later version.                                                    *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifndef LOGFILEWRITER_H
#define LOGFILEWRITER_H

#include "protobuf/status.h"
#include "logfilehasher.h"
#include <QObject>
#include <QString>
#include <QDataStream>
#include <QFile>
#include <QList>

class QMutex;
class LogFileReader;

class LogFileWriter : public QObject
{
    Q_OBJECT
public:
    explicit LogFileWriter();
    ~LogFileWriter() override;
    LogFileWriter(const LogFileWriter &) = delete;
    LogFileWriter& operator=(const LogFileWriter &) = delete;

    bool open(const QString &filename);
    void close();
    bool isOpen() const { return m_file.isOpen(); }

    QString filename() const { return m_file.fileName(); }
    LogFileReader * makeStatusSource();

public slots:
    bool writeStatus(const Status &status);

private:
    void writePackageEntry(qint64 time, QByteArray &&data);
    void addFirstPackage(qint64 time, QByteArray &&data);

    mutable QMutex *m_mutex;
    QFile m_file;
    QDataStream m_stream;
    QByteArray m_packageBuffer;
    int m_packageBufferCount;
    QList<qint64> m_timeStamps;
    QList<qint64> m_packetOffsets;
    qint64 m_writtenPackages;
    LogFileHasher m_hasher;
    enum class HashingState {
        UNINITIALIZED, NEEDS_HASHING, HAS_HASHING
    };
    HashingState m_hashState = HashingState::UNINITIALIZED;
    Status m_hashStatus = Status(new amun::Status);

    const static qint32 GROUPED_PACKAGES = 100;
    static_assert(GROUPED_PACKAGES >= LogFileHasher::HASHED_PACKAGES);
    static_assert(LogFileHasher::HASHED_PACKAGES > 2);

    qint32 m_packageBufferOffsets[GROUPED_PACKAGES];
    qint64 m_packageTimeStamps[LogFileHasher::HASHED_PACKAGES-2]; //Only to be used while HashingState::NEEDS_HASHING
};

#endif // LOGFILEWRITER_H
