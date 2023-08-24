/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2018  Thomas Piccirello <thomas.piccirello@gmail.com>
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

#include "metafilecontroller.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QThreadPool>

#include "base/global.h"
#include "base/bittorrent/torrentcreator.h"
#include "base/utils/string.h"
#include "apierror.h"

const QString KEY_COMMENT = u"comment"_s;
const QString KEY_FORMAT = u"format"_s;
const QString KEY_ID = u"id"_s;
const QString KEY_INPUT_PATH = u"inputPath"_s;
const QString KEY_OPTIMIZE_ALIGNMENT = u"optimizeAlignment"_s;
const QString KEY_PADDED_FILE_SIZE_LIMIT = u"paddedFileSizeLimit"_s;
const QString KEY_PIECE_SIZE = u"pieceSize"_s;
const QString KEY_PRIVATE = u"private"_s;
const QString KEY_SAVE_PATH = u"savePath"_s;
const QString KEY_SOURCE = u"source"_s;
const QString KEY_TRACKERS = u"trackers"_s;
const QString KEY_URL_SEEDS = u"urlSeeds"_s;

namespace
{
    using Utils::String::parseBool;
    using Utils::String::parseInt;
    using Utils::String::parseDouble;
}

TorrentCreationTask::TorrentCreationTask(BitTorrent::TorrentCreatorParams &params, QObject *parent)
    : QObject(parent)
    , m_params(params)
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
    m_content = result.content;
    m_params.pieceSize = result.pieceSize;
}

bool TorrentCreationTask::isDoneWithSuccess() const
{
    return m_done && !m_content.isEmpty();
}

bool TorrentCreationTask::isDoneWithError() const
{
    return m_done && (!m_errorMsg.isEmpty() || m_content.isEmpty());
}

bool TorrentCreationTask::isRunning() const
{
    return m_started && !m_done;
}

