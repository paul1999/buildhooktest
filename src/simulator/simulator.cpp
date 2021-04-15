/****************************************************************************
 *   Copyright 2021 Tobias Heineken                                        *
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
#include <clocale>
#include <QApplication>
#include <QUdpSocket>
#include <QThread>
#include <QNetworkDatagram>
#include <QCommandLineParser>
#include <cmath>

#include "protobuf/ssl_simulation_robot_control.pb.h"
#include "protobuf/ssl_simulation_robot_feedback.pb.h"
#include "protobuf/ssl_simulation_custom_erforce_robot_spec.pb.h"
#include "protobuf/sslsim.h"
#include "protobuf/status.h"
#include "protobuf/command.h"
#include "protobuf/geometry.h"
#include "simulator/simulator.h"

#include "core/timer.h"
#include "core/run_out_of_scope.h"
#include "core/configuration.h"

#include "ssl_robocup_server.h"

static int BLUE_PORT = 10301;
static int YELLOW_PORT = 10302;
static int CONTROL_PORT = 10300;


/**
 * Stand alone Erforce simulator
 *
 * Known issues:
 *  - [ ]: Currently, it is not possible to supply partial positions for teleportBall or teleportRobot
 *  - [ ]: Simulator Config port change is not implemented
 *  - [ ]: Simulator realism will be discarded if a new world is created
 *  - [ ]: Robots go into standby after 0.1 seconds without command (Safty)
 *  - [ ]: The newstes simprotocol updates are not used
 *  - [ ]: It is not possible to change specs or geometry without resetting the world
 *  - [ ]: It is not possible to setUp a team with no robots if that team already had robots (You can still teleport them away)
 *  - [ ]: Dribbler will reset if a new command doesn't contain a new dribbling speed (contrary to the definition that states all not set values should stay as previously assumed)
 *  - [ ]: Commands that are recieved at t0 will not be in effect after the next tick of the simulator (around 5 ms), no interpolation.
 *  - [ ]: Tournament mode where commands origin are checked is not implemented
 */

class SimulatorCommandAdaptor: public QObject {
    Q_OBJECT
public:
    SimulatorCommandAdaptor(Timer* timer);
private slots:
    void handleDatagrams();

public slots:
    void handleSimulatorError(const QList<SSLSimError> &error, camun::simulator::ErrorSource source);

signals:
    void sendCommand(const Command& c);

private:
    QUdpSocket m_server;
    QHostAddress m_senderAddress;
    int m_senderPort;
    Timer* m_timer; // unowned
};

SimulatorCommandAdaptor::SimulatorCommandAdaptor(Timer* timer):
    m_server(this),
    m_senderAddress(QHostAddress::Null),
    m_senderPort(-1),
    m_timer(timer)
{
    m_server.bind(QHostAddress::Any, CONTROL_PORT);
    connect(&m_server, &QUdpSocket::readyRead, this, &SimulatorCommandAdaptor::handleDatagrams);
}

class RobotCommandAdaptor: public QObject{
    Q_OBJECT
public:
    RobotCommandAdaptor(bool blue, Timer* timer);

private:
    void sendRobotRespose(const sslsim::RobotControlResponse& rcr);

public slots:
    void handleRobotResponse(const QList<robot::RadioResponse>& responses);
    void handleSimulatorError(const QList<SSLSimError> &error, camun::simulator::ErrorSource source);

private slots:
    void handleDatagrams();

signals:
    void sendRadioCommands(const SSLSimRobotControl & commands, bool isBlue, qint64 processingDelay);


private:
    bool m_is_blue;
    QUdpSocket m_server;
    QHostAddress m_senderAddress;
    int m_senderPort;
    Timer* m_timer; // unowned
};

RobotCommandAdaptor::RobotCommandAdaptor(bool blue, Timer* timer): m_is_blue(blue),
    m_server(this),
    m_senderAddress(QHostAddress::Null),
    m_senderPort(-1),
    m_timer(timer)
{
    m_server.bind(QHostAddress::Any, (blue)? BLUE_PORT : YELLOW_PORT);
    connect(&m_server, &QUdpSocket::readyRead, this, &RobotCommandAdaptor::handleDatagrams);
}

enum class SimError {
    UNSUPPORTED_VELOCITY,
    UNSUPPORTED_ANGLE,
    UNREADABLE,
    MISSING_SPEC,
    INVALID_REALISM,
};

