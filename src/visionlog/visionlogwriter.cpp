/***************************************************************************
 *   Copyright 2018 Tobias Heineken                                        *
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

#include "visionlogwriter.h"
#include "visionlogheader.h"
#include "messagetype.h"

#include <fstream>
#include <cstring>
#include <QtEndian>
#include <QByteArray>

VisionLogWriter::VisionLogWriter(const QString& filename):
    QObject()
{
    //keep reference to filename bytes alive
    QByteArray filenameBytes = filename.toUtf8();
    const char *fname = filenameBytes.constData();
    out_stream = new std::ofstream(fname, std::ios_base::out | std::ios_base::binary);

    VisionLog::FileHeader fileHeader;
    fileHeader.version = 1;
    // length of struct fileHeader and char[12] fileHeader.name is known: write ... sizeof
    memcpy(fileHeader.name, VisionLog::DEFAULT_FILE_HEADER_NAME, std::min(sizeof(fileHeader.name), strlen(VisionLog::DEFAULT_FILE_HEADER_NAME)));
    // Log data is stored big endian, convert from host byte order
    fileHeader.version = qToBigEndian(fileHeader.version);

    out_stream->write((char*) &fileHeader, sizeof(fileHeader));
}

bool VisionLogWriter::isOpen() const
{
    return out_stream->is_open();
}

VisionLogWriter::~VisionLogWriter()
{
    delete out_stream;
}

void VisionLogWriter::addVisionPacket(const SSL_WrapperPacket& frame, qint64 time)
{
    if (!isOpen()) {
        return;
    }
    QByteArray data;
    data.resize(frame.ByteSize());
    if (!frame.IsInitialized()){
        qFatal("Writing an uninitialized detectionFrame to Vision log");
    }
    if (!frame.SerializeToArray(data.data(), data.size())){
        qFatal("Writing to array failed in %s (%s:%d)", __func__, __FILE__, __LINE__);
    }

    writePacket(data, time, VisionLog::MessageType::MESSAGE_SSL_VISION_2014);
}

void VisionLogWriter::addRefereePacket(const SSL_Referee& state, qint64 time)
{
    if (!isOpen()) {
        return;
    }
    QByteArray data;
    data.resize(state.ByteSize());
    if (!state.IsInitialized()){
        qFatal("Writing an uninitialized referee packet to Vision log");
    }
    if (!state.SerializeToArray(data.data(), data.size())){
        qFatal("Writing to array failed in %s (%s:%d)", __func__, __FILE__, __LINE__);
    }

    writePacket(data, time, VisionLog::MessageType::MESSAGE_SSL_REFBOX_2013);
}

void VisionLogWriter::writePacket(const QByteArray &data, qint64 time, VisionLog::MessageType type)
{
    VisionLog::DataHeader dataHeader;
    dataHeader.timestamp = time;
    dataHeader.messageType = type;
    dataHeader.messageSize = data.size();

    // Log data is stored big endian, convert from host byte order
    dataHeader.timestamp = qToBigEndian((qint64)dataHeader.timestamp);
    dataHeader.messageType = (VisionLog::MessageType) qToBigEndian((int32_t) dataHeader.messageType);
    dataHeader.messageSize = qToBigEndian(dataHeader.messageSize);

    out_stream->write((char*) &dataHeader, sizeof(dataHeader));

    out_stream->write(data.constData(), data.size());
}
