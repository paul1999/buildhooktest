/***************************************************************************
 *   Copyright 2016 Michael Eischer, Philipp Nordhus                       *
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

#include "amun.h"
#include "receiver.h"
#include "core/timer.h"
#include "processor/processor.h"
#include "processor/transceiver.h"
#include "processor/networktransceiver.h"
#include "simulator/simulator.h"
#include "strategy/strategy.h"
#include "networkinterfacewatcher.h"
#include <QMetaType>
#include <QThread>

/*!
 * \class Amun
 * \ingroup amun
 * \brief Core class of Amun architecture
 *
 * Creates a processor thread, a simulator thread, a networking thread and two
 * strategy threads.
 */

/*!
 * \fn void Amun::gotCommand(const Command &command)
 * \brief Passes a \ref Command
 */

/*!
 * \brief Creates an Amun instance
 * \param parent Parent object
 */
Amun::Amun(bool simulatorOnly, QObject *parent) :
    QObject(parent),
    m_processor(nullptr),
    m_transceiver(nullptr),
    m_networkTransceiver(nullptr),
    m_simulator(nullptr),
    m_referee(nullptr),
    m_vision(nullptr),
    m_networkCommand(nullptr),
    m_mixedTeam(nullptr),
    m_simulatorEnabled(false),
    m_scaling(1.0f),
    m_useNetworkTransceiver(false),
    m_simulatorOnly(simulatorOnly),
    m_networkInterfaceWatcher(nullptr)
{
    qRegisterMetaType<QNetworkInterface>("QNetworkInterface");
    qRegisterMetaType<Command>("Command");
    qRegisterMetaType< QList<robot::RadioCommand> >("QList<robot::RadioCommand>");
    qRegisterMetaType< QList<robot::RadioResponse> >("QList<robot::RadioResponse>");
    qRegisterMetaType<Status>("Status");

    m_strategy[0] = NULL;
    m_strategy[1] = NULL;

    // global timer, which can be slowed down / sped up
    m_timer = new Timer;
    // these threads just run an event loop
    // using the signal-slot mechanism the objects in these can be called
    m_processorThread = new QThread(this);
    m_networkThread = new QThread(this);
    m_simulatorThread = new QThread(this);
    m_strategyThread[0] = new QThread(this);
    m_strategyThread[1] = new QThread(this);

    m_networkInterfaceWatcher = (!m_simulatorOnly) ? new NetworkInterfaceWatcher(this) : nullptr;
}

/*!
 * \brief Destroys the Amun instance
 */
Amun::~Amun()
{
    stop();
    delete m_timer;
}

/*!
 * \brief Start processing
 *
 * This method starts all threads.
 */
