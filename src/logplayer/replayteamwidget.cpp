#include <QSettings>
#include "replayteamwidget.h"
#include "ui_replayteamwidget.h"

ReplayTeamWidget::ReplayTeamWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ReplayTeamWidget)
{
    ui->setupUi(this);
    ui->blue->init(true);
    ui->yellow->init(false);
    QSettings s;
    s.beginGroup("Strategy");
    m_recentScripts = s.value("RecentScripts").toStringList();
    s.endGroup();

    ui->blue->setRecentScripts(&m_recentScripts);
    ui->yellow->setRecentScripts(&m_recentScripts);
    ui->blue->load();
    ui->yellow->load();

    connect(ui->replayBlue, SIGNAL(clicked(bool)), ui->blue, SLOT(setEnabled(bool)));
    connect(ui->replayYellow, SIGNAL(clicked(bool)), ui->yellow, SLOT(setEnabled(bool)));
    connect(ui->replayBlue, SIGNAL(clicked(bool)), this, SIGNAL(enableStrategyBlue(bool)));
    connect(ui->replayYellow, SIGNAL(clicked(bool)), this, SIGNAL(enableStrategyYellow(bool)));
    connect(ui->replayBlue, SIGNAL(clicked(bool)), ui->blue, SLOT(resendAll(bool)));
    connect(ui->replayYellow, SIGNAL(clicked(bool)), ui->yellow, SLOT(resendAll(bool)));

    connect(ui->blue, SIGNAL(sendCommand(Command)), this, SIGNAL(sendCommand(Command)));
    connect(ui->yellow, SIGNAL(sendCommand(Command)), this, SIGNAL(sendCommand(Command)));
    connect(this, SIGNAL(gotStatus(Status)), ui->blue, SLOT(handleStatus(Status)));
    connect(this, SIGNAL(gotStatus(Status)), ui->yellow, SLOT(handleStatus(Status)));
}

ReplayTeamWidget::~ReplayTeamWidget()
{
    QSettings s;
    s.beginGroup("Strategy");
    s.setValue("RecentScripts", m_recentScripts);
    s.endGroup();
    delete ui;
}

bool ReplayTeamWidget::replayBlueEnabled() const
{
    return ui->replayBlue->isChecked();
}

bool ReplayTeamWidget::replayYellowEnabled() const
{
    return ui->replayYellow->isChecked();
}