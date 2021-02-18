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
#include "networktransceiver.h"
#include "core/timer.h"
#include "protobuf/grsim_commands.pb.h"
#include "protobuf/grsim_replacement.pb.h"
#include "protobuf/ssl_simulation_robot_control.pb.h"
#include <QUdpSocket>
#include <cmath>

NetworkTransceiver::NetworkTransceiver(QObject *parent) : QObject(parent),
    m_charge(false)
{
    m_udpSocket = new QUdpSocket(this);
}

NetworkTransceiver::~NetworkTransceiver() { }

bool NetworkTransceiver::sendSSLSimPacket(const QList<robot::RadioCommand> &commands, bool blueTeam) {
    sslsim::RobotControl control;
    for (const robot::RadioCommand &robot : commands) {
        if (robot.is_blue() != blueTeam) {
            continue;
        }
        auto* robotCommand = control.add_robot_commands();
        robotCommand->set_id(robot.id());
        if (robot.command().kick_power() > 0 && m_charge) {
            robotCommand->set_kick_speed(robot.command().kick_power());
            if (robot.command().kick_style() == robot::Command::Chip) {
                robotCommand->set_kick_angle(45);
            }
        }
        if (robot.command().has_dribbler()) {
            robotCommand->set_dribbler_speed(robot.command().dribbler() * 150 * 60 * .5f / M_PI ); // convert from 1 - 0 to rpm, where 1 is 150 rad/s
        }
        if (robot.command().has_output1()) {
            auto* moveCommand  = robotCommand->mutable_move_command()->mutable_local_velocity();
            moveCommand->set_forward(robot.command().output1().v_f());
            moveCommand->set_left(-robot.command().output1().v_s());
            moveCommand->set_angular(robot.command().output1().omega());
        }
    }

    bool sendingSuccessful = false;
    if (m_configuration.IsInitialized()) {
        QHostAddress address(QString::fromStdString(m_configuration.host()));

        QByteArray data;
        data.resize(control.ByteSize());
        if (control.SerializeToArray(data.data(), data.size())) {
            sendingSuccessful = m_udpSocket->writeDatagram(data, address, m_configuration.port()) == data.size();
        }
    }
    return sendingSuccessful;

}

void NetworkTransceiver::handleRadioCommands(const QList<robot::RadioCommand> &commands, qint64 processingStart)
{
    (void)processingStart;
    Status status(new amun::Status);
    const qint64 transceiver_start = Timer::systemTime();

    bool sendingSuccessful = sendSSLSimPacket(commands, false);
    sendingSuccessful &= sendSSLSimPacket(commands, true);

    status->mutable_timing()->set_transceiver((Timer::systemTime() - transceiver_start) * 1E-9f);
    status->mutable_transceiver()->set_active(sendingSuccessful);
    status->mutable_transceiver()->set_error("Network");
    emit sendStatus(status);
}

void NetworkTransceiver::handleCommand(const Command &command)
{
    if (command->has_transceiver()) {
        const amun::CommandTransceiver &t = command->transceiver();

        if (t.has_charge()) {
            m_charge = t.charge();
        }

        if (t.has_network_configuration()) {
            m_configuration = t.network_configuration();
        }

        if (t.has_enable()) {
            if (!t.enable()) {
                Status status(new amun::Status);
                status->mutable_transceiver()->set_active(false);
                emit sendStatus(status);
            }
        }
    }
}