static void setError(sslsim::SimulatorError* error, SimError code, std::string appendix = "") {
    switch(code) {
        case SimError::UNREADABLE:
            error->set_code("UNREADABLE");
            error->set_message("The recieved message was unreadable " + appendix);
            break;
        case SimError::UNSUPPORTED_VELOCITY:
            error->set_code("VELOCITY_TYPE");
            error->set_message("The recieved message had a velocity type unsupported by this simulator "+appendix);
            break;
        case SimError::UNSUPPORTED_ANGLE:
            error->set_code("ANGLE_VALUE");
            error->set_message("The recieved kick angle was not equal to either 0 or 45 " + appendix);
            break;
        case SimError::MISSING_SPEC:
            error->set_code("INVALID_SPEC");
            error->set_message("The recieved spec is missing one of the required fields for this simultor " + appendix);
        case SimError::INVALID_REALISM:
            error->set_code("INVALID_REALISM");
            error->set_message("The recieved realism is not conforming to the realism configuration for this simulator " + appendix);
        default:
            std::cerr << "Unmanaged SimError for message" << std::endl;
    }
}

static void sendUDP(const google::protobuf::Message& out, QUdpSocket& server, const QHostAddress& senderAddress, int senderPort) {
    QByteArray data;
    data.resize(out.ByteSize());
    bool sendingSuccessful = false;
    if (out.SerializeToArray(data.data(), data.size())) {
        sendingSuccessful = server.writeDatagram(data, senderAddress, senderPort) == data.size();
    }
    if (!sendingSuccessful) {
        std::cerr << "Sending relpy failed: " << std::endl;
    }
}

#define SCALE_UP(OBJ, ATTR) do{if((OBJ).has_##ATTR()) (OBJ).set_##ATTR((OBJ).ATTR() * 1e3);} while(0)

static void warnLatency(qint64 delta) {
    if (delta > 1e6) {
        std::cout << "Warning: Handled Datagram in " << delta <<"ns, should be lower than 1e6"<< std::endl;
    }
}


//TODO: Always update the following constant if the robotSpecs did change,
// either disregard the new field and just increase the expected number if the new field is useless to our simulator,
// or convert it properly and update this number.
constexpr int expected_specs_fields = 10 + 3;
constexpr int functionToFixForSpecs = __LINE__;
template<class T>
static bool convertSpecsToErForce(T outGen, const sslsim::RobotSpecs& in) // @return false: Error occured
{
    if (!in.has_mass()) {
        return false;
    }
    if (!in.has_limits()) {
        return false;
    }
    if (!in.has_custom()) {
        return false;
    }
    if (!in.has_dribbler_width()) {
        return false;
    }
    sslsim::RobotSpecErForce rsef;
    /*
    optional float shoot_radius = 1;
    // The height of the dribbling bar from the ground [m]
    optional float dribbler_height = 2;
    // The 'loss' at the front of the robot where the dribbler bar does not (yet) start, due to the mechanical layout of the robot [m]
    optional float dribbler_width_extra = 3 [default = 0.014];
    */
    if (!in.custom().UnpackTo(&rsef)) {
        return false;
    }
    if (!rsef.has_shoot_radius()) {
        return false;
    }
    if (!rsef.has_dribbler_height()) {
        return false;
    }
    const sslsim::RobotLimits& lim = in.limits();
    if (!lim.has_acc_speedup_absolute_max()) {
        return false;
    }
    if (!lim.has_acc_speedup_angular_max()) {
        return false;
    }
    if (!lim.has_acc_brake_absolute_max()) {
        return false;
    }
    if (!lim.has_acc_brake_angular_max()) {
        return false;
    }
    if (!lim.has_vel_absolute_max()) {
        return false;
    }
    if (!lim.has_vel_angular_max()) {
        return false;
    }
    if (!in.id().has_id()) {
        return false;
    }
    if (!in.id().has_team()) {
        return false;
    }
    robot::Specs* out = outGen(in.id().team() == gameController::BLUE);
    out->set_year(1970);
    out->set_generation(0);
    out->set_id(in.id().id());
    out->set_type(robot::Specs_GenerationType_Regular);
    out->set_radius(in.radius());
    out->set_height(in.height());
    out->set_mass(in.mass());
    out->set_v_max(lim.vel_absolute_max());
    out->set_omega_max(lim.vel_angular_max());
    if (in.has_max_linear_kick_speed()) {
        out->set_shot_linear_max(in.max_linear_kick_speed());
    }
    else {
        out->set_shot_linear_max(100);
    }
    // TODO: chips
    out->set_dribbler_width(in.dribbler_width());
    auto* acc = out->mutable_strategy();

    acc->set_a_speedup_f_max(lim.acc_speedup_absolute_max());
    acc->set_a_speedup_s_max(lim.acc_speedup_absolute_max());
    acc->set_a_speedup_phi_max(lim.acc_speedup_angular_max());
    acc->set_a_brake_f_max(lim.acc_brake_absolute_max());
    acc->set_a_brake_s_max(lim.acc_brake_absolute_max());
    acc->set_a_brake_phi_max(lim.acc_brake_angular_max());

    out->set_shoot_radius(rsef.shoot_radius());
    out->set_dribbler_height(rsef.dribbler_height());

    // TODO: angle
    out->set_angle(0.982f);
    return true;
            /*
// Movement limits for a robot
message RobotLimits {
    // Max absolute speed-up acceleration [m/s^2]
    optional float acc_speedup_absolute_max = 1;
    // Max angular speed-up acceleration [rad/s^2]
    optional float acc_speedup_angular_max = 2;
    // Max absolute brake acceleration [m/s^2]
    optional float acc_brake_absolute_max = 3;
    // Max angular brake acceleration [rad/s^2]
    optional float acc_brake_angular_max = 4;
    // Max absolute velocity [m/s]
    optional float vel_absolute_max = 5;
    // Max angular velocity [rad/s]
    optional float vel_angular_max = 6;
    */
}