const QByteArray &TorrentCreationTask::content() const
{
    return m_content;
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

void MetafileController::createAction()
{
    requireParams({u"inputPath"_s});

    BitTorrent::TorrentCreatorParams createTorrentParams;
    createTorrentParams.isPrivate = parseBool(params()[KEY_PRIVATE]).value_or(false);
#ifdef QBT_USES_LIBTORRENT2
    const QString format = params()[KEY_FORMAT].toLower();
    if (format == u"v1"_s)
        createTorrentParams.torrentFormat = BitTorrent::TorrentFormat::V1;
    else if (format == u"v2"_s)
        createTorrentParams.torrentFormat = BitTorrent::TorrentFormat::V2;
    else
        createTorrentParams.torrentFormat = BitTorrent::TorrentFormat::Hybrid;
#else
    createTorrentParams.isAlignmentOptimized = parseBool(params()[KEY_OPTIMIZE_ALIGNMENT]).value_or(true);
    createTorrentParams.paddedFileSizeLimit = parseInt(params()[KEY_PADDED_FILE_SIZE_LIMIT]).value_or(-1);
#endif
    createTorrentParams.pieceSize = parseInt(params()[KEY_PIECE_SIZE]).value_or(0);
    createTorrentParams.inputPath = Path {params()[KEY_INPUT_PATH]};
    createTorrentParams.savePath = Path {params()[KEY_SAVE_PATH]};
    createTorrentParams.comment = params()[KEY_COMMENT];
    createTorrentParams.source = params()[KEY_COMMENT];
    createTorrentParams.trackers = params()[KEY_TRACKERS].split(u'|');
    createTorrentParams.urlSeeds = params()[KEY_URL_SEEDS].split(u'|');

    auto *torrentCreator = new BitTorrent::TorrentCreator(createTorrentParams, true, this);
    auto *creationTask = new TorrentCreationTask(createTorrentParams, this);
    connect(creationTask, &QObject::destroyed, torrentCreator, &BitTorrent::TorrentCreator::requestInterruption);
    connect(torrentCreator, &BitTorrent::TorrentCreator::creationSuccess, creationTask, &TorrentCreationTask::handleSuccess);
    connect(torrentCreator, &BitTorrent::TorrentCreator::creationFailure, creationTask, &TorrentCreationTask::handleFailure);
    connect(torrentCreator, &BitTorrent::TorrentCreator::updateProgress, creationTask, &TorrentCreationTask::handleProgress);

    QString taskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    while (m_tasks.contains(taskId)){
        taskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    m_tasks[taskId] = creationTask;

    QThreadPool::globalInstance()->start(torrentCreator);

    setResult(QJsonObject {{KEY_ID, taskId}});
}

void MetafileController::deleteAction()
{
    requireParams({KEY_ID});
    const QString id = params()[KEY_ID];

    const auto iter = m_tasks.find(id);
    if (iter == m_tasks.end())
        throw APIError(APIErrorType::NotFound);

    delete iter.value();
    m_tasks.erase(iter);
}

void MetafileController::statusAction()
{
    const QString id = params()[KEY_ID];

    if (!id.isEmpty() && !m_tasks.contains(id))
        throw APIError(APIErrorType::NotFound);

    QJsonArray statusArray;
    const QList<QString> ids{id.isEmpty() ? m_tasks.keys() : QList<QString> {id}};

    for (const QString &task_id: ids){
        TorrentCreationTask * task = m_tasks[task_id];
        QJsonObject taskJson {
            {KEY_ID, task_id},
            {KEY_INPUT_PATH, task->params().inputPath.toString()},
            {KEY_PRIVATE, task->params().isPrivate},
        };

        if (!task->params().comment.isEmpty())
            taskJson[KEY_COMMENT] = task->params().comment;

        if (task->params().pieceSize)
            taskJson[KEY_PIECE_SIZE] = task->params().pieceSize;

        if (!task->params().savePath.isEmpty())
            taskJson[KEY_SAVE_PATH] = task->params().savePath.toString();

        if (!task->params().source.isEmpty())
            taskJson[KEY_SOURCE] = task->params().source;

        if (!task->params().trackers.isEmpty())
            taskJson[KEY_TRACKERS] = QJsonArray::fromStringList(task->params().trackers);

        if (!task->params().urlSeeds.isEmpty())
            taskJson[KEY_URL_SEEDS] = QJsonArray::fromStringList(task->params().urlSeeds);

        if (task->isDoneWithError())
        {
            taskJson[u"status"_s] = u"Error"_s;
            taskJson[u"error_msg"_s] = task->errorMsg();
        }
        else if (task->isDoneWithSuccess())
        {
            taskJson[u"status"_s] = u"Done"_s;
        }
        else if (task->isRunning())
        {
            taskJson[u"status"_s] = u"Processing"_s;
            taskJson[u"progress"_s] = task->progress();
        }
        else
        {
            taskJson[u"status"_s] = u"Pending"_s;
        }

#ifdef QBT_USES_LIBTORRENT2
        switch (task->params().torrentFormat)
        {
            case BitTorrent::TorrentFormat::V1:
                taskJson[KEY_FORMAT] = u"v1"_s;
                break;
            case BitTorrent::TorrentFormat::V2:
                taskJson[KEY_FORMAT] = u"v2"_s;
                break;
            default:
                taskJson[KEY_FORMAT] = u"hybrid"_s;
        }
#else
        taskJson[KEY_OPTIMIZE_ALIGNMENT] = task->params().isAlignmentOptimized;
        taskJson[KEY_PADDED_FILE_SIZE_LIMIT] = task->params().paddedFileSizeLimit;
#endif

        statusArray << taskJson;
    }

    setResult(statusArray);
}

void MetafileController::getFileAction()
{
    requireParams({KEY_ID});
    const QString id = params()[KEY_ID];

    const auto iter = m_tasks.find(id);
    if (iter == m_tasks.end())
        throw APIError(APIErrorType::NotFound);

    TorrentCreationTask * task = iter.value();
    setResult(task->content());
}