void Amun::start()
{
    // create processor
    Q_ASSERT(m_processor == NULL);
    m_processor = new Processor(m_timer);
    m_processor->moveToThread(m_processorThread);
    connect(m_processorThread, SIGNAL(finished()), m_processor, SLOT(deleteLater()));
    // route commands to processor
    connect(this, SIGNAL(gotCommand(Command)), m_processor, SLOT(handleCommand(Command)));
    // relay tracking, geometry, referee, controller and accelerator information
    connect(m_processor, SIGNAL(sendStatus(Status)), SLOT(handleStatus(Status)));

    // start strategy threads
    for (int i = 0; i < 2; i++) {
        Q_ASSERT(m_strategy[i] == NULL);
        m_strategy[i] = new Strategy(m_timer, (i == 0) ? StrategyType::YELLOW : StrategyType::BLUE);
        m_strategy[i]->moveToThread(m_strategyThread[i]);
        connect(m_strategyThread[i], SIGNAL(finished()), m_strategy[i], SLOT(deleteLater()));

        // send tracking, geometry and referee to strategy
        connect(m_processor, SIGNAL(sendStrategyStatus(Status)),
                m_strategy[i], SLOT(handleStatus(Status)));
        // forward robot commands to processor
        connect(m_strategy[i], SIGNAL(sendStrategyCommand(bool, unsigned int, unsigned int, QByteArray, qint64)),
                m_processor, SLOT(handleStrategyCommand(bool, unsigned int, unsigned int, QByteArray, qint64)));
        connect(m_strategy[i], SIGNAL(sendHalt(bool)),
                m_processor, SLOT(handleStrategyHalt(bool)));

        // route commands from and to strategy
        connect(m_strategy[i], SIGNAL(gotCommand(Command)), SLOT(handleCommand(Command)));
        connect(this, SIGNAL(gotCommand(Command)),
                m_strategy[i], SLOT(handleCommand(Command)));
        // relay status and debug information of strategy
        connect(m_strategy[i], SIGNAL(sendStatus(Status)), SLOT(handleStatus(Status)));
    }

    if (!m_simulatorOnly) {
        // create referee
        setupReceiver(m_referee, QHostAddress("224.5.23.1"), 10003);
        // move referee packets to processor
        connect(m_referee, SIGNAL(gotPacket(QByteArray, qint64)), m_processor, SLOT(handleRefereePacket(QByteArray, qint64)));

        // create vision
        setupReceiver(m_vision, QHostAddress("224.5.23.2"), 10002);
        // allow updating the port used to listen for ssl vision
        connect(this, &Amun::updateVisionPort, m_vision, &Receiver::updatePort);
        // vision is connected in setSimulatorEnabled
        connect(m_vision, &Receiver::sendStatus, this, &Amun::handleStatus);

        // create network radio protocol receiver
        setupReceiver(m_networkCommand, QHostAddress(), 10010);
        // pass packets to processor
        connect(m_networkCommand, SIGNAL(gotPacket(QByteArray, qint64)), m_processor, SLOT(handleNetworkCommand(QByteArray, qint64)));

        // create mixed team information receiver
        setupReceiver(m_mixedTeam, QHostAddress(), 10012);
        // pass packets to processor
        connect(m_mixedTeam, SIGNAL(gotPacket(QByteArray,qint64)), m_processor, SLOT(handleMixedTeamInfo(QByteArray, qint64)));
    }

    // create simulator
    Q_ASSERT(m_simulator == NULL);
    m_simulator = new Simulator(m_timer);
    m_simulator->moveToThread(m_simulatorThread);
    connect(m_simulatorThread, SIGNAL(finished()), m_simulator, SLOT(deleteLater()));
    // pass on simulator and team settings
    connect(this, SIGNAL(gotCommand(Command)), m_simulator, SLOT(handleCommand(Command)));
    // pass simulator timing
    connect(m_simulator, SIGNAL(sendStatus(Status)), SLOT(handleStatus(Status)));

    if (!m_simulatorOnly) {
        Q_ASSERT(m_transceiver == NULL);
        m_transceiver = new Transceiver();
        m_transceiver->moveToThread(m_processorThread);
        connect(m_processorThread, SIGNAL(finished()), m_transceiver, SLOT(deleteLater()));
        // route commands to transceiver
        connect(this, SIGNAL(gotCommand(Command)), m_transceiver, SLOT(handleCommand(Command)));
        // relay transceiver status and timing
        connect(m_transceiver, SIGNAL(sendStatus(Status)), SLOT(handleStatus(Status)));

        Q_ASSERT(m_networkTransceiver == NULL);
        m_networkTransceiver = new NetworkTransceiver();
        m_networkTransceiver->moveToThread(m_processorThread);
        connect(m_processorThread, SIGNAL(finished()), m_networkTransceiver, SLOT(deleteLater()));
        // route commands to transceiver
        connect(this, SIGNAL(gotCommand(Command)), m_networkTransceiver, SLOT(handleCommand(Command)));
        // relay transceiver status and timing
        connect(m_networkTransceiver, SIGNAL(sendStatus(Status)), SLOT(handleStatus(Status)));
    }

    // connect transceiver
    setSimulatorEnabled(m_simulatorOnly, false);

    // start threads
    m_processorThread->start();
    m_networkThread->start();
    m_simulatorThread->start();
    m_strategyThread[0]->start();
    m_strategyThread[1]->start();
}

/*!
 * \brief Stop processing
 *
 * All threads are stopped.
 */
void Amun::stop()
{
    // stop threads
    m_processorThread->quit();
    m_networkThread->quit();
    m_simulatorThread->quit();
    m_strategyThread[0]->quit();
    m_strategyThread[1]->quit();

    // wait for threads
    m_processorThread->wait();
    m_networkThread->wait();
    m_simulatorThread->wait();
    m_strategyThread[0]->wait();
    m_strategyThread[1]->wait();

    // worker objects are destroyed on thread shutdown
    m_transceiver = NULL;
    m_networkTransceiver = NULL;
    m_networkCommand = NULL;
    m_simulator = NULL;
    m_vision = NULL;
    m_referee = NULL;
    m_mixedTeam = NULL;
    for (int i = 0; i < 2; i++) {
        m_strategy[i] = NULL;
    }
    m_processor = NULL;
}

