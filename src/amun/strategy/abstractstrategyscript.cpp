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

#include "abstractstrategyscript.h"

AbstractStrategyScript::AbstractStrategyScript() {
    m_hasDebugger = false;
}

bool AbstractStrategyScript::triggerDebugger()
{
    // fail as default
    return false;
}

void AbstractStrategyScript::clearDebug()
{
    m_debug.clear_value();
    m_debug.clear_visualization();
    m_debug.clear_log();
    m_debug.clear_plot();
}

void AbstractStrategyScript::setSelectedOptions(const QStringList &options)
{
    m_selectedOptions = options;
}
