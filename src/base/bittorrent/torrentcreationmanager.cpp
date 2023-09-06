/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2015, 2018  Vladimir Golovnev <glassez@yandex.ru>
 * Copyright (C) 2006  Christophe Dumez <chris@qbittorrent.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "torrentcreationmanager.h"

#include <QByteArray>
#include <QFile>
#include <QPointer>
#include <QUuid>

using namespace BitTorrent;

QPointer<TorrentCreationManager> TorrentCreationManager::m_instance = nullptr;

TorrentCreationTask::TorrentCreationTask(const QString &id, const BitTorrent::TorrentCreatorParams &params, QObject *parent)
    : QObject(parent)
    , m_id {id}
    , m_params {params}
{
}

void TorrentCreationTask::handleProgress(int progress)
{
    m_started = true;
    m_progress = progress;
}

void TorrentCreationTask::handleFailure(const QString &msg)
{
    m_started = true;
    m_done = true;
    m_errorMsg = msg;
}

void TorrentCreationTask::handleSuccess(const BitTorrent::TorrentCreatorResult &result)
{
    m_started = true;
    m_done = true;
    m_result = result;
    m_params.pieceSize = result.pieceSize;
}

bool TorrentCreationTask::isDoneWithSuccess() const
{
    return m_done && (!m_result.content.isEmpty() || !m_result.path.isEmpty());
}

bool TorrentCreationTask::isDoneWithError() const
{
    return m_done && !m_errorMsg.isEmpty();
}

bool TorrentCreationTask::isRunning() const
{
    return m_started && !m_done;
}

QString TorrentCreationTask::id() const
{
    return m_id;
}

QByteArray TorrentCreationTask::content() const
{
    if (!isDoneWithSuccess())
        return QByteArray {};

    if (!m_result.content.isEmpty())
        return m_result.content;

    QFile file(m_result.path.toString());
    if(!file.open(QIODevice::ReadOnly))
        return QByteArray {};

    return file.readAll();
}

const BitTorrent::TorrentCreatorParams &TorrentCreationTask::params() const
{
    return m_params;
}

int TorrentCreationTask::progress() const
{
    return m_progress;
}

QString TorrentCreationTask::errorMsg() const
{
    return m_errorMsg;
}


TorrentCreationManager::TorrentCreationManager()
    : m_threadPool {this}
{
    Q_ASSERT(!m_instance); // only one instance is allowed
    m_instance = this;
}

TorrentCreationManager::~TorrentCreationManager()
{
    qDeleteAll(m_tasks);
}

TorrentCreationManager *TorrentCreationManager::instance()
{
    if (!m_instance)
        m_instance = new TorrentCreationManager;
    return m_instance;
}

void TorrentCreationManager::freeInstance()
{
    delete m_instance;
}

QString TorrentCreationManager::createTask(const TorrentCreatorParams &params, bool startSeeding)
{
    QString taskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    while (tasksById().contains(taskId))
    {
        taskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    auto *torrentCreator = new TorrentCreator(params, this);
    auto *creationTask = new TorrentCreationTask(taskId, params, this);

    auto onSuccess = [this, startSeeding, taskId](const TorrentCreatorResult &result)
    {
        if (startSeeding)
            result.startSeeding(false);
        TorrentCreationTask * task = getTask(taskId);
        if (task)
        {
            task->handleSuccess(result);
        }
    };

    auto onFailure = [this, taskId](const QString &msg)
    {
        TorrentCreationTask * task = getTask(taskId);
        if (task)
        {
            task->handleFailure(msg);
        }
    };
    connect(creationTask, &QObject::destroyed, torrentCreator, &BitTorrent::TorrentCreator::requestInterruption);
    connect(torrentCreator, &BitTorrent::TorrentCreator::creationSuccess, this, onSuccess);
    connect(torrentCreator, &BitTorrent::TorrentCreator::creationFailure, this, onFailure);
    connect(torrentCreator, &BitTorrent::TorrentCreator::updateProgress, creationTask, &TorrentCreationTask::handleProgress);

    //connect(torrentCreator, &BitTorrent::TorrentCreator::creationSuccess, this, TorrentCreationManager::taskDone);
    //connect(torrentCreator, &BitTorrent::TorrentCreator::creationFailure, this, onFailure);

    tasksById().insert(creationTask);
    m_threadPool.start(torrentCreator);
    return taskId;
}

QStringList TorrentCreationManager::taskIds() const
{
    QStringList ids;
    ids.reserve(tasksById().size());
    for (TorrentCreationTask * task: tasksById())
        ids << task->id();
    return ids;
}

TorrentCreationTask *TorrentCreationManager::getTask(const QString &id) const
{
    const auto iter = tasksById().find(id);
    if (iter == tasksById().end())
        return nullptr;

    return *iter;
}

bool TorrentCreationManager::deleteTask(const QString &id)
{
    const auto iter = tasksById().find(id);
    if (iter == tasksById().end())
        return false;

    delete *iter;
    tasksById().erase(iter);
    return true;
}

void TorrentCreationManager::taskDone(const QString &id)
{
}

const TorrentCreationTasksSetById &TorrentCreationManager::tasksById() const
{
    return m_tasks.get<0>();
}

TorrentCreationTasksSetById &TorrentCreationManager::tasksById()
{
    return m_tasks.get<0>();
}

const TorrentCreationTasksSetByCompletion &TorrentCreationManager::tasksByCompletion() const
{
    return m_tasks.get<1>();
}

TorrentCreationTasksSetByCompletion &TorrentCreationManager::tasksByCompletion()
{
    return m_tasks.get<1>();
}
