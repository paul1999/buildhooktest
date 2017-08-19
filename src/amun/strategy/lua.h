/***************************************************************************
 *   Copyright 2015 Alexander Danzer. Michael Eischer, Philipp Nordhus     *
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

#ifndef LUA_H
#define LUA_H

#include <lua.hpp>
#include <QString>
#include <QStringList>
#include <QDir>
#include "abstractstrategyscript.h"
#include "strategytype.h"

class FileWatcher;
class Lua;
class Timer;

Lua *getStrategyThread(lua_State *state);

class Lua : public AbstractStrategyScript
{
    Q_OBJECT
private:
    Lua(const Timer *timer, StrategyType type, bool debugEnabled, bool refboxControlEnabled);
public:
    static bool canHandle(const QString &filename);
    static AbstractStrategyScript* createStrategy(const Timer *timer, StrategyType type, bool debugEnabled, bool refboxControlEnabled);
    ~Lua() override;

public:
    bool loadScript(const QString &filename, const QString &entryPoint, const world::Geometry &geometry, const robot::Team &team) override;
    bool process(double &pathPlanning, const world::State &worldState, const amun::GameState &refereeState, const amun::UserInput &userInput) override;
    bool triggerDebugger() override;

    const world::Geometry& geometry() const { return m_geometry; }
    const robot::Team& team() const { return m_team; }
    const world::State& worldState() const { return m_worldState; }
    const amun::GameState& refereeState() const { return m_refereeState; }
    const amun::UserInput& userInput() const { return m_userInput; }
    qint64 startTime() const { return m_startTime; }
    qint64 time() const;
    bool isBlue() const { return m_type == StrategyType::BLUE; }
    const QDir baseDir() const { return m_baseDir; }
    bool refboxControlEnabled() const { return m_refboxControlEnabled; }
    void setCommand(uint generation, uint robotId, robot::Command &command);
    void log(const QString &text);
    amun::Visualization *addVisualization();
    void removeVisualizations();
    amun::DebugValue *addDebug();
    amun::PlotValue *addPlot();
    amun::RobotValue *addRobotValue();
    bool sendCommand(const Command &command);
    bool sendNetworkReferee(const QByteArray &referee);
    void sendMixedTeam(const QByteArray &info);
    bool sendAutoref(const QByteArray &event);
    void watch(const QString &filename);
private:
    void loadLibs();
    void loadDebugLibs();
    void loadLib(const char* name, lua_CFunction function);
    void setupPackageLoader();
    void replaceLuaFunction(const char *module, const char *key, lua_CFunction replacement);

private:
    lua_State *m_state;
    FileWatcher *m_watcher;
    const Timer *m_timer;
    const StrategyType m_type;
    const bool m_debugEnabled;
    const bool m_refboxControlEnabled;

    QString m_filename;
    QDir m_baseDir;
    qint64 m_startTime;

    world::Geometry m_geometry;
    robot::Team m_team;
    world::State m_worldState;
    amun::GameState m_refereeState;
    amun::UserInput m_userInput;
};

#endif // LUA_H
