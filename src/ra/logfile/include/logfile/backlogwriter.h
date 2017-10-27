/***************************************************************************
 *   Copyright 2017 Andreas Wendler                                        *
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

#ifndef BACKLOGWRITER_H
#define BACKLOGWRITER_H

#include "protobuf/status.h"
#include "statussource.h"
#include <QContiguousCache>
#include <QObject>

class QString;
class QByteArray;

class BacklogStatusSource : public StatusSource
{
    Q_OBJECT
public:
    BacklogStatusSource(QContiguousCache<QByteArray> &backlog, QContiguousCache<qint64> &timings);
    bool isOpen() const override { return true; }

    const QList<qint64>& timings() const override { return m_timings; }
    // equals timings().size()
    int packetCount() const override { return m_timings.size(); }
    Status readStatus(int packet) override;

public slots:
    void readPackets(int startPacket, int count) override;

private:
    QContiguousCache<QByteArray> m_packets;
    QList<qint64> m_timings;
};


class BacklogWriter : public QObject
{
    Q_OBJECT
public:
    BacklogWriter(unsigned seconds);
    BacklogStatusSource * makeStatusSource();

signals:
    void enableBacklogSave(bool enabled);
    void clearData();

private slots:
    // these slots must be called in the same thread
    void clear();
    void handleStatus(const Status &status);
    void saveBacklog(QString filename, Status teamStatus);

private:
    Status packetFromByteArray(QByteArray packetData);

private:
    QContiguousCache<QByteArray> m_packets;
    QContiguousCache<qint64> m_timings;

    // approximately, with both strategys running
    const int BACKLOG_SIZE_PER_SECOND = 570;
};

#endif // BACKLOGWRITER_H