void SimulatorCommandAdaptor::handleDatagrams() {
    while(m_server.hasPendingDatagrams()) {
        qint64 start = m_timer->currentTime();
        auto datagram = m_server.receiveDatagram();
        sslsim::SimulatorResponse sir;
        bool sendSir = false;
        m_senderAddress = datagram.senderAddress();
        m_senderPort = datagram.senderPort();
        auto data = datagram.data();

        RUN_WHEN_OUT_OF_SCOPE({
                if (sendSir) {
                    sendUDP(sir, m_server, m_senderAddress, m_senderPort);
                }
            });
        sslsim::SimulatorCommand simcom;
        if (!simcom.ParseFromArray(data.data(), data.size())) {
            sendSir = true;
            setError(sir.add_errors(), SimError::UNREADABLE);
            continue;
        }
        if (simcom.has_control()) {
            Command c{new amun::Command};
            auto* sslControl = c->mutable_simulator()->mutable_ssl_control();
            sslControl->CopyFrom(simcom.control());
            if (sslControl->has_teleport_ball()) {
                auto* teleportBall = sslControl->mutable_teleport_ball();
                SCALE_UP(*teleportBall, x);
                SCALE_UP(*teleportBall, y);
                SCALE_UP(*teleportBall, z);
                SCALE_UP(*teleportBall, vx);
                SCALE_UP(*teleportBall, vy);
                SCALE_UP(*teleportBall, vz);
            }
            for(sslsim::TeleportRobot& robot : *sslControl->mutable_teleport_robot()) {
                SCALE_UP(robot, x);
                SCALE_UP(robot, y);
                SCALE_UP(robot, v_x);
                SCALE_UP(robot, v_y);
            }
            emit sendCommand(c);
        }
        if (simcom.has_config()) {
            const auto& config{simcom.config()};

            if (config.has_geometry()) {
                Command c{new amun::Command};
                auto* setup = c->mutable_simulator()->mutable_simulator_setup();
                convertFromSSlGeometry(config.geometry().field(), *(setup->mutable_geometry()));
                setup->mutable_camera_setup()->CopyFrom(config.geometry().calib());
                emit sendCommand(c);
            }

            if (config.robot_specs_size() > 0) {
                Command c{new amun::Command};
                robot::Team* blueTeam = nullptr;
                robot::Team* yellowTeam = nullptr;
                auto newSz = config.robot_specs_size();
                for (const auto& spec : config.robot_specs()) {
                    bool success = convertSpecsToErForce([&blueTeam, &yellowTeam, &c](bool isBlue){
                            if (isBlue) {
                                if (blueTeam == nullptr) {
                                    blueTeam = c->mutable_set_team_blue();
                                }
                                return blueTeam->add_robot();
                            }
                            if (yellowTeam == nullptr) {
                                yellowTeam = c->mutable_set_team_yellow();
                            }
                            return yellowTeam->add_robot();
                            }
                            , spec);
                    if (!success) {
                        sendSir = true;
                        setError(sir.add_errors(), SimError::MISSING_SPEC, spec.DebugString());
                        newSz--;
                    }
                }
                std::cout << "Updated to " << newSz << " robots" << std::endl;
                emit sendCommand(c);
            }
            if (config.has_realism_config() && config.realism_config().has_custom()) {
                RealismConfigErForce rcef;
                if (config.realism_config().custom().UnpackTo(&rcef)) {
                    Command c{new amun::Command};
                    c->mutable_simulator()->mutable_realism_config()->CopyFrom(rcef);
                    emit sendCommand(c);
                } else {
                    sendSir = true;
                    setError(sir.add_errors(), SimError::INVALID_REALISM, config.realism_config().DebugString());
                }
            }
        }

        qint64 delta = m_timer->currentTime() - start;
        warnLatency(delta);
    }
}

