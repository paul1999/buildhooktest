/***************************************************************************
 *   Copyright 2020 Michael Eischer, Andreas Wendler                       *
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

#include "combinedlogwriter.h"
#include "backlogwriter.h"
#include "logfilewriter.h"
#include "statussource.h"

#include <QThread>
#include <QDateTime>
#include <QSettings>
#include <QCoreApplication>

namespace CombinedLogWriterInternal {
    class SignalSource: public QObject {
        Q_OBJECT

    public:
        SignalSource(QObject* parent = nullptr) : QObject(parent) {}

    signals:
        void saveBacklogFile(QString filename, const Status &status, bool processEvents);
        void gotStatusForRecording(const Status &status);
        void gotStatusForBacklog(const Status &status);

    public:
        void emitSaveBacklog(QString filename, const Status &status, bool processEvents);
        void emitStatusToRecording(const Status &status);
        void emitStatusToBacklog(const Status &status);
    };

    void SignalSource::emitSaveBacklog(QString filename, const Status &status, bool b) {
        emit saveBacklogFile(filename, status,b);
    }

    void SignalSource::emitStatusToRecording(const Status &status) {
        emit gotStatusForRecording(status);
    }

    void SignalSource::emitStatusToBacklog(const Status & status) {
        emit gotStatusForBacklog(status);
    }
}

using CombinedLogWriterInternal::SignalSource;

CombinedLogWriter::CombinedLogWriter(bool replay, int backlogLength) :
    m_logState(LogState::BACKLOG),
    m_isReplay(replay),
    m_logFile(NULL),
    m_logFileThread(NULL),
    m_lastTime(0),
    m_isLoggingEnabled(true),
    m_signalSource(new SignalSource(this))
{
    // start backlog writer thread
    m_backlogThread = new QThread();
    m_backlogThread->setObjectName("Seshat Backlog");
    m_backlogThread->start();
    m_backlogWriter = new BacklogWriter(backlogLength);
    m_backlogWriter->moveToThread(m_backlogThread);

    connect(m_backlogWriter, SIGNAL(enableBacklogSave(bool)), this, SLOT(enableLogging(bool)));
    connect(m_signalSource, SIGNAL(gotStatusForBacklog(Status)), m_backlogWriter, SLOT(handleStatus(Status)));
    connect(m_signalSource, SIGNAL(saveBacklogFile(QString,Status,bool)), m_backlogWriter, SLOT(saveBacklog(QString,Status,bool)));
    connect(this, SIGNAL(resetBacklog()), m_backlogWriter, SLOT(clear()));
}

CombinedLogWriter::~CombinedLogWriter()
{
    if (m_logFileThread) {
        m_logFileThread->quit();
        m_logFileThread->wait();
        delete m_logFileThread;
    }
    delete m_logFile;
    m_backlogThread->quit();
    m_backlogThread->wait();
    delete m_backlogThread;
    delete m_backlogWriter;
}

void CombinedLogWriter::sendBacklogStatus(int lastNPackets)
{
    if (m_logState != LogState::BACKLOG) {
        return;
//        return QList<Status>();
    }
    // source is located in another thread, but when no signals/slots are used this is fine
    std::shared_ptr<StatusSource> source = m_backlogWriter->makeStatusSource();
    QList<Status> packets;
    packets.reserve(source->packetCount());
    for (int i = std::max(0, source->packetCount() - lastNPackets);i<source->packetCount();i++) {
        packets.append(source->readStatus(i));
    }
    // copy the response afterwards in one large UI-Response:
    Status s = Status::createArena();
    amun::UiResponse* response = s->mutable_pure_ui_response();
    {
        int i = 0;
        for (const auto& status: packets) {
            response->add_logger_status()->CopyFrom(*status);
            if (++i % 10 == 0) {
                QCoreApplication::processEvents();
            }
        }
    }
    emit sendStatus(s);
    return;
}

void CombinedLogWriter::handleCommand(Command comm) {
    if (!comm->has_record()) {
        return;
    }
    const amun::CommandRecord& recordCommand = comm->record();
    if (recordCommand.has_use_logfile_location()) {
        useLogfileLocation(recordCommand.use_logfile_location());
    }
    if (recordCommand.has_run_logging() && recordCommand.for_replay() == m_isReplay) {
        recordButtonToggled(recordCommand.run_logging());
    }
    if (recordCommand.has_save_backlog() && recordCommand.for_replay() == m_isReplay) {
        saveBackLog();
    }
    if (recordCommand.has_request_backlog() && recordCommand.for_replay() == m_isReplay) {
        sendBacklogStatus(recordCommand.request_backlog());
    }
}

std::shared_ptr<StatusSource> CombinedLogWriter::makeStatusSource()
{
    if (m_logState == LogState::LOGGING) {
        return m_logFile->makeStatusSource();
    } else { // While PENDING we use the (soon to be outdated) backlog source as m_logFile will still be empty
        return m_backlogWriter->makeStatusSource();
    }
}

void CombinedLogWriter::handleStatus(Status status)
{
    if (!status->has_time()) {
        status->set_time(m_lastTime);
    }

    // keep team configurations for the logfile
    if (status->has_team_yellow()) {
        m_yellowTeam.CopyFrom(status->team_yellow());
    }
    if (status->has_team_blue()) {
        m_blueTeam.CopyFrom(status->team_blue());
    }

    // keep team names for the logfile
    if (status->has_game_state()) {
        const amun::GameState &state = status->game_state();
        const SSL_Referee_TeamInfo &teamBlue = state.blue();
        m_blueTeamName = QString::fromStdString(teamBlue.name());

        const SSL_Referee_TeamInfo &teamYellow = state.yellow();
        m_yellowTeamName = QString::fromStdString(teamYellow.name());
    }


    if (m_isLoggingEnabled && m_logState == LogState::PENDING) {
        startLogfile();
    }

    // If we didn't tell the UI because we didn't know what time is it, we have to send this information here.
    if (m_lastTime == 0 && m_logState == LogState::LOGGING) {
        Status s = Status::createArena();
        s->set_time(status->time());
        amun::UiResponse* response = s->mutable_pure_ui_response();
        response->set_is_logging(true);
        emit sendStatus(s);
    }

    m_lastTime = status->time();

    if (status->has_pure_ui_response()) {
        return;
    }

    if (m_isLoggingEnabled && m_logState == LogState::LOGGING) {
        m_signalSource->emitStatusToRecording(status);
    }
    if (m_logState == LogState::BACKLOG) {
        m_signalSource->emitStatusToBacklog(status);
    }
}

void CombinedLogWriter::enableLogging(bool enable)
{
    if (!enable) {
        if (m_isLoggingEnabled) {
            recordButtonToggled(false);
        }
    }
    m_isLoggingEnabled = enable;
    Status s = Status::createArena();
    s->set_time(m_lastTime);
    amun::UiResponse* response = s->mutable_pure_ui_response();
    response->set_enable_logging(enable);
    emit sendStatus(s);
}

void CombinedLogWriter::saveBackLog()
{
    const QString filename = createLogFilename();

    Status status(new amun::Status);
    status->mutable_team_yellow()->CopyFrom(m_yellowTeam);
    status->mutable_team_blue()->CopyFrom(m_blueTeam);

    m_signalSource->emitSaveBacklog(filename, status, true);
}

QString CombinedLogWriter::dateTimeToString(const QDateTime & dt)
{
    const int utcOffset = dt.secsTo(QDateTime(dt.date(), dt.time(), Qt::UTC));

    int sign = utcOffset >= 0 ? 1: -1;
    const QString date = dt.toString(Qt::ISODate) + QString::fromLatin1("%1%2%3")
            .arg(sign == 1 ? QLatin1Char('+') : QLatin1Char('-'))
            .arg(utcOffset * sign / (60 * 60), 2, 10, QLatin1Char('0'))
            .arg((utcOffset / 60) % 60, 2, 10, QLatin1Char('0'));
    return date;
}

void CombinedLogWriter::useLogfileLocation(bool enabled)
{
    m_useSettingLocation = enabled;
}


QString CombinedLogWriter::createLogFilename() const
{
    QSettings s;
    s.beginGroup("LogLocation");
    QString path(".");
    if (m_useSettingLocation) {
        int size = s.beginReadArray("locations");
        if (size > 0) {
            s.setArrayIndex(0);
            path = s.value("path").toString();
        }
        s.endArray();
        s.endGroup();
    }
    QString teamnames;
    if (!m_yellowTeamName.isEmpty() && !m_blueTeamName.isEmpty()) {
        teamnames = QString("%1 vs %2").arg(m_yellowTeamName).arg(m_blueTeamName);
    } else if (!m_yellowTeamName.isEmpty()) {
        teamnames = m_yellowTeamName;
    } else  if (!m_blueTeamName.isEmpty()) {
        teamnames = m_blueTeamName;
    }

    const QString date = dateTimeToString(QDateTime::currentDateTime()).replace(":", "");
    if (m_isReplay) {
        return path+"/"+QString("replay%1.log").arg(date);
    } else {
        return path+"/"+QString("%1%2.log").arg(date).arg(teamnames);
    }
}

Status CombinedLogWriter::getTeamStatus()
{
    Status status(new amun::Status);
    status->set_time(m_lastTime);
    status->mutable_team_yellow()->CopyFrom(m_yellowTeam);
    status->mutable_team_blue()->CopyFrom(m_blueTeam);
    return status;
}

void CombinedLogWriter::startLogfile()
{
    m_logFile->writeStatus(getTeamStatus());
    m_logState = LogState::LOGGING;
}

void CombinedLogWriter::recordButtonToggled(bool enabled)
{
    if (enabled) {
        Q_ASSERT(!m_logFile);
        emit resetBacklog();

        const QString filename = createLogFilename();

        // create log file and forward status
        m_logFile = new LogFileWriter();
        if (!m_logFile->open(filename)) {
            delete m_logFile;
            m_logFile = nullptr;
            // show in the ui that the recording failed by informing it that the recording finished.
            Status s = Status::createArena();
            s->set_time(m_lastTime);
            amun::UiResponse* response = s->mutable_pure_ui_response();
            response->set_is_logging(false);
            emit sendStatus(s);
            return;
        }
        connect(m_signalSource, SIGNAL(gotStatusForRecording(Status)), m_logFile, SLOT(writeStatus(Status)));

        // create thread if not done yet and move to seperate thread
        if (m_logFileThread == NULL) {
            m_logFileThread = new QThread();
            m_logFileThread->start();
        }
        m_logFile->moveToThread(m_logFileThread);
        m_logState = LogState::PENDING;
    } else {
        // defer log file deletion to happen in its thread
        if (m_logFile != nullptr) {
            m_logFile->deleteLater();
            m_logFile = nullptr;
        }
        m_logState = LogState::BACKLOG;
    }
    if (m_lastTime != 0) {
        // Just tell the UI that we already started to log.
        // In Horus mode, this disables the ability to move in the logfile.
        // This is done to increase reponsiveness for the user.
        // The user doesn't need to know that the file is not yet being created.
        // The rare case where a user (most likely via loglog) produces a log without any status
        // and wonders why he cannot open the log was considered but deemed not important
        // and the responsiveness of the UI was considered more useful.
        Status s = Status::createArena();
        s->set_time(m_lastTime);
        amun::UiResponse* response = s->mutable_pure_ui_response();
        response->set_is_logging(enabled);
        emit sendStatus(s);
    }
}

#include "combinedlogwriter.moc"