void Amun::setupReceiver(Receiver *&receiver, const QHostAddress &address, quint16 port)
{
    Q_ASSERT(receiver == NULL);
    receiver = new Receiver(address, port);
    receiver->moveToThread(m_networkThread);
    connect(m_networkThread, SIGNAL(finished()), receiver, SLOT(deleteLater()));
    // start and stop socket
    connect(m_networkThread, SIGNAL(started()), receiver, SLOT(startListen()));
    // pass packets to processor
    connect(m_networkInterfaceWatcher, &NetworkInterfaceWatcher::interfaceUpdated, receiver, &Receiver::updateInterface);
}

/*!
 * \brief Process a command
 * \param command Command to process
 */
void Amun::handleCommand(const Command &command)
{
    if (command->has_simulator()) {
        if (command->simulator().has_enable()) {
            setSimulatorEnabled(command->simulator().enable(), m_useNetworkTransceiver);

            // time scaling can only be used with the simulator
            if (m_simulatorEnabled) {
                updateScaling(m_scaling);
            } else {
                // reset timer to realtime
                m_timer->reset();
                updateScaling(1.0);
            }
        }
    }

    if (command->has_speed()) {
        m_scaling = command->speed();
        if (m_simulatorEnabled) {
            updateScaling(m_scaling);
        }
    }

    if (command->has_amun()) {
        if (command->amun().has_vision_port()) {
            emit updateVisionPort(command->amun().vision_port());
        }
    }

    if (command->has_transceiver()) {
        if (command->transceiver().has_use_network()) {
            setSimulatorEnabled(m_simulatorEnabled, command->transceiver().use_network());
        }
    }
    emit gotCommand(command);
}

/*!
 * \brief Set time scaling
 * \param scaling Scaling factor for time
 */
void Amun::updateScaling(float scaling)
{
    m_timer->setScaling(scaling);
}

/*!
 * \brief Add timestamp and emit \ref sendStatus
 * \param status Status to send
 */
void Amun::handleStatus(const Status &status)
{
    status->set_time(m_timer->currentTime());
    emit sendStatus(status);
}

/*!
 * \brief Toggle simulator mode
 *
 * Switches vision input of \ref Processor to either SSLVision or
 * \ref Simulator and output of \ref Processor to either \ref Transceiver or \ref Simulator.
 *
 * \param enabled Whether to enable or disable the simulator
 * \param useNetworkTransceiver Whether to send commands over network or using the transceiver
 */
void Amun::setSimulatorEnabled(bool enabled, bool useNetworkTransceiver)
{
    if (m_simulatorOnly) {
        enabled = true;
    }
    m_simulatorEnabled = enabled;
    m_useNetworkTransceiver = useNetworkTransceiver;
    // remove vision connections
    m_simulator->disconnect(m_processor);
    if (!m_simulatorOnly) {
        m_vision->disconnect(m_processor);
    }
    //remove radio command and response connections
    m_processor->disconnect(m_simulator);
    if (!m_simulatorOnly) {
        m_processor->disconnect(m_transceiver);
        m_processor->disconnect(m_networkTransceiver);
    }

    if (enabled) {
        connect(m_simulator, SIGNAL(gotPacket(QByteArray, qint64)),
                m_processor, SLOT(handleVisionPacket(QByteArray,qint64)));
        connect(m_simulator, SIGNAL(sendRadioResponses(QList<robot::RadioResponse>)),
                m_processor, SLOT(handleRadioResponses(QList<robot::RadioResponse>)));
        connect(m_processor, SIGNAL(sendRadioCommands(QList<robot::RadioCommand>)),
                m_simulator, SLOT(handleRadioCommands(QList<robot::RadioCommand>)));
    } else {
        connect(m_vision, SIGNAL(gotPacket(QByteArray, qint64)),
                m_processor, SLOT(handleVisionPacket(QByteArray,qint64)));
        if (!useNetworkTransceiver) {
            connect(m_transceiver, SIGNAL(sendRadioResponses(QList<robot::RadioResponse>)),
                    m_processor, SLOT(handleRadioResponses(QList<robot::RadioResponse>)));
            connect(m_processor, SIGNAL(sendRadioCommands(QList<robot::RadioCommand>)),
                    m_transceiver, SLOT(handleRadioCommands(QList<robot::RadioCommand>)));
        } else {
            connect(m_processor, SIGNAL(sendRadioCommands(QList<robot::RadioCommand>)),
                    m_networkTransceiver, SLOT(handleRadioCommands(QList<robot::RadioCommand>)));
        }
    }
}