void RobotCommandAdaptor::handleSimulatorError(const QList<SSLSimError> &error,camun::simulator::ErrorSource source)
{

    camun::simulator::ErrorSource expected = m_is_blue ? camun::simulator::ErrorSource::BLUE : camun::simulator::ErrorSource::YELLOW;
    if (source != expected) return;
    if (error.size() == 0) return;

    sslsim::RobotControlResponse rcr;

    for(const SSLSimError& err : error) {
        auto* sendError = rcr.add_errors();
        *sendError = *err;
    }

    sendRobotRespose(rcr);
}

void SimulatorCommandAdaptor::handleSimulatorError(const QList<SSLSimError> &error, camun::simulator::ErrorSource source) {
    if (source != camun::simulator::ErrorSource::CONFIG) return;
    if (error.size() == 0) return;
    sslsim::SimulatorResponse sir;
    for (const SSLSimError& err : error) {
        sir.add_errors()->CopyFrom(*err);
    }
    sendUDP(sir, m_server, m_senderAddress, m_senderPort);
}


void RobotCommandAdaptor::handleDatagrams()
{
    while(m_server.hasPendingDatagrams()) {
        qint64 start =m_timer->currentTime();
        sslsim::RobotControlResponse rcr;
        bool sendRcr = false;
        auto datagram = m_server.receiveDatagram();
        // TODO: do something with m_senderAddress and datagram.senderAddress
        m_senderAddress = datagram.senderAddress();
        m_senderPort = datagram.senderPort();
        auto data = datagram.data();

        RUN_WHEN_OUT_OF_SCOPE({
                if (sendRcr) {
                    sendRobotRespose(rcr);
                }
            });

        SSLSimRobotControl control{new sslsim::RobotControl};
        if (!control->ParseFromArray(data.data(), data.size())) {
            sendRcr = true;
            setError(rcr.add_errors(), SimError::UNREADABLE);
            continue;
        }

        for (const auto& command : control->robot_commands()) {
            if (command.has_move_command()) {
                const auto& moveCmd = command.move_command();
                if (moveCmd.has_wheel_velocity() || moveCmd.has_global_velocity()) {
                    sendRcr = true;
                    setError(rcr.add_errors(), SimError::UNSUPPORTED_VELOCITY, std::string{"(Robot :"}+std::to_string(command.id()) + ")");
                }
            }
        }
        emit sendRadioCommands(control, m_is_blue, m_timer->currentTime()); // This might be a bit late.
        // TODO: response!
        qint64 delta = m_timer->currentTime() - start;

        warnLatency(delta);
    }
}

void RobotCommandAdaptor::handleRobotResponse(const QList<robot::RadioResponse>& res) {
    if (m_senderAddress.isNull()) {
        return;
    }

    sslsim::RobotControlResponse out;
    bool send = false;

    for (const auto& response : res) {
        if (response.has_is_blue() && response.is_blue() == m_is_blue && response.has_ball_detected()) {
            auto* outFeedback = out.add_feedback();
            outFeedback->set_id(response.id());
            outFeedback->set_dribbler_ball_contact(response.ball_detected());
            send = true;
        }
    }

    if (send) {
        sendRobotRespose(out);
    }
}

