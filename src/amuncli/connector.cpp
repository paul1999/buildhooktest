/***************************************************************************
 *   Copyright 2016 Michael Eischer                                        *
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

#include "connector.h"
#include <QCoreApplication>
#include <QDir>
#include <QRegExp>
#include <iostream>
#include <google/protobuf/text_format.h>

Connector::Connector(QObject *parent) :
    QObject(parent),
    m_asBlue(false),
    m_debug(false),
    m_exitCode(255)
{
}

Connector::~Connector()
{
}

void Connector::setInitScript(const QString &initScript)
{
    QDir currentDirectory(".");
    m_initScript = currentDirectory.absoluteFilePath(initScript);
}

void Connector::setEntryPoint(const QString &entryPoint)
{
    m_entryPoint = entryPoint;
}

void Connector::setStrategyColor(bool asBlue)
{
    m_asBlue = asBlue;
}

void Connector::setDebug(bool debug)
{
    m_debug = debug;
}

void Connector::addStrategyLoad(amun::CommandStrategy *strategy)
{
    strategy->set_enable_debug(true);
    auto *load = strategy->mutable_load();
    load->set_filename(m_initScript.toStdString());
    if (!m_entryPoint.isNull()) {
        load->set_entry_point(m_entryPoint.toStdString());
    }
}

void Connector::start()
{
    // FIXME: send load robots
    // FIXME: send config
    // FIXME: send referee?

    Command command(new amun::Command);
    command->mutable_simulator()->set_enable(true);
    command->mutable_referee()->set_active(true);

    auto *strategy = (m_asBlue) ? command->mutable_strategy_blue() : command->mutable_strategy_yellow();
    addStrategyLoad(strategy);
    sendCommand(command);
}

void Connector::dumpEntrypoints(const amun::StatusStrategy &strategy)
{
    for (const auto& entrypoint: strategy.entry_point()) {
        std::cout << "Entrypoint: '" << entrypoint << "'" << std::endl;
    }
}

void Connector::handleStrategyStatus(const amun::StatusStrategy &strategy)
{
    for (const std::string &stdOption: strategy.option()) {
        m_options.insert({stdOption, false});
    }
    if (strategy.state() == amun::StatusStrategy::FAILED) {
        const auto& it = std::find_if_not(m_options.begin(), m_options.end(), [](const std::pair<std::string, bool>& p){ return p.second; });
        if (m_exitCode != 0 || it == m_options.end()) {
            qApp->exit(m_exitCode);
        } else {
            std::cout << "Rerunning with " << it->first <<" set to true"<< std::endl;
            it->second = true;
            sendOptions();
        }
    } else if (strategy.state() == amun::StatusStrategy::RUNNING) {
        if (m_entryPoint.isNull()) {
            dumpEntrypoints(strategy);
            qApp->exit(0);
        } else {
            QString currentEntryPoint = QString::fromStdString(strategy.current_entry_point());
            if (currentEntryPoint != m_entryPoint) {
                std::cout << "Invalid entrypoint" << std::endl;
                qApp->exit(1);
            }
        }
    }
}

void Connector::sendOptions()
{
    Command command(new amun::Command);
    amun::CommandStrategySetOptions *opts =
            command->mutable_strategy_yellow()->mutable_options();
    for (const auto &pair: m_options) {
        if (!pair.second) {
            continue;
        }
        const std::string& option = pair.first;
        std::string *stdopt = opts->add_option();
        *stdopt = option;
    }

    command->mutable_strategy_blue()->CopyFrom(command->strategy_yellow());
    command->mutable_strategy_autoref()->CopyFrom(command->strategy_yellow());
    emit sendCommand(command);
}

void Connector::dumpLog(const amun::DebugValues &debug)
{
    for (const amun::StatusLog &entry: debug.log()) {
        QString text = stripHTML(QString::fromStdString(entry.text()));
        std::pair<int, bool> exitCodeOpt = toExitCode(text);
        if (exitCodeOpt.second) {
            // don't print exit codes
            m_exitCode = exitCodeOpt.first;
        } else {
            std::cout << text.toStdString() << std::endl;
        }
    }
}

void Connector::dumpProtobuf(const google::protobuf::Message &message)
{
    std::string s;
    google::protobuf::TextFormat::PrintToString(message, &s);
    std::cout << s << std::endl;
}

std::pair<int, bool> Connector::toExitCode(const QString &str)
{
    auto parts = str.split("\n");
    const QString &firstLine = parts.at(0);
    QRegExp regex("os\\.exit\\((\\d+)\\)$");
    if (regex.indexIn(firstLine) != -1) {
        const QString exitCodeStr = regex.capturedTexts().at(1);
        bool ok = false;
        int exitCode = exitCodeStr.toInt(&ok);
        return std::make_pair(exitCode, ok);
    }
    return std::make_pair(-1, false);
}

void Connector::handleStatus(const Status &status)
{
    amun::DebugSource expectedSource = (m_asBlue) ? amun::StrategyBlue : amun::StrategyYellow;
    for (const auto& debug: status->debug()) {
        if (debug.source() == expectedSource) {
            if (m_debug) {
                dumpProtobuf(*status);
            }

            dumpLog(debug);
        }
    }

    if (status->has_status_strategy()) {
        const auto& strategy = status->status_strategy().status();
        handleStrategyStatus(strategy);
    }
}

QString Connector::stripHTML(const QString &logText)
{
    QString text = logText;
    return text
            .replace("&nbsp;", " ").replace("&gt;", ">")
            .replace("\n", "").replace("<br>", "\n")
            .remove(QRegExp("<[^>]*>"));
}

/*
"simulator {
  vision_delay: 35000000
  vision_processing_time: 5000000
}
strategy_yellow {
  enable_refbox_control: false
}
transceiver {
  configuration {
    channel: 11
  }
  network_configuration {
    host: \"\"
    port: 10010
  }
  use_network: false
}
tracking {
  system_delay: 30000000
}
amun {
  vision_port: 10005
}
mixed_team_destination {
  host: \"\"
  port: 10012
}
"
"referee {
  command: \"\\010\\000\\020\\001\\030\\000 \\000(\\0000\\000:\\022\
\\000\\020\\000\\030\\000(\\0000\\0048\\200\\306\\206\\217\\001@\\005B\\022\
\\000\\020\\000\\030\\000(\\0000\\0048\\200\\306\\206\\217\\001@\\000\"
}
"
"referee {
  command: \"\\010\\000\\020\\001\\030\\000 \\000(\\0000\\000:\\022\
\\000\\020\\000\\030\\000(\\0000\\0048\\200\\306\\206\\217\\001@\\005B\\022\
\\000\\020\\000\\030\\000(\\0000\\0048\\200\\306\\206\\217\\001@\\007\"
}
"
*/
