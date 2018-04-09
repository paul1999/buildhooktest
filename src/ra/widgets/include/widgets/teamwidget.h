/***************************************************************************
 *   Copyright 2015 Michael Eischer, Philipp Nordhus                       *
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

#ifndef TEAMWIDGET_H
#define TEAMWIDGET_H

#include "protobuf/command.h"
#include "protobuf/status.h"
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QToolButton>
#include <QStringList>

class TeamWidget : public QFrame
{
    Q_OBJECT

public:
    enum TeamType {
        BLUE, YELLOW, AUTOREF
    };

    explicit TeamWidget(QWidget *parent = 0);
    ~TeamWidget() override;

signals:
    void sendCommand(const Command &command);

public:
    void init(TeamType type);
    void load();
    void setRecentScripts(QStringList *recent);
    void forceAutoReload(bool force);
    void shutdown();
    void enableContent(bool enable);

public slots:
    void handleStatus(const Status &status);
    void resendAll(bool send);
    void saveConfig();

private slots:
    void showOpenDialog();
    void open();
    void selectEntryPoint(const QString &entry_point);
    void selectEntryPoint(QAction* action);
    void closeScript();
    void prepareScriptMenu();
    void sendReload();
    void sendAutoReload();
    void sendEnableDebug(bool enable);
    void sendTriggerDebug();

private:
    void open(const QString &filename);
    void setColor(const QColor &color);
    void updateStyleSheet();
    void addEntryPoint(QMenu *menu, const QString &name, const QString &entryPoint);
    QString shortenEntrypointName(const QMenu *menu, const QString &name, int targetLength);
    QString teamTypeName() const;
    amun::CommandStrategy *commandStrategyFromType(const Command &command) const;

private:
    TeamType m_type;
    QToolButton *m_btnOpen;
    QToolButton *m_btnEntryPoint;
    QToolButton *m_btnReload;
    QMenu *m_scriptMenu;
    QMenu *m_entryPoints;
    QString m_filename;
    QString m_entryPoint;
    QAction *m_actionDisable;
    QAction *m_reloadAction;
    QAction *m_enableDebugAction;
    QAction *m_debugAction;
    bool m_userAutoReload;
    bool m_notification;
    QStringList *m_recentScripts;
};

#endif // TEAMWIDGET_H