void RobotCommandAdaptor::sendRobotRespose(const sslsim::RobotControlResponse& out) {
    sendUDP(out, m_server, m_senderAddress, m_senderPort);
}

class SSLVisionServer: public QObject {
    Q_OBJECT
public:
    SSLVisionServer(int port);

public slots:
    void sendVisionData(const QByteArray& data, qint64 time, QString sender);

private:
    RoboCupSSLServer m_server;
};

SSLVisionServer::SSLVisionServer(int port): m_server(this, port)
{
}

void SSLVisionServer::sendVisionData(const QByteArray& data, qint64, QString)
{
    m_server.send(data);
}

using camun::simulator::Simulator;
using camun::simulator::ErrorSource;

class SimProxy: public QObject {
    Q_OBJECT
public:
    SimProxy(Timer* t): m_timer(t) {}
signals:
    void sendSSLSimError(const QList<SSLSimError>& errors, ErrorSource source); // out
    void sendRadioResponses(const QList<robot::RadioResponse> &responses); // out
    void gotPacket(const QByteArray &data, qint64 time, QString sender); // out
    void gotCommand(const Command &command); // internal
    void handleRadioCommands(const SSLSimRobotControl& control, bool isBlue, qint64 processingStart); // in
public slots:
    void handleCommand(const Command &command);

private:
    Timer* m_timer;
    Simulator* m_sim = nullptr;
    Command m_teamCommand{new amun::Command};
};

void SimProxy::handleCommand(const Command &command) {
    bool hasSimSetup = command->has_simulator() && command->simulator().has_simulator_setup();

    if (command->has_set_team_blue()) {
        m_teamCommand->mutable_set_team_blue()->CopyFrom(command->set_team_blue());
        if (hasSimSetup) {
            command->clear_set_team_blue();
        }
    }
    if (command->has_set_team_yellow()) {
        m_teamCommand->mutable_set_team_yellow()->CopyFrom(command->set_team_yellow());
        if (hasSimSetup) {
            command->clear_set_team_yellow();
        }
    }
    if (hasSimSetup) {
        // replace m_sim
        if (m_sim != nullptr) {
            // replace old connectios
            m_sim->blockSignals(true);
            m_sim->deleteLater();
        }
        m_sim = new Simulator(m_timer, command->simulator().simulator_setup());
        connect(this, &SimProxy::gotCommand, m_sim, &Simulator::handleCommand);
        connect(m_sim, &Simulator::gotPacket, this, &SimProxy::gotPacket);
        connect(this, &SimProxy::handleRadioCommands, m_sim, &Simulator::handleRadioCommands);
        connect(m_sim, &Simulator::sendSSLSimError, this, &SimProxy::sendSSLSimError);
        connect(m_sim, &Simulator::sendRadioResponses, this, &SimProxy::sendRadioResponses);
        auto* simCommand = m_teamCommand->mutable_simulator();
        simCommand->set_enable(true);
        auto* trCommand = m_teamCommand->mutable_transceiver();
        trCommand->set_charge(true);
        emit gotCommand(m_teamCommand);
    }
    emit gotCommand(command);
}

#include "simulator.moc"



int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("Simulator");
    app.setOrganizationName("ER-Force");

    std::setlocale(LC_NUMERIC, "C");

    qRegisterMetaType<QList<robot::RadioResponse>>("QList<robot::RadioResponse>");
    qRegisterMetaType<Status>("Status");
    qRegisterMetaType<Command>("Command");
    qRegisterMetaType<SSLSimRobotControl>("SSLSimRobotControl");
    qRegisterMetaType<SSLSimError>("SSLSimError");
    qRegisterMetaType<QList<SSLSimError>>("QList<SSLSimError>");
    qRegisterMetaType<camun::simulator::ErrorSource>("ErrorSource");

    QCommandLineParser parser;
    parser.setApplicationDescription("ER-Force simulator command line interface");
    parser.addHelpOption();

    QCommandLineOption geometryConfig({"g", "geometry"}, "The geometry file to load as default", "file", "2020");
    parser.addOption(geometryConfig);


    parser.process(app);

    auto* desc = sslsim::RobotSpecs::descriptor();
    int real_fields = desc->field_count();
    auto* desc2 = sslsim::RobotSpecErForce::descriptor();
    real_fields += desc2->field_count();


    if (real_fields != expected_specs_fields) {
        std::string msg = "BUG: The number of fields for the specs has a different number compared to expected!";
        msg += " expected: " + std::to_string(expected_specs_fields);
        msg += ", but found: " + std::to_string(real_fields);
        msg += " (in ";
        msg += __FILE__;
        msg += ": ";
        msg += std::to_string(functionToFixForSpecs);
        std::cout << msg << ')' << std::endl;
    }

    Timer timer;
    RobotCommandAdaptor blue{true, &timer}, yellow{false, &timer};
    SimulatorCommandAdaptor commands{&timer};
    SimProxy sim{&timer};

    blue.connect(&blue, &RobotCommandAdaptor::sendRadioCommands, &sim, &SimProxy::handleRadioCommands);
    blue.connect(&sim, &SimProxy::sendRadioResponses, &blue, &RobotCommandAdaptor::handleRobotResponse);
    yellow.connect(&yellow, &RobotCommandAdaptor::sendRadioCommands, &sim, &SimProxy::handleRadioCommands);
    yellow.connect(&sim, &SimProxy::sendRadioResponses, &yellow, &RobotCommandAdaptor::handleRobotResponse);

    SSLVisionServer vision{10020};

    vision.connect(&sim, &SimProxy::gotPacket, &vision, &SSLVisionServer::sendVisionData);
    commands.connect(&commands, &SimulatorCommandAdaptor::sendCommand, &sim, &SimProxy::handleCommand);


    commands.connect(&sim, &SimProxy::sendSSLSimError, &commands, &SimulatorCommandAdaptor::handleSimulatorError);
    blue.connect(&sim, &SimProxy::sendSSLSimError, &blue, &RobotCommandAdaptor::handleSimulatorError);
    yellow.connect(&sim, &SimProxy::sendSSLSimError, &yellow, &RobotCommandAdaptor::handleSimulatorError);

    Command c{new amun::Command};
    // start with 6 robots for yellow and blue, take ER-Force specs.


    robot::Specs ERForce;
    ERForce.set_generation(0);
    ERForce.set_year(1970);
    ERForce.set_type(robot::Specs::Regular);
    ERForce.set_mass(1.5);
    ERForce.set_angle(0.98291);
    ERForce.set_v_max(3);
    ERForce.set_omega_max(6);
    ERForce.set_shot_linear_max(8);
    ERForce.set_shot_chip_max(3);
    ERForce.set_dribbler_width(0.07);
    ERForce.set_shoot_radius(0.067);
    ERForce.set_dribbler_height(0.04);

    auto* accel = ERForce.mutable_acceleration();
    accel->set_a_speedup_f_max(7);
    accel->set_a_speedup_s_max(6);
    accel->set_a_speedup_phi_max(60);
    accel->set_a_brake_f_max(7);
    accel->set_a_brake_s_max(6);
    accel->set_a_brake_phi_max(60);

    auto* str = ERForce.mutable_strategy();
    str->set_a_speedup_f_max(7);
    str->set_a_speedup_s_max(6);
    str->set_a_speedup_phi_max(60);
    str->set_a_brake_f_max(7);
    str->set_a_brake_s_max(6);
    str->set_a_brake_phi_max(60);

    auto* teamBlue = c->mutable_set_team_blue();
    auto* teamYellow = c->mutable_set_team_yellow();
    for(auto* team : {teamBlue, teamYellow}) {
        for(int i=0; i < 6; ++i){
            auto* robot = team->add_robot();
            robot->CopyFrom(ERForce);
            robot->set_id(i);
        }
    }

    if (!loadConfiguration("simulator/" + parser.value(geometryConfig), c->mutable_simulator()->mutable_simulator_setup(), false)) {
        exit(EXIT_FAILURE);
    }

    emit commands.sendCommand(c);

    QThread rcv_thread;

    blue.moveToThread(&rcv_thread);
    yellow.moveToThread(&rcv_thread);
    vision.moveToThread(&rcv_thread);
    commands.moveToThread(&rcv_thread);


    rcv_thread.start();

    return app.exec();
}
